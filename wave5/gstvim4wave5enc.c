/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "devices.h"
#include "nv12-planes.h"

typedef struct {
    GstBin parent;
    GstElement *planes, *encoder, *encoded_caps;
    GstPad *sink, *src;
    gint bitrate, gop, framerate;
    guint64 key_interval;
} GstVim4Wave5Enc;
typedef struct { GstBinClass parent; } GstVim4Wave5EncClass;
G_DEFINE_TYPE(GstVim4Wave5Enc, gst_vim4_wave5_enc, GST_TYPE_BIN)
enum { PROP_0, PROP_BITRATE, PROP_GOP, PROP_FRAMERATE, PROP_KEY_INTERVAL };

static void set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
    GstVim4Wave5Enc *self = (GstVim4Wave5Enc *)object;
    if (id == PROP_BITRATE) self->bitrate = g_value_get_int(value);
    else if (id == PROP_GOP) self->gop = g_value_get_int(value);
    else if (id == PROP_FRAMERATE) self->framerate = g_value_get_int(value);
    else if (id == PROP_KEY_INTERVAL) self->key_interval = g_value_get_uint64(value);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
    GstVim4Wave5Enc *self = (GstVim4Wave5Enc *)object;
    if (id == PROP_BITRATE) g_value_set_int(value, self->bitrate);
    else if (id == PROP_GOP) g_value_set_int(value, self->gop);
    else if (id == PROP_FRAMERATE) g_value_set_int(value, self->framerate);
    else if (id == PROP_KEY_INTERVAL) g_value_set_uint64(value, self->key_interval);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static GstPadProbeReturn cache_bitstream(GstPad *pad, GstPadProbeInfo *info, gpointer unused)
{
    (void)pad; (void)unused;
    GstBuffer *original = GST_PAD_PROBE_INFO_BUFFER(info);
    /* Wave5 compressed DMA memory is coherent but expensive for NAL scans. */
    GstBuffer *cached = gst_buffer_copy_deep(original);
    if (!cached) return GST_PAD_PROBE_DROP;
    GST_PAD_PROBE_INFO_DATA(info) = cached;
    gst_buffer_unref(original);
    return GST_PAD_PROBE_OK;
}

static void clear_children(GstVim4Wave5Enc *self)
{
    gst_ghost_pad_set_target(GST_GHOST_PAD(self->sink), NULL);
    gst_ghost_pad_set_target(GST_GHOST_PAD(self->src), NULL);
    GstElement **children[] = {&self->planes, &self->encoder, &self->encoded_caps};
    for (guint i = 0; i < G_N_ELEMENTS(children); i++) {
        if (*children[i]) {
            gst_element_set_state(*children[i], GST_STATE_NULL);
            gst_bin_remove(GST_BIN(self), *children[i]);
            *children[i] = NULL;
        }
    }
}

static gboolean configure(GstVim4Wave5Enc *self)
{
    GstCaps *allowed = gst_pad_peer_query_caps(self->src, NULL);
    gboolean h264 = FALSE, h265 = FALSE;
    for (guint i = 0; allowed && i < gst_caps_get_size(allowed); i++) {
        const gchar *name = gst_structure_get_name(gst_caps_get_structure(allowed, i));
        h264 |= !strcmp(name, "video/x-h264");
        h265 |= !strcmp(name, "video/x-h265");
    }
    if (allowed) gst_caps_unref(allowed);
    if (!h264 && !h265) {
        GST_ELEMENT_ERROR(self, CORE, NEGOTIATION, ("Select H.264 or H.265 downstream"), (NULL));
        return FALSE;
    }
    gboolean hevc = h265 && !h264;
    gchar *device = vim4_encoder_device();
    if (!device) {
        GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND, ("Expected one Wave5 encoder device"), (NULL));
        return FALSE;
    }
    self->planes = g_object_new(gst_vim4_nv12_planes_get_type(), NULL);
    self->encoder = gst_element_factory_make(hevc ? "v4l2h265enc" : "v4l2h264enc", "wave5");
    self->encoded_caps = gst_element_factory_make("capsfilter", "wave5_format");
    if (self->planes) gst_bin_add(GST_BIN(self), self->planes);
    if (self->encoder) gst_bin_add(GST_BIN(self), self->encoder);
    if (self->encoded_caps) gst_bin_add(GST_BIN(self), self->encoded_caps);
    gchar *selected = NULL;
    if (self->encoder) g_object_get(self->encoder, "device", &selected, NULL);
    gboolean matched = selected && !strcmp(selected, device);
    g_free(selected); g_free(device);
    if (!matched || !self->planes || !self->encoded_caps) {
        GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
            ("The matching V4L2 encoder factory is missing or selected another device"), (NULL));
        clear_children(self);
        return FALSE;
    }
    GstStructure *controls = gst_structure_new("controls",
        "video_bitrate", G_TYPE_INT, self->bitrate * 1000,
        "video_gop_size", G_TYPE_INT, self->gop,
        "intra_refresh_period", G_TYPE_INT, self->gop ? 0 : 45,
        "frame_level_rate_control_enable", G_TYPE_INT, 1,
        "h264_mb_level_rate_control", G_TYPE_INT, 1, NULL);
    if (hevc) gst_structure_set(controls, "hevc_level", G_TYPE_INT, 8, NULL);
    g_object_set(self->encoder, "output-io-mode", 5, "extra-controls", controls,
        "min-force-key-unit-interval", self->key_interval, NULL);
    gst_structure_free(controls);
    GstCaps *caps = hevc ? gst_caps_from_string("video/x-h265,profile=main,level=(string)5.1") :
        gst_caps_from_string("video/x-h264");
    g_object_set(self->encoded_caps, "caps", caps, NULL);
    gst_caps_unref(caps);
    if (!gst_element_link_many(self->planes, self->encoder, self->encoded_caps, NULL)) {
        GST_ELEMENT_ERROR(self, CORE, NEGOTIATION, ("Cannot link Wave5 plane import and encoder"), (NULL));
        clear_children(self);
        return FALSE;
    }
    GstPad *pad = gst_element_get_static_pad(self->planes, "sink");
    gst_ghost_pad_set_target(GST_GHOST_PAD(self->sink), pad);
    gst_object_unref(pad);
    pad = gst_element_get_static_pad(self->encoded_caps, "src");
    gst_ghost_pad_set_target(GST_GHOST_PAD(self->src), pad);
    gst_object_unref(pad);
    pad = gst_element_get_static_pad(self->encoder, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, cache_bitstream, NULL, NULL);
    gst_object_unref(pad);
    return TRUE;
}

