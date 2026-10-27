/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <gst/video/gstvideoencoder.h>
#include <gst/video/video.h>
#include <gst/allocators/gstdmabuf.h>
#include "vp_multi_codec_1_0.h"

GType gst_vim4_h265_src_get_type(void);

#ifndef VIM4_HDR_LIBRARY
#define VIM4_HDR_LIBRARY "/usr/lib/vim4-kvm/hdr/libvpcodec.so"
#endif

/* The extended encoder ABI differs from the system's eight-bit codec.
 * A separate namespace keeps its VPU symbols and dependencies private even
 * when an SDR encoder has already loaded the system library in this process.
 * Retain one namespace for the process, rather than allocating one per stream. */
typedef struct {
    void *library;
    vl_codec_handle_t (*init)(vl_codec_id_t, vl_encode_info_t, qp_param_t *);
    encoding_metadata_t (*encode)(vl_codec_handle_t, vl_frame_type_t,
                                  unsigned char *, vl_buffer_info_t *, vl_buffer_info_t *);
    int (*destroy)(vl_codec_handle_t);
} CodecAPI;

static CodecAPI api;
static gsize api_once;

static gboolean load_api(void)
{
    if (g_once_init_enter(&api_once)) {
        api.library = dlmopen(LM_ID_NEWLM, VIM4_HDR_LIBRARY, RTLD_NOW | RTLD_LOCAL);
        if (api.library) {
            api.init = dlsym(api.library, "vl_multi_encoder_init");
            api.encode = dlsym(api.library, "vl_multi_encoder_encode");
            api.destroy = dlsym(api.library, "vl_multi_encoder_destroy");
        }
        g_once_init_leave(&api_once, 1);
    }
    return api.init && api.encode && api.destroy;
}

typedef struct {
    guint64 frames, dma_inputs, cpu_inputs, encode_us, encode_max_us;
    guint pending_peak;
} Main10Timing;

typedef struct {
    GstVideoEncoder parent;
    vl_codec_handle_t encoder;
    GstVideoInfo info;
    guint bitrate, framerate;
    guint8 *output;
    /* A packet can arrive before the SDK retires the input DMA buffer. */
    GHashTable *pending_dma;
    gboolean timing;
    Main10Timing timing_stats;
} GstVim4Hevc10;
typedef GstVideoEncoderClass GstVim4Hevc10Class;
G_DEFINE_TYPE(GstVim4Hevc10, gst_vim4_hevc10, GST_TYPE_VIDEO_ENCODER)

enum { PROP_0, PROP_BITRATE, PROP_FRAMERATE };
#define OUTPUT_BYTES (16u * 1024 * 1024)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS(
    "video/x-raw,format=P010_10LE,width=[64,4096],height=[64,2160],framerate=[0/1,120/1]"));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE("src",
    GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS(
    "video/x-h265,stream-format=byte-stream,alignment=au,profile=main-10"));

static void close_encoder(GstVim4Hevc10 *self)
{
    if (self->encoder) {
        api.destroy(self->encoder);
        self->encoder = 0;
    }
    /* Destroy first: the hardware must no longer own any outstanding fd. */
    g_hash_table_remove_all(self->pending_dma);
    g_clear_pointer(&self->output, g_free);
    if (self->timing)
        self->timing_stats = (Main10Timing){0};
}

