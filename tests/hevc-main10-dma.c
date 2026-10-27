/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Offline integration test: fake codec, real GStreamer DMA memory/lifecycle.
 * /dev/null fds cannot be mapped as video, so successful DMA encoding also
 * proves that the encoder never CPU-maps a supported DMA input. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <gst/check/gstharness.h>
#include "../hdr/gstvim4hevc10.c"

static gint expected_type, expected_stride, return_fd;
static guint encode_calls, destroy_calls;
static gboolean encode_error;
static gint live_at_destroy = -1;

static vl_codec_handle_t fake_init(vl_codec_id_t codec, vl_encode_info_t config,
                                    qp_param_t *qp)
{
    g_assert_cmpint(codec, ==, CODEC_ID_H265);
    g_assert_cmpint(config.internal_bit_depth, ==, 10);
    return (vl_codec_handle_t)1;
}

static encoding_metadata_t fake_encode(vl_codec_handle_t codec, vl_frame_type_t type,
    unsigned char *out, vl_buffer_info_t *input, vl_buffer_info_t *returned)
{
    encode_calls++;
    g_assert_cmpint(input->buf_type, ==, expected_type);
    g_assert_cmpint(input->buf_fmt, ==, IMG_FMT_P010);
    g_assert_cmpint(input->buf_stride, ==, expected_stride);
    if (input->buf_type == DMA_TYPE) {
        g_assert_cmpuint(input->buf_info.dma_info.num_planes, ==, 1);
        g_assert_cmpint(fcntl(input->buf_info.dma_info.shared_fd[0], F_GETFD), >=, 0);
    } else {
        g_assert_cmpuint(input->buf_info.in_ptr[1] - input->buf_info.in_ptr[0], ==,
                         (guint)expected_stride * 64);
    }
    if (return_fd >= 0) {
        returned->buf_type = DMA_TYPE;
        returned->buf_info.dma_info.num_planes = 1;
        returned->buf_info.dma_info.shared_fd[0] = return_fd;
    }
    memset(out, 0, 8);
    return (encoding_metadata_t){.is_valid = !encode_error,
        .encoded_data_length_in_bytes = encode_error ? 0 : 8};
}

static int fake_destroy(vl_codec_handle_t codec)
{
    destroy_calls++;
    if (live_at_destroy >= 0)
        g_assert_cmpint(fcntl(live_at_destroy, F_GETFD), >=, 0);
    return 0;
}

static void buffer_freed(gpointer data, GstMiniObject *object)
{
    (*(guint *)data)++;
}

static GstBuffer *dma_buffer(gint stride, guint *freed, gint *fd)
{
    *fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    g_assert_cmpint(*fd, >=, 0);
    GstAllocator *allocator = gst_dmabuf_allocator_new();
    GstBuffer *buffer = gst_buffer_new();
    gst_buffer_append_memory(buffer, gst_dmabuf_allocator_alloc(allocator, *fd,
                             stride * 64 * 3 / 2));
    gst_object_unref(allocator);
    gsize offsets[GST_VIDEO_MAX_PLANES] = {0, stride * 64};
    gint strides[GST_VIDEO_MAX_PLANES] = {stride, stride};
    g_assert_nonnull(gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE,
        GST_VIDEO_FORMAT_P010_10LE, 64, 64, 2, offsets, strides));
    gst_mini_object_weak_ref(GST_MINI_OBJECT(buffer), buffer_freed, freed);
    return buffer;
}

static GstHarness *harness(void)
{
    expected_type = DMA_TYPE;
    expected_stride = 160;
    return_fd = -1;
    encode_calls = destroy_calls = 0;
    encode_error = FALSE;
    live_at_destroy = -1;
    GstHarness *h = gst_harness_new_with_element(
        g_object_new(gst_vim4_hevc10_get_type(), NULL), "sink", "src");
    gst_harness_set_src_caps_str(h,
        "video/x-raw,format=P010_10LE,width=64,height=64,framerate=60/1,colorimetry=bt2100-pq");
    return h;
}