static GstStateChangeReturn change_state(GstElement *element, GstStateChange transition)
{
    GstVim4Wave5Enc *self = (GstVim4Wave5Enc *)element;
    if (transition == GST_STATE_CHANGE_NULL_TO_READY && !configure(self))
        return GST_STATE_CHANGE_FAILURE;
    GstStateChangeReturn result = GST_ELEMENT_CLASS(gst_vim4_wave5_enc_parent_class)->change_state(element, transition);
    if (transition == GST_STATE_CHANGE_READY_TO_NULL ||
        (transition == GST_STATE_CHANGE_NULL_TO_READY && result == GST_STATE_CHANGE_FAILURE))
        clear_children(self);
    return result;
}

static void gst_vim4_wave5_enc_class_init(GstVim4Wave5EncClass *klass)
{
    GObjectClass *object = G_OBJECT_CLASS(klass);
    object->set_property = set_property; object->get_property = get_property;
    const guint flags = G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS;
    g_object_class_install_property(object, PROP_BITRATE, g_param_spec_int("bitrate", "Bitrate",
        "Target bitrate in kbps", 1000, 60000, 12000, flags));
    g_object_class_install_property(object, PROP_GOP, g_param_spec_int("gop", "GOP",
        "Keyframe period; zero uses cyclic refresh", 0, 10000, 0, flags));
    g_object_class_install_property(object, PROP_FRAMERATE, g_param_spec_int("framerate", "Frame rate",
        "Compatibility budget hint; actual timing follows negotiated capture caps", 0, 120, 0, flags));
    g_object_class_install_property(object, PROP_KEY_INTERVAL, g_param_spec_uint64("min-force-key-unit-interval",
        "Keyframe request interval", "Minimum interval between recovery keyframes in ns", 0, G_MAXUINT64, 250000000, flags));
    GstElementClass *element = GST_ELEMENT_CLASS(klass);
    element->change_state = change_state;
    GstCaps *caps = gst_caps_from_string("video/x-raw,format=NV12,width=[2,4096],height=[2,2160]");
    gst_element_class_add_pad_template(element, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
    gst_caps_unref(caps);
    caps = gst_caps_from_string("video/x-h264;video/x-h265");
    gst_element_class_add_pad_template(element, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
    gst_caps_unref(caps);
    gst_element_class_set_static_metadata(element, "VIM4 Wave5 hardware encoder", "Codec/Encoder/Video/Hardware",
        "V4L2 Wave5 with leased DMA plane views and cached compressed output", "vim4-kvm");
}

static void gst_vim4_wave5_enc_init(GstVim4Wave5Enc *self)
{
    self->bitrate = 12000; self->key_interval = 250000000;
    GstElementClass *klass = GST_ELEMENT_GET_CLASS(self);
    self->sink = gst_ghost_pad_new_no_target_from_template("sink", gst_element_class_get_pad_template(klass, "sink"));
    self->src = gst_ghost_pad_new_no_target_from_template("src", gst_element_class_get_pad_template(klass, "src"));
    gst_element_add_pad(GST_ELEMENT(self), self->sink);
    gst_element_add_pad(GST_ELEMENT(self), self->src);
}