static gboolean set_format(GstVideoEncoder *base, GstVideoCodecState *input)
{
    GstVim4Hevc10 *self = (GstVim4Hevc10 *)base;
    GstVideoInfo *v = &input->info;
    close_encoder(self);
    if (!load_api()) {
        GST_ELEMENT_ERROR(self, LIBRARY, INIT,
            ("Private Main10 encoder is unavailable"), ("%s", dlerror()));
        return FALSE;
    }
    if ((v->width & 1) || (v->height & 1) || v->stride[0] != v->stride[1])
        return FALSE;
    /* The board's vendor GStreamer adds a transfer enum before SMPTE2084.
     * Convert through the runtime ABI instead of comparing build-host enums. */
    guint transfer = gst_video_transfer_function_to_iso(v->colorimetry.transfer);
    gboolean pq = transfer == 16;
    gboolean sdr = transfer == 1 || transfer == 6 || transfer == 13;
    if ((!pq && !sdr) || (sdr && (v->colorimetry.primaries != GST_VIDEO_COLOR_PRIMARIES_BT709 ||
                                v->colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_BT709)) ||
        (pq && (v->colorimetry.primaries != GST_VIDEO_COLOR_PRIMARIES_BT2020 ||
                               v->colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_BT2020))) {
        GST_ELEMENT_ERROR(self, STREAM, FORMAT,
            ("Main10 capture requires PQ/BT.2020 or SDR/BT.709 colorimetry"),
            ("range=%d matrix=%d transfer=%d primaries=%d; caps=%" GST_PTR_FORMAT,
             v->colorimetry.range, v->colorimetry.matrix,
             v->colorimetry.transfer, v->colorimetry.primaries, input->caps));
        return FALSE;
    }
    vl_encode_info_t config = {
        .width = v->width, .height = v->height,
        .frame_rate = self->framerate, .bit_rate = self->bitrate * 1000,
        .gop = 0, .img_format = IMG_FMT_P010, .internal_bit_depth = 10,
        .prepend_spspps_to_idr_frames = true,
        .intra_refresh_mode = 1, .intra_refresh_arg = 1,
        .vui_parameters_present_flag = 1, .video_signal_type_present_flag = 1,
        .colour_description_present_flag = 1,
        .video_full_range_flag = v->colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255,
        .colour_primaries = pq ? 9 : 1,
        .transfer_characteristics = transfer,
        .matrix_coefficients = pq ? 9 : 1,
    };
    qp_param_t qp = {.qp_min=0, .qp_max=51, .qp_I_base=30,
        .qp_I_min=0, .qp_I_max=51, .qp_P_base=30, .qp_P_min=0, .qp_P_max=51};
    self->encoder = api.init(CODEC_ID_H265, config, &qp);
    if (!self->encoder) {
        GST_ELEMENT_ERROR(self, LIBRARY, INIT, ("Main10 encoder initialization failed"), (NULL));
        return FALSE;
    }
    self->info = *v;
    self->output = g_malloc(OUTPUT_BYTES);
    GstCaps *caps = gst_caps_from_string(
        "video/x-h265,stream-format=byte-stream,alignment=au,profile=main-10");
    GstVideoCodecState *state = gst_video_encoder_set_output_state(base, caps, input);
    gst_video_codec_state_unref(state);
    return gst_video_encoder_negotiate(base);
}

static gboolean prepare_dma_input(GstVim4Hevc10 *self, GstBuffer *buffer,
                                  vl_buffer_info_t *input)
{
    if (gst_buffer_n_memory(buffer) != 1)
        return FALSE;
    GstMemory *memory = gst_buffer_peek_memory(buffer, 0);
    if (!gst_is_dmabuf_memory(memory))
        return FALSE;

    GstVideoMeta *meta = gst_buffer_get_video_meta(buffer);
    if (meta && (meta->format != GST_VIDEO_FORMAT_P010_10LE ||
                 meta->width != (guint)self->info.width ||
                 meta->height != (guint)self->info.height || meta->n_planes != 2))
        return FALSE;
    gint stride = meta ? meta->stride[0] : self->info.stride[0];
    gint uv_stride = meta ? meta->stride[1] : self->info.stride[1];
    gsize y_offset = meta ? meta->offset[0] : self->info.offset[0];
    gsize uv_offset = meta ? meta->offset[1] : self->info.offset[1];
    gsize memory_offset;
    gsize size = gst_memory_get_sizes(memory, &memory_offset, NULL);
    /* The SDK imports an fd, not an fd + offset. Its single-fd P010 path
     * derives UV from pitch*height, and requires a 16-byte DMA pitch. */
    if (stride < self->info.width * 2 || (stride & 15) || stride != uv_stride ||
        memory_offset || y_offset || uv_offset != (gsize)stride * self->info.height ||
        size < uv_offset + (gsize)stride * (self->info.height / 2))
        return FALSE;
    input->buf_type = DMA_TYPE;
    input->buf_fmt = IMG_FMT_P010;
    input->buf_stride = stride;
    input->buf_info.dma_info.num_planes = 1;
    input->buf_info.dma_info.shared_fd[0] = gst_dmabuf_memory_get_fd(memory);
    return input->buf_info.dma_info.shared_fd[0] >= 0;
}

static gboolean retire_dma_input(GstVim4Hevc10 *self, const vl_buffer_info_t *returned)
{
    if (returned->buf_type != DMA_TYPE || !returned->buf_info.dma_info.num_planes)
        return TRUE;
    /* This plugin submits exactly one fd per input, including both planes. */
    gint fd = returned->buf_info.dma_info.shared_fd[0];
    if (returned->buf_info.dma_info.num_planes != 1 ||
        !g_hash_table_remove(self->pending_dma, GINT_TO_POINTER(fd))) {
        GST_ELEMENT_ERROR(self, STREAM, ENCODE,
            ("Main10 encoder returned an unknown DMA input"), ("fd=%d planes=%u", fd,
             returned->buf_info.dma_info.num_planes));
        return FALSE;
    }
    return TRUE;
}

