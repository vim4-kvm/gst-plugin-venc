/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <errno.h>
#include "../hdr/hdr10.h"
static gboolean metadata_read(int fd, Vim4Hdr10 *metadata);
#define vim4_hdr10_read metadata_read
#include "../hdr/gstvim4h265src.c"
#undef vim4_hdr10_read
GType gst_vim4_hevc10_get_type(void);

static guint metadata_reads, recovery_requests;
static gboolean metadata_read(int fd, Vim4Hdr10 *metadata)
{
    (void)fd;
    metadata_reads++;
    memset(metadata, metadata_reads, sizeof(*metadata));
    return metadata_reads != 1;
}

static gboolean recovery_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
    g_assert_true(gst_video_event_is_force_key_unit(event));
    recovery_requests++;
    gst_event_unref(event);
    return TRUE;
}

static void metadata_loss_recovery(void)
{
    GstVim4H265Src *source = g_object_new(gst_vim4_h265_src_get_type(), NULL);
    source->wave5 = source->hdr = source->have_metadata = TRUE;
    source->checked_at = g_get_monotonic_time();
    GstPad *pad = gst_pad_new("encoded", GST_PAD_SRC);
    gst_pad_set_event_function(pad, recovery_event);
    gst_pad_set_active(pad, TRUE);
    metadata_reads = recovery_requests = 0;
    for (guint i = 0; i < 5; i++) {
        /* Lose an IRAP, suppress its predicted successors, then recover only
         * on a real IRAP. Flags alone must not label a delta as random access. */
        guint8 au[] = {0,0,0,1, i == 0 || i == 3 ? 0x26 : 2, 1, 0xaf};
        GstBuffer *buffer = gst_buffer_new_allocate(NULL, sizeof(au), NULL);
        gst_buffer_fill(buffer, 0, au, sizeof(au));
        GstPadProbeInfo probe = {.type = GST_PAD_PROBE_TYPE_BUFFER, .data = buffer};
        GstPadProbeReturn result = encoded_frame(pad, &probe, source);
        if (i < 3) {
            g_assert_cmpint(result, ==, GST_PAD_PROBE_DROP);
        } else {
            g_assert_cmpint(result, ==, GST_PAD_PROBE_OK);
            GstBuffer *sent = GST_PAD_PROBE_INFO_BUFFER(&probe);
            g_assert_cmpint(GST_BUFFER_FLAG_IS_SET(sent, GST_BUFFER_FLAG_DISCONT), ==, i == 3);
            if (i == 3) {
                guint8 prefix[36];
                g_assert_true(vim4_hdr10_is_irap(sent));
                g_assert_cmpuint(gst_buffer_extract(sent, 0, prefix, sizeof(prefix)), ==, sizeof(prefix));
                g_assert_cmpuint(prefix[4] >> 1, ==, 39);
                g_assert_cmpuint(prefix[6], ==, 137);
                for (guint n = 8; n < 32; n++) g_assert_cmpuint(prefix[n], ==, 4);
                g_assert_cmpuint(prefix[32], ==, 144);
                g_assert_cmpuint(prefix[34], ==, 4);
            }
        }
        gst_buffer_unref(GST_PAD_PROBE_INFO_BUFFER(&probe));
    }
    g_assert_cmpuint(recovery_requests, ==, 1);
    gst_pad_set_active(pad, FALSE); gst_object_unref(pad); gst_object_unref(source);
}

static void policy(void)
{
    const gchar *pq = "Hactive: 3840\nVactive: 2160\nHDR EOTF: SMPTE_ST_2048\n";
    g_assert_true(preserve_hdr("HDR_SDR_ENABLED=0\n", pq));
    g_assert_false(preserve_hdr("HDR_SDR_ENABLED=1\n", pq));
    g_assert_false(preserve_hdr("# HDR_SDR_ENABLED=0\nOTHER=0\n", pq));
    g_assert_false(preserve_hdr("HDR_SDR_ENABLED=garbage\n", pq));
    g_assert_false(preserve_hdr("HDR_SDR_ENABLED=0\nHDR_SDR_ENABLED=1\n", pq));
    g_assert_true(preserve_hdr("HDR_SDR_ENABLED=1\n HDR_SDR_ENABLED = 0\n", pq));
    g_assert_false(preserve_hdr("HDR_SDR_ENABLED=0", "HDR EOTF: HLG\n"));
    g_assert_false(preserve_hdr("HDR_SDR_ENABLED=0", "HDR EOTF: SDR\n"));
}