static void push(GstHarness *h, GstBuffer *buffer)
{
    g_assert_cmpint(gst_harness_push(h, buffer), ==, GST_FLOW_OK);
    gst_buffer_unref(gst_harness_pull(h));
}

static void delayed_retirement(void)
{
    GstHarness *h = harness();
    guint freed_a = 0, freed_b = 0;
    gint fd_a, fd_b;
    push(h, dma_buffer(160, &freed_a, &fd_a));
    g_assert_cmpuint(freed_a, ==, 0);
    return_fd = fd_a;
    push(h, dma_buffer(160, &freed_b, &fd_b));
    g_assert_cmpuint(freed_a, ==, 1);
    g_assert_cmpuint(freed_b, ==, 0);
    // CPU fallback can retire a DMA input submitted by an earlier call.
    expected_type = VMALLOC_TYPE;
    expected_stride = 128;
    return_fd = fd_b;
    push(h, gst_buffer_new_allocate(NULL, 128 * 64 * 3 / 2, NULL));
    g_assert_cmpuint(freed_b, ==, 1);
    g_assert_cmpuint(encode_calls, ==, 3);
    g_assert_cmpuint(((GstVim4Hevc10 *)h->element)->timing_stats.frames, ==, 0);
    gst_harness_teardown(h);
    g_assert_cmpuint(destroy_calls, ==, 1);
}

static void destroy_before_release(gboolean error)
{
    GstHarness *h = harness();
    guint freed = 0;
    gint fd;
    GstBuffer *buffer = dma_buffer(160, &freed, &fd);
    live_at_destroy = fd;
    encode_error = error;
    if (error)
        g_assert_cmpint(gst_harness_push(h, buffer), ==, GST_FLOW_ERROR);
    else {
        push(h, buffer);
        g_assert_cmpuint(freed, ==, 0);
    }
    gst_harness_teardown(h);
    g_assert_cmpuint(destroy_calls, ==, 1);
    g_assert_cmpuint(freed, ==, 1);
    g_assert_cmpint(fcntl(fd, F_GETFD), ==, -1);
}

static void unknown_retirement(void)
{
    GstHarness *h = harness();
    guint freed = 0;
    gint fd;
    GstBuffer *buffer = dma_buffer(160, &freed, &fd);
    live_at_destroy = fd;
    return_fd = fd + 1000;
    g_assert_cmpint(gst_harness_push(h, buffer), ==, GST_FLOW_ERROR);
    gst_harness_teardown(h);
    g_assert_cmpuint(destroy_calls, ==, 1);
    g_assert_cmpuint(freed, ==, 1);
}

static void reused_pending_input(void)
{
    GstHarness *h = harness();
    guint freed = 0;
    gint fd;
    GstBuffer *buffer = dma_buffer(160, &freed, &fd);
    push(h, gst_buffer_ref(buffer));
    live_at_destroy = fd;
    g_assert_cmpint(gst_harness_push(h, buffer), ==, GST_FLOW_ERROR);
    g_assert_cmpuint(encode_calls, ==, 1);
    gst_harness_teardown(h);
    g_assert_cmpuint(destroy_calls, ==, 1);
    g_assert_cmpuint(freed, ==, 1);
}

static void noncontiguous_dma_planes(void)
{
    GstHarness *h = harness();
    guint freed = 0;
    gint fd;
    GstBuffer *buffer = dma_buffer(160, &freed, &fd);
    // A single fd alone does not prove the SDK's implicit UV offset matches.
    gst_buffer_get_video_meta(buffer)->offset[1] += 16;
    g_assert_cmpint(gst_harness_push(h, buffer), ==, GST_FLOW_ERROR);
    g_assert_cmpuint(encode_calls, ==, 0);
    gst_harness_teardown(h);
    g_assert_cmpuint(destroy_calls, ==, 1);
    g_assert_cmpuint(freed, ==, 1);
}