static void record_timing(GstVim4Hevc10 *self, vl_buffer_type_t input_type,
                          guint pending_before_encode, gint64 encode_us)
{
    Main10Timing *stats = &self->timing_stats;
    stats->frames++;
    if (input_type == DMA_TYPE)
        stats->dma_inputs++;
    else
        stats->cpu_inputs++;
    stats->pending_peak = MAX(stats->pending_peak, pending_before_encode);
    stats->encode_us += encode_us;
    stats->encode_max_us = MAX(stats->encode_max_us, (guint64)encode_us);
    if (stats->frames % 120 == 0)
        g_printerr("vim4hevc10 timing frames=%" G_GUINT64_FORMAT
            " dma_inputs=%" G_GUINT64_FORMAT " cpu_inputs=%" G_GUINT64_FORMAT
            " pending_peak=%u pending=%u encode_ms=%.3f encode_max_ms=%.3f\n",
            stats->frames, stats->dma_inputs, stats->cpu_inputs,
            stats->pending_peak, g_hash_table_size(self->pending_dma),
            stats->encode_us / (double)stats->frames / 1000.0,
            stats->encode_max_us / 1000.0);
}

static GstFlowReturn handle_frame(GstVideoEncoder *base, GstVideoCodecFrame *frame)
{
    GstVim4Hevc10 *self = (GstVim4Hevc10 *)base;
    GstVideoFrame mapped;
    gboolean cpu_mapped = FALSE;
    vl_buffer_info_t input = {0};
    if (!self->encoder)
        goto fail;
    if (prepare_dma_input(self, frame->input_buffer, &input)) {
        gint fd = input.buf_info.dma_info.shared_fd[0];
        if (g_hash_table_contains(self->pending_dma, GINT_TO_POINTER(fd))) {
            GST_ELEMENT_ERROR(self, STREAM, ENCODE,
                ("Main10 DMA input was reused before retirement"), ("fd=%d", fd));
            goto fail;
        }
        /* Keep the pool slot and fd alive even after finish_frame releases
         * the codec frame. No CPU mapping/cache round trip is needed here. */
        g_hash_table_insert(self->pending_dma, GINT_TO_POINTER(fd),
                            gst_buffer_ref(frame->input_buffer));
    } else {
        if (!gst_video_frame_map(&mapped, &self->info, frame->input_buffer, GST_MAP_READ))
            goto fail;
        cpu_mapped = TRUE;
        /* The CPU path also derives UV from pitch*height. */
        if (GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, 0) != GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, 1) ||
            (guint8 *)GST_VIDEO_FRAME_PLANE_DATA(&mapped, 1) !=
            (guint8 *)GST_VIDEO_FRAME_PLANE_DATA(&mapped, 0) +
            (gsize)GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, 0) * self->info.height) {
            GST_ELEMENT_ERROR(self, STREAM, FORMAT,
                ("Main10 CPU input requires contiguous P010 planes"), (NULL));
            goto fail;
        }
        input.buf_type = VMALLOC_TYPE;
        input.buf_fmt = IMG_FMT_P010;
        /* The extended API takes pitch in bytes, including for P010. */
        input.buf_stride = GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, 0);
        input.buf_info.in_ptr[0] = (unsigned long)GST_VIDEO_FRAME_PLANE_DATA(&mapped, 0);
        input.buf_info.in_ptr[1] = (unsigned long)GST_VIDEO_FRAME_PLANE_DATA(&mapped, 1);
    }
    vl_buffer_info_t returned = {0};
    guint pending_before_encode = 0;
    gint64 encode_begin = 0;
    if (self->timing) {
        pending_before_encode = g_hash_table_size(self->pending_dma);
        encode_begin = g_get_monotonic_time();
    }
    encoding_metadata_t result = api.encode(self->encoder,
        GST_VIDEO_CODEC_FRAME_IS_FORCE_KEYFRAME(frame) ? FRAME_TYPE_IDR : FRAME_TYPE_AUTO,
        self->output, &input, &returned);
    gint64 encode_us = self->timing ? g_get_monotonic_time() - encode_begin : 0;
    if (cpu_mapped) {
        gst_video_frame_unmap(&mapped);
        cpu_mapped = FALSE;
    }
    if (!retire_dma_input(self, &returned))
        goto fail;
    if (!result.is_valid || result.encoded_data_length_in_bytes <= 0 ||
        (guint)result.encoded_data_length_in_bytes > OUTPUT_BYTES) {
        GST_ELEMENT_ERROR(self, STREAM, ENCODE,
            ("Main10 encoding failed"), ("status=%d bytes=%d", result.err_cod,
                                       result.encoded_data_length_in_bytes));
        goto fail;
    }
    if (self->timing)
        record_timing(self, input.buf_type, pending_before_encode, encode_us);
    frame->output_buffer = gst_video_encoder_allocate_output_buffer(base, result.encoded_data_length_in_bytes);
    if (!frame->output_buffer)
        goto fail;
    gst_buffer_fill(frame->output_buffer, 0, self->output, result.encoded_data_length_in_bytes);
    if (result.is_key_frame)
        GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT(frame);
    return gst_video_encoder_finish_frame(base, frame);
