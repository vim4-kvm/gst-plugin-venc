/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "nv12-planes.h"
#include <gst/allocators/gstdmabuf.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include <sys/stat.h>
#include <unistd.h>

GstBuffer *vim4_nv12_plane_view(GstBuffer *original, guint width, guint height)
{
    if (!width || !height || width > 4096 || height > 2160 ||
        (width & 1) || (height & 1) || gst_buffer_n_memory(original) != 1)
        return NULL;
    gsize luma = (gsize)width * height;
    gsize aligned = (gsize)width * ((height + 15) & ~15u);
    GstMemory *memory = gst_buffer_peek_memory(original, 0);
    gsize offset;
    if (gst_buffer_get_size(original) != luma * 3 / 2 ||
        !gst_is_dmabuf_memory(memory) ||
        (gst_memory_get_sizes(memory, &offset, NULL), offset != 0))
        return NULL;
    GstMemory *padded = NULL;
    if (aligned != luma) {
        int fd = gst_dmabuf_memory_get_fd(memory);
        struct stat info;
        if (fstat(fd, &info) || info.st_size < (off_t)(luma + aligned / 2))
            return NULL;
        fd = dup(fd);
        if (fd < 0) return NULL;
        GstAllocator *allocator = gst_dmabuf_allocator_new();
        padded = gst_dmabuf_allocator_alloc(allocator, fd, info.st_size);
        gst_object_unref(allocator);
        if (!padded) { close(fd); return NULL; }
        memory = padded;
    }
    GstMemory *y = gst_memory_share(memory, 0, aligned);
    GstMemory *uv = gst_memory_share(memory, luma, aligned / 2);
    if (padded) gst_memory_unref(padded);
    if (!y || !uv) {
        if (y) gst_memory_unref(y);
        if (uv) gst_memory_unref(uv);
        return NULL;
    }
    GstBuffer *view = gst_buffer_new();
    gst_buffer_append_memory(view, y);
    gst_buffer_append_memory(view, uv);
    /* Memory references alone do not retain the capture pool's buffer lease. */
    gst_buffer_add_parent_buffer_meta(view, original);
    gsize offsets[GST_VIDEO_MAX_PLANES] = {0, aligned};
    gint strides[GST_VIDEO_MAX_PLANES] = {(gint)width, (gint)width};
    gst_buffer_add_video_meta_full(view, GST_VIDEO_FRAME_FLAG_NONE,
        GST_VIDEO_FORMAT_NV12, width, height, 2, offsets, strides);
    gst_buffer_copy_into(view, original, GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_FLAGS, 0, -1);
    return view;
}

typedef struct { GstBaseTransform parent; guint width, height; } GstVim4Nv12Planes;
typedef struct { GstBaseTransformClass parent; } GstVim4Nv12PlanesClass;
G_DEFINE_TYPE(GstVim4Nv12Planes, gst_vim4_nv12_planes, GST_TYPE_BASE_TRANSFORM)

static gboolean set_caps(GstBaseTransform *base, GstCaps *input, GstCaps *output)
{
    (void)output;
    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, input) || GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12)
        return FALSE;
    GstVim4Nv12Planes *self = (GstVim4Nv12Planes *)base;
    self->width = info.width; self->height = info.height;
    return TRUE;
}

static GstFlowReturn prepare(GstBaseTransform *base, GstBuffer *input, GstBuffer **output)
{
    GstVim4Nv12Planes *self = (GstVim4Nv12Planes *)base;
    *output = vim4_nv12_plane_view(input, self->width, self->height);
    if (!*output) {
        GST_ELEMENT_ERROR(base, STREAM, FORMAT, ("Expected packed NV12 DMA with encoder tail padding"), (NULL));
        return GST_FLOW_ERROR;
    }
    return GST_FLOW_OK;
}

static GstFlowReturn transform(GstBaseTransform *base, GstBuffer *input, GstBuffer *output)
{ (void)base; (void)input; (void)output; return GST_FLOW_OK; }

static gboolean copy_metadata(GstBaseTransform *base, GstBuffer *input, GstBuffer *output)
{
    (void)base;
    /* The new view already has the padded two-plane VideoMeta. */
    return gst_buffer_copy_into(output, input, GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_FLAGS, 0, -1);
}

static void gst_vim4_nv12_planes_class_init(GstVim4Nv12PlanesClass *klass)
{
    GstElementClass *element = GST_ELEMENT_CLASS(klass);
    GstCaps *caps = gst_caps_from_string("video/x-raw,format=NV12,width=[2,4096],height=[2,2160]");
    gst_element_class_add_pad_template(element, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
    gst_element_class_add_pad_template(element, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
    gst_caps_unref(caps);
    gst_element_class_set_static_metadata(element, "VIM4 NV12 DMA plane views", "Filter/Video",
        "Retain capture leases while importing packed NV12 into Wave5", "vim4-kvm");
    GstBaseTransformClass *base = GST_BASE_TRANSFORM_CLASS(klass);
    base->set_caps = set_caps; base->prepare_output_buffer = prepare;
    base->transform = transform; base->copy_metadata = copy_metadata;
}

static void gst_vim4_nv12_planes_init(GstVim4Nv12Planes *self)
{
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), FALSE);
    gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), FALSE);
}