static void stop_reopen(void)
{
    GstHarness *h = harness();
    guint freed_a = 0, freed_b = 0;
    gint fd_a, fd_b;
    push(h, dma_buffer(160, &freed_a, &fd_a));
    live_at_destroy = fd_a;
    gst_element_set_state(h->element, GST_STATE_NULL);
    g_assert_cmpuint(freed_a, ==, 1);
    g_assert_cmpuint(destroy_calls, ==, 1);
    live_at_destroy = -1;
    gst_harness_play(h);
    gst_harness_set_src_caps_str(h,
        "video/x-raw,format=P010_10LE,width=64,height=64,framerate=60/1,colorimetry=bt2100-pq");
    push(h, dma_buffer(160, &freed_b, &fd_b));
    live_at_destroy = fd_b;
    gst_harness_teardown(h);
    g_assert_cmpuint(freed_b, ==, 1);
    g_assert_cmpuint(destroy_calls, ==, 2);
}

static GString *timing_text;
static guint timing_log_count;

static void capture_timing(const gchar *message)
{
    g_assert_true(g_str_has_prefix(message, "vim4hevc10 timing "));
    timing_log_count++;
    g_string_append(timing_text, message);
}

static void timing_summary(void)
{
    timing_text = g_string_new(NULL);
    timing_log_count = 0;
    GPrintFunc previous = g_set_printerr_handler(capture_timing);
    g_setenv("VIM4_VIDEO_TIMING", "1", TRUE);
    GstHarness *h = harness();
    g_unsetenv("VIM4_VIDEO_TIMING");
    for (guint cycle = 0; cycle < 80; cycle++) {
        guint freed_a = 0, freed_b = 0;
        gint fd_a, fd_b;
        expected_type = DMA_TYPE;
        expected_stride = 160;
        return_fd = -1;
        push(h, dma_buffer(160, &freed_a, &fd_a));
        return_fd = fd_a;
        push(h, dma_buffer(160, &freed_b, &fd_b));
        g_assert_cmpuint(freed_a, ==, 1);
        if (cycle == 39)
            g_assert_cmpuint(timing_log_count, ==, 0);
        expected_type = VMALLOC_TYPE;
        expected_stride = 128;
        return_fd = fd_b;
        push(h, gst_buffer_new_allocate(NULL, 128 * 64 * 3 / 2, NULL));
        g_assert_cmpuint(freed_b, ==, 1);
        g_assert_cmpuint(timing_log_count, ==, (cycle + 1) * 3 / 120);
    }
    g_assert_nonnull(strstr(timing_text->str,
        "frames=120 dma_inputs=80 cpu_inputs=40 pending_peak=2 pending=0 encode_ms="));
    g_assert_nonnull(strstr(timing_text->str,
        "frames=240 dma_inputs=160 cpu_inputs=80 pending_peak=2 pending=0 encode_ms="));
    g_assert_nonnull(strstr(timing_text->str, " encode_max_ms="));
    Main10Timing *stats = &((GstVim4Hevc10 *)h->element)->timing_stats;
    g_assert_cmpuint(stats->encode_max_us, <=, stats->encode_us);
    gst_element_set_state(h->element, GST_STATE_NULL);
    g_assert_cmpuint(stats->frames, ==, 0);
    g_assert_cmpuint(stats->pending_peak, ==, 0);
    gst_harness_teardown(h);
    g_set_printerr_handler(previous);
    g_string_free(timing_text, TRUE);
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    g_unsetenv("VIM4_VIDEO_TIMING");
    api_once = 1;
    api.init = fake_init;
    api.encode = fake_encode;
    api.destroy = fake_destroy;
    delayed_retirement();
    destroy_before_release(FALSE);
    destroy_before_release(TRUE);
    unknown_retirement();
    reused_pending_input();
    noncontiguous_dma_planes();
    stop_reopen();
    timing_summary();
    g_print("PASS: Main10 DMA no-map, padded pitch, delayed retirement, CPU fallback, error/stop/reopen lifetime, timing summaries\n");
    return 0;
}