static void capture_scaling_contract(void)
{
    GstElementFactory *factory = gst_element_factory_find("vfmcapsrc");
    if (!factory) {
        g_test_skip("Requires the companion vfmcapsrc plugin (NULL state only; no board access)");
        return;
    }
    gst_object_unref(factory);
    GstVim4H265Src *source = g_object_new(gst_vim4_h265_src_get_type(), NULL);
    const guint requested[] = {0, 1, 1701, 2160};
    const guint expected[] = {0, 2, 1700, 2160};
    for (guint i = 0; i < G_N_ELEMENTS(requested); i++) {
        source->height = requested[i];
        gchar *text = description(source, TRUE);
        GError *error = NULL;
        GstElement *bin = gst_parse_bin_from_description(text, TRUE, &error);
        g_assert_no_error(error);
        g_assert_nonnull(bin);
        GstElement *capture = gst_bin_get_by_name(GST_BIN(bin), "capture");
        guint height;
        g_object_get(capture, "height", &height, NULL);
        g_assert_cmpuint(height, ==, expected[i]);
        GstElement *encoder = gst_bin_get_by_name(GST_BIN(bin), "encoder");
        GstPad *sink = gst_element_get_static_pad(encoder, "sink");
        GstCaps *caps = gst_pad_peer_query_caps(sink, NULL);
        GstCaps *required = gst_caps_from_string("video/x-raw,format=P010_10LE,framerate=60/1");
        g_assert_false(gst_caps_is_empty(caps));
        g_assert_true(gst_caps_is_subset(caps, required));
        gst_caps_unref(required);
        gst_caps_unref(caps);
        gst_object_unref(sink);
        gst_object_unref(encoder);
        gst_object_unref(capture);
        gst_object_unref(bin);
        g_free(text);
    }
    gst_object_unref(source);
}

static void sdr_scaling_contract(void)
{
    GstElementFactory *factory = gst_element_factory_find("vim4scale");
    if (!factory) {
        g_test_skip("Requires the companion DMA scaler plugin (NULL state only)");
        return;
    }
    gst_object_unref(factory);
    GstVim4H265Src *source = g_object_new(gst_vim4_h265_src_get_type(), "height", 1701, NULL);
    gchar *text = description(source, FALSE);
    GError *error = NULL;
    GstElement *bin = gst_parse_bin_from_description(text, TRUE, &error);
    g_assert_no_error(error);
    g_assert_nonnull(bin);
    GstElement *capture = gst_bin_get_by_name(GST_BIN(bin), "capture");
    GstElement *scaler = gst_bin_get_by_name(GST_BIN(bin), "scaler");
    guint capture_height, output_height, alignment;
    g_object_get(capture, "height", &capture_height, NULL);
    g_object_get(scaler, "height", &output_height, "width-alignment", &alignment, NULL);
    /* Driver canvas rounding must not change the source's visible aspect ratio. */
    g_assert_cmpuint(capture_height, ==, 0);
    g_assert_cmpuint(output_height, ==, 1700);
    g_assert_cmpuint(alignment, ==, 16);
    gst_object_unref(scaler);
    gst_object_unref(capture);
    gst_object_unref(bin);
    gst_object_unref(source);
    g_free(text);
}

static void bootstrap_retirement(void)
{
    GstVim4H265Src *source = g_object_new(gst_vim4_h265_src_get_type(), NULL);
    gint descriptors[2];
    g_assert_cmpint(pipe(descriptors), ==, 0);
    source->bootstrap = descriptors[0];
    source->checked_at = g_get_monotonic_time();
    g_assert_cmpint(encoded_frame(NULL, NULL, source), ==, GST_PAD_PROBE_OK);
    g_assert_cmpint(fcntl(descriptors[0], F_GETFD), ==, -1);
    g_assert_cmpint(errno, ==, EBADF);
    g_assert_cmpint(source->bootstrap, ==, -1);
    release_bootstrap(source);
    close(descriptors[1]);
    source->changing = TRUE;
    g_assert_cmpint(encoded_frame(NULL, NULL, source), ==, GST_PAD_PROBE_DROP);
    gst_object_unref(source);
}

static void reject_stock_runtime(void)
{
    g_unsetenv("VIM4_SECONDARY_HDMI_CAPTURE");
    GstElement *pipeline = gst_pipeline_new(NULL);
    GstElement *source = g_object_new(gst_vim4_h265_src_get_type(), NULL);
    gst_bin_add(GST_BIN(pipeline), source);
    g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_READY), ==, GST_STATE_CHANGE_FAILURE);
    g_assert_cmpint(((GstVim4H265Src *)source)->bootstrap, ==, -1);
    g_assert_null(((GstVim4H265Src *)source)->chain);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

static void partial_startup_cleanup(void)
{
    GstVim4H265Src *source = g_object_new(gst_vim4_h265_src_get_type(), NULL);
    GstElement *chain = gst_parse_bin_from_description("fakesrc ! identity", TRUE, NULL);
    source->chain = chain;
    gst_bin_add(GST_BIN(source), chain);
    gst_object_ref(chain);
    g_assert_cmpint(gst_element_set_state(chain, GST_STATE_READY), ==, GST_STATE_CHANGE_SUCCESS);
    clear_chain(source);
    GstState state;
    gst_element_get_state(chain, &state, NULL, GST_CLOCK_TIME_NONE);
    g_assert_cmpint(state, ==, GST_STATE_NULL);
    gst_object_unref(chain);
    gst_object_unref(source);
}

