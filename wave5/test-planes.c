/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "nv12-planes.h"
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <fcntl.h>
#include <unistd.h>

static GstBuffer *buffer(guint width, guint height, gsize capacity)
{
    gchar *name = NULL;
    int fd = g_file_open_tmp("vim4-planes-XXXXXX", &name, NULL);
    g_assert_cmpint(fd, >=, 0);
    unlink(name); g_free(name);
    g_assert_cmpint(ftruncate(fd, capacity), ==, 0);
    GstAllocator *allocator = gst_dmabuf_allocator_new();
    GstMemory *memory = gst_dmabuf_allocator_alloc(allocator, fd, capacity);
    gst_object_unref(allocator);
    GstBuffer *result = gst_buffer_new();
    gst_buffer_append_memory(result, gst_memory_share(memory, 0, (gsize)width * height * 3 / 2));
    gst_memory_unref(memory);
    GST_BUFFER_PTS(result) = 42 * GST_MSECOND;
    return result;
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    const guint sizes[][2] = {{3840,2160},{1920,1080},{1280,720},{640,360}};
    for (guint i = 0; i < G_N_ELEMENTS(sizes); i++) {
        guint width = sizes[i][0], height = sizes[i][1];
        gsize luma = (gsize)width * height, aligned = (gsize)width * ((height + 15) & ~15u);
        GstBuffer *parent = buffer(width, height, luma + aligned / 2);
        GstBuffer *view = vim4_nv12_plane_view(parent, width, height);
        g_assert_nonnull(view);
        g_assert_cmpuint(gst_buffer_n_memory(view), ==, 2);
        g_assert_false(gst_buffer_is_writable(parent));
        g_assert_cmpuint(gst_buffer_peek_memory(view, 0)->size, ==, aligned);
        g_assert_cmpuint(gst_buffer_peek_memory(view, 1)->offset, ==, luma);
        g_assert_cmpuint(gst_buffer_peek_memory(view, 1)->size, ==, aligned / 2);
        g_assert_cmpuint(GST_BUFFER_PTS(view), ==, GST_BUFFER_PTS(parent));
        GstVideoMeta *meta = gst_buffer_get_video_meta(view);
        g_assert_cmpuint(meta->offset[1], ==, aligned);
        gst_buffer_unref(view);
        g_assert_true(gst_buffer_is_writable(parent));
        gst_buffer_unref(parent);
    }
    GstBuffer *plain = gst_buffer_new_allocate(NULL, 1920 * 1080 * 3 / 2, NULL);
    g_assert_null(vim4_nv12_plane_view(plain, 1920, 1080));
    gst_buffer_unref(plain);
    GstBuffer *short_tail = buffer(1920, 1080, 1920 * 1080 * 3 / 2);
    g_assert_null(vim4_nv12_plane_view(short_tail, 1920, 1080));
    gst_buffer_unref(short_tail);
    return 0;
}
