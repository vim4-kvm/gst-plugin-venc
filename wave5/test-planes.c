/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "nv12-planes.h"
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
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

static void firmware_budget_preserves_pts(void)
{
    const gchar *rates[] = {"framerate=59999/1000", "framerate=60000/1001",
                            "framerate=1199/10", "framerate=0/1,max-framerate=120/1"};
    const gint budgets[] = {60, 60, 120, 120};
    const guint64 pts[] = {0, 8333333, 28741496, 37074829};
    for (guint i = 0; i < G_N_ELEMENTS(rates); i++) {
        GstElement *pipeline = gst_pipeline_new(NULL);
        GstElement *src = gst_element_factory_make("appsrc", NULL);
        GstElement *adapter = g_object_new(gst_vim4_nv12_planes_get_type(), NULL);
        GstElement *sink = gst_element_factory_make("appsink", NULL);
        g_autofree gchar *text = g_strdup_printf("video/x-raw,format=NV12,width=64,height=32,%s", rates[i]);
        GstCaps *caps = gst_caps_from_string(text);
        g_object_set(src, "format", GST_FORMAT_TIME, "caps", caps, NULL);
        g_object_set(sink, "sync", FALSE, NULL);
        gst_caps_unref(caps);
        gst_bin_add_many(GST_BIN(pipeline), src, adapter, sink, NULL);
        g_assert_true(gst_element_link_many(src, adapter, sink, NULL));
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        for (guint j = 0; j < G_N_ELEMENTS(pts); j++) {
            GstBuffer *raw = buffer(64, 32, 64 * 32 * 3 / 2);
            GST_BUFFER_PTS(raw) = pts[j];
            g_assert_cmpint(gst_app_src_push_buffer(GST_APP_SRC(src), raw), ==, GST_FLOW_OK);
            GstSample *out = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), GST_SECOND);
            g_assert_nonnull(out);
            gint num, den;
            g_assert_true(gst_structure_get_fraction(gst_caps_get_structure(gst_sample_get_caps(out), 0),
                                                     "framerate", &num, &den));
            g_assert_cmpint(num, ==, budgets[i] * den);
            g_assert_cmpuint(GST_BUFFER_PTS(gst_sample_get_buffer(out)), ==, pts[j]);
            gst_sample_unref(out);
        }
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    firmware_budget_preserves_pts();
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