static void terminal_caps(void)
{
    GstVim4H265Src *source = g_object_new(gst_vim4_h265_src_get_type(), NULL);
    for (guint hdr = 0; hdr < 2; ++hdr) {
        gchar *text = description(source, hdr);
        /* A bare caps expression at the end of a parsed bin is interpreted
         * as an element/URI. The terminal filter must work without a sink. */
        gchar *tail = strrchr(text, '!');
        GError *error = NULL;
        GstElement *bin = gst_parse_bin_from_description(tail + 1, TRUE, &error);
        g_assert_no_error(error);
        g_assert_nonnull(bin);
        GstPad *src = gst_element_get_static_pad(bin, "src");
        g_assert_nonnull(src);
        gst_object_unref(src);
        gst_object_unref(bin);
        g_free(text);
    }
    gst_object_unref(source);
}

static void wave5_raw_branch_contract(void)
{
    g_setenv("VIM4_VIDEO_BACKEND", "wave5", TRUE);
    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch("vim4h265src name=video ! fakesink video.raw ! fakesink", &error);
    g_assert_no_error(error); g_assert_nonnull(pipeline);
    GstElement *video = gst_bin_get_by_name(GST_BIN(pipeline), "video");
    GstPad *raw = gst_element_get_static_pad(video, "raw");
    g_assert_nonnull(raw); g_assert_true(gst_pad_is_linked(raw));
    gst_object_unref(raw); gst_object_unref(video); gst_object_unref(pipeline);
    GstVim4H265Src *source = g_object_new(gst_vim4_h265_src_get_type(), "height", 1700, NULL);
    source->wave5 = TRUE;
    for (guint hdr = 0; hdr < 2; hdr++) {
        gchar *text = description(source, hdr);
        GstElement *bin = gst_parse_bin_from_description(text, TRUE, &error);
        if (error && error->domain == GST_PARSE_ERROR && error->code == GST_PARSE_ERROR_NO_SUCH_ELEMENT) {
            g_test_skip("Requires companion capture and Wave5 plugins (NULL state only)");
            g_clear_error(&error);
            if (bin) gst_object_unref(bin);
            g_free(text); break;
        }
        g_assert_no_error(error); g_assert_nonnull(bin);
        GstElement *capture = gst_bin_get_by_name(GST_BIN(bin), "capture");
        guint height;
        g_object_get(capture, "height", &height, NULL);
        g_assert_cmpuint(height, ==, 1700);
        GstElement *encoder = gst_bin_get_by_name(GST_BIN(bin), "encoder");
        GstPad *sink = gst_element_get_static_pad(encoder, "sink");
        GstCaps *actual = gst_pad_peer_query_caps(sink, NULL);
        GstCaps *expected = gst_caps_from_string(hdr ?
            "video/x-raw,format=P010_10LE,colorimetry=bt2100-pq,framerate=60/1" :
            "video/x-raw,format=NV12");
        g_assert_false(gst_caps_is_empty(actual)); g_assert_true(gst_caps_is_subset(actual, expected));
        gst_caps_unref(expected); gst_caps_unref(actual); gst_object_unref(sink);
        gst_object_unref(encoder); gst_object_unref(capture); gst_object_unref(bin); g_free(text);
    }
    gst_object_unref(source);
    g_unsetenv("VIM4_VIDEO_BACKEND");
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    /* Use the real encoder type without loading a second copy of this source
     * class from the shared plugin. No codec is opened in NULL state. */
    g_assert_true(gst_element_register(NULL, "vim4hevc10enc", GST_RANK_NONE, gst_vim4_hevc10_get_type()));
    g_assert_true(gst_element_register(NULL, "vim4h265src", GST_RANK_NONE, gst_vim4_h265_src_get_type()));
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/h265-source/global-policy", policy);
    g_test_add_func("/h265-source/metadata-loss-recovery", metadata_loss_recovery);
    g_test_add_func("/h265-source/capture-scaling-contract", capture_scaling_contract);
    g_test_add_func("/h265-source/sdr-scaling-contract", sdr_scaling_contract);
    g_test_add_func("/h265-source/bootstrap-retirement", bootstrap_retirement);
    g_test_add_func("/h265-source/reject-stock-runtime", reject_stock_runtime);
    g_test_add_func("/h265-source/partial-startup-cleanup", partial_startup_cleanup);
    g_test_add_func("/h265-source/terminal-caps", terminal_caps);
    g_test_add_func("/h265-source/wave5-raw-branch-contract", wave5_raw_branch_contract);
    return g_test_run();
}