fail:
    if (cpu_mapped)
        gst_video_frame_unmap(&mapped);
    /* On error the API may not report the accepted input back. Stop hardware
     * before releasing any remaining DMA pool slots. */
    close_encoder(self);
    gst_video_codec_frame_unref(frame);
    return GST_FLOW_ERROR;
}

static gboolean stop(GstVideoEncoder *base)
{
    close_encoder((GstVim4Hevc10 *)base);
    return TRUE;
}

static void set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
    GstVim4Hevc10 *self = (GstVim4Hevc10 *)object;
    switch (id) {
    case PROP_BITRATE: self->bitrate = g_value_get_uint(value); break;
    case PROP_FRAMERATE: self->framerate = g_value_get_uint(value); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    }
}

static void get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
    GstVim4Hevc10 *self = (GstVim4Hevc10 *)object;
    switch (id) {
    case PROP_BITRATE: g_value_set_uint(value, self->bitrate); break;
    case PROP_FRAMERATE: g_value_set_uint(value, self->framerate); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    }
}

static void gst_vim4_hevc10_init(GstVim4Hevc10 *self)
{
    self->bitrate = 4000;
    self->framerate = 60;
    self->timing = g_strcmp0(g_getenv("VIM4_VIDEO_TIMING"), "1") == 0;
    self->pending_dma = g_hash_table_new_full(g_direct_hash, g_direct_equal,
        NULL, (GDestroyNotify)gst_buffer_unref);
}

static void finalize(GObject *object)
{
    GstVim4Hevc10 *self = (GstVim4Hevc10 *)object;
    close_encoder(self);
    g_hash_table_unref(self->pending_dma);
    G_OBJECT_CLASS(gst_vim4_hevc10_parent_class)->finalize(object);
}

static void gst_vim4_hevc10_class_init(GstVim4Hevc10Class *klass)
{
    GObjectClass *object = G_OBJECT_CLASS(klass);
    GstElementClass *element = GST_ELEMENT_CLASS(klass);
    GstVideoEncoderClass *encoder = GST_VIDEO_ENCODER_CLASS(klass);
    object->set_property = set_property;
    object->get_property = get_property;
    object->finalize = finalize;
    g_object_class_install_property(object, PROP_BITRATE, g_param_spec_uint(
        "bitrate", "Bitrate", "Target kbps; set before encoding", 1000, 60000, 4000,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY));
    g_object_class_install_property(object, PROP_FRAMERATE, g_param_spec_uint(
        "framerate", "Framerate", "Rate-control fps; set before encoding", 1, 120, 60,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY));
    gst_element_class_set_static_metadata(element, "VIM4 HEVC Main10 encoder", "Codec/Encoder/Video",
        "Preserves ten-bit samples and PQ colorimetry", "vim4-kvm");
    gst_element_class_add_static_pad_template(element, &sink_template);
    gst_element_class_add_static_pad_template(element, &src_template);
    encoder->set_format = set_format;
    encoder->handle_frame = handle_frame;
    encoder->stop = stop;
}

static gboolean plugin_init(GstPlugin *plugin)
{
    return gst_element_register(plugin, "vim4hevc10enc", GST_RANK_NONE, gst_vim4_hevc10_get_type()) &&
        gst_element_register(plugin, "vim4h265src", GST_RANK_NONE, gst_vim4_h265_src_get_type());
}
#define PACKAGE "vim4-hevc10"
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, vim4hevc10, "VIM4 HEVC Main10",
    plugin_init, "1.0", "LGPL", PACKAGE, "https://github.com/vim4-kvm/gst-plugin-venc")
