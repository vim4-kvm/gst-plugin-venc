/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exercise the actual bin configuration without a video device/firmware. */
#include "gstvim4wave5enc.c"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
typedef struct { GstElement parent; GstStructure *controls; } FakeEncoder;
typedef GstElementClass FakeEncoderClass;
G_DEFINE_TYPE(FakeEncoder, fake_encoder, GST_TYPE_ELEMENT)
enum { FAKE_0, FAKE_DEVICE, FAKE_CONTROLS, FAKE_IO, FAKE_INTERVAL };
static void fake_set(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
    FakeEncoder *self = (FakeEncoder *)object;
    if (id == FAKE_CONTROLS) {
        if (self->controls) gst_structure_free(self->controls);
        self->controls = g_value_dup_boxed(value);
    }
}
static void fake_get(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
    if (id == FAKE_DEVICE) g_value_set_string(value, "/dev/mock-wave5");
}
static void fake_finalize(GObject *object)
{
    FakeEncoder *self = (FakeEncoder *)object;
    if (self->controls) gst_structure_free(self->controls);
    G_OBJECT_CLASS(fake_encoder_parent_class)->finalize(object);
}
static void fake_encoder_class_init(FakeEncoderClass *klass)
{
    GObjectClass *object = G_OBJECT_CLASS(klass);
    object->set_property = fake_set; object->get_property = fake_get; object->finalize = fake_finalize;
    g_object_class_install_property(object, FAKE_DEVICE, g_param_spec_string("device", "Device", "Mock device", "/dev/mock-wave5", G_PARAM_READWRITE));
    g_object_class_install_property(object, FAKE_CONTROLS, g_param_spec_boxed("extra-controls", "Controls", "V4L2 controls", GST_TYPE_STRUCTURE, G_PARAM_WRITABLE));
    g_object_class_install_property(object, FAKE_IO, g_param_spec_int("output-io-mode", "IO", "Mock IO mode", 0, 5, 0, G_PARAM_WRITABLE));
    g_object_class_install_property(object, FAKE_INTERVAL, g_param_spec_uint64("min-force-key-unit-interval", "IDR interval", "Mock IDR", 0, G_MAXUINT64, 0, G_PARAM_WRITABLE));
    GstElementClass *element = GST_ELEMENT_CLASS(klass);
    GstCaps *caps = gst_caps_from_string("video/x-raw,format=(string){NV12,P010_10LE}");
    gst_element_class_add_pad_template(element, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps)); gst_caps_unref(caps);
    caps = gst_caps_from_string("video/x-h265;video/x-h264");
    gst_element_class_add_pad_template(element, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps)); gst_caps_unref(caps);
    gst_element_class_set_static_metadata(element, "Mock encoder", "Codec/Encoder", "No hardware", "vim4-kvm");
}
static void fake_encoder_init(FakeEncoder *self)
{
    GstElementClass *klass = GST_ELEMENT_GET_CLASS(self);
    gst_element_add_pad(GST_ELEMENT(self), gst_pad_new_from_template(gst_element_class_get_pad_template(klass, "sink"), "sink"));
    gst_element_add_pad(GST_ELEMENT(self), gst_pad_new_from_template(gst_element_class_get_pad_template(klass, "src"), "src"));
}
static void configure_profiles(void)
{
    const char *profiles[] = {"main", "main-10"};
    for (guint tenbit = 0; tenbit < 2; tenbit++) {
        GstElement *pipeline = gst_pipeline_new(NULL);
        GstVim4Wave5Enc *encoder = g_object_new(gst_vim4_wave5_enc_get_type(), NULL);
        GstElement *filter = gst_element_factory_make("capsfilter", NULL);
        gchar *text = g_strdup_printf("video/x-h265,profile=%s", profiles[tenbit]);
        GstCaps *caps = gst_caps_from_string(text); g_free(text);
        g_object_set(filter, "caps", caps, NULL); gst_caps_unref(caps);
        gst_bin_add_many(GST_BIN(pipeline), GST_ELEMENT(encoder), filter, NULL);
        g_assert_true(gst_element_link(GST_ELEMENT(encoder), filter));
        g_assert_true(configure(encoder, "/dev/mock-wave5"));
        FakeEncoder *hardware = (FakeEncoder *)encoder->encoder;
        gint profile;
        g_assert_true(gst_structure_get_int(hardware->controls, "hevc_profile", &profile));
        g_assert_cmpint(profile, ==, tenbit ? V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10 : V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN);
        if (tenbit) {
            GstPad *sink = gst_element_get_static_pad(encoder->planes, "sink");
            caps = gst_pad_query_caps(sink, NULL);
            GstCaps *p010 = gst_caps_from_string("video/x-raw,format=P010_10LE");
            g_assert_true(gst_caps_is_subset(caps, p010));
            gst_caps_unref(p010); gst_caps_unref(caps); gst_object_unref(sink);
        }
        clear_children(encoder); gst_object_unref(pipeline);
    }
}
static void avc_without_receiver_is_limited(void)
{
    GstElement *pipeline = gst_pipeline_new(NULL);
    GstVim4Wave5Enc *encoder = g_object_new(gst_vim4_wave5_enc_get_type(), NULL);
    GstElement *filter = gst_element_factory_make("capsfilter", NULL);
    GstCaps *caps = gst_caps_from_string("video/x-h264");
    g_object_set(filter, "caps", caps, NULL);
    gst_caps_unref(caps);
    gst_bin_add_many(GST_BIN(pipeline), GST_ELEMENT(encoder), filter, NULL);
    g_assert_true(gst_element_link(GST_ELEMENT(encoder), filter));
    g_assert_true(configure(encoder, "/dev/mock-wave5"));
    g_assert_nonnull(encoder->rate_limit);
    gint maximum;
    g_object_get(encoder->rate_limit, "max-rate", &maximum, NULL);
    g_assert_cmpint(maximum, ==, 60);
    clear_children(encoder);
    gst_object_unref(pipeline);
}

static void capture_budget_follows_fixed_or_vtem_timing(void)
{
    const struct {
        gint num, den, vrr, ceiling, out_num, out_den, max;
    } modes[] = {
        {59999,1000,0,60,59999,1000,0}, {1199,10,0,120,1199,10,0},
        {60,1,0,120,60,1,0}, {120,1,0,60,60,1,0}, {60,1,0,10,10,1,0},
        {0,1,120,120,0,1,120}, {0,1,120,60,0,1,60}, {0,1,-1,0,0,1,0},
    };
    for (guint i = 0; i < G_N_ELEMENTS(modes); i++) {
        Vim4Input input = {.num=modes[i].num, .den=modes[i].den, .vrr=modes[i].vrr};
        g_autofree gchar *rate = vim4_capture_rate_chain(&input, modes[i].ceiling);
        g_autofree gchar *text = g_strdup_printf("videotestsrc num-buffers=12 ! "
            "capsfilter caps=video/x-raw,format=NV12,width=64,height=32%s ! fakesink name=end", rate);
        GError *error = NULL;
        GstElement *pipeline = gst_parse_launch(text, &error);
        g_assert_no_error(error);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        GstBus *bus = gst_element_get_bus(pipeline);
        GstMessage *msg = gst_bus_timed_pop_filtered(bus, 3 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
        g_assert_nonnull(msg);
        g_assert_cmpint(GST_MESSAGE_TYPE(msg), ==, GST_MESSAGE_EOS);
        GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "end");
        GstPad *pad = gst_element_get_static_pad(sink, "sink");
        GstCaps *caps = gst_pad_get_current_caps(pad);
        GstStructure *s = gst_caps_get_structure(caps, 0);
        gint num, den;
        g_assert_true(gst_structure_get_fraction(s, "framerate", &num, &den));
        g_assert_cmpint(num, ==, modes[i].out_num);
        g_assert_cmpint(den, ==, modes[i].out_den);
        if (modes[i].max) {
            g_assert_true(gst_structure_get_fraction(s, "max-framerate", &num, &den));
            g_assert_cmpint(num, ==, modes[i].max * den);
        }
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_caps_unref(caps); gst_object_unref(pad); gst_object_unref(sink);
        gst_message_unref(msg); gst_object_unref(bus); gst_object_unref(pipeline);
    }
}

static void variable_capture_keeps_source_pts(void)
{
    const GstClockTime pts[] = {0, 8333333, 16666667, 37074830, 57482993, 65816326, 74149659};
    Vim4Input input = {.vrr=120, .den=1};
    for (guint ceiling = 60; ceiling <= 120; ceiling += 60) {
        g_autofree gchar *rate = vim4_capture_rate_chain(&input, ceiling);
        g_autofree gchar *text = g_strdup_printf("appsrc name=in format=time "
            "caps=video/x-raw,format=NV12,width=16,height=16,framerate=120/1 ! "
            "capsfilter caps=video/x-raw,format=NV12%s ! appsink name=end sync=false", rate);
        GError *error = NULL;
        GstElement *pipeline = gst_parse_launch(text, &error);
        g_assert_no_error(error);
        GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline), "in");
        GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "end");
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        for (guint i = 0; i < G_N_ELEMENTS(pts); i++) {
            GstBuffer *frame = gst_buffer_new_allocate(NULL, 16 * 16 * 3 / 2, NULL);
            GST_BUFFER_PTS(frame) = pts[i];
            g_assert_cmpint(gst_app_src_push_buffer(GST_APP_SRC(src), frame), ==, GST_FLOW_OK);
        }
        gst_app_src_end_of_stream(GST_APP_SRC(src));
        guint count = 0, i = 0;
        GstSample *sample;
        while ((sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), GST_SECOND))) {
            GstClockTime stamp = GST_BUFFER_PTS(gst_sample_get_buffer(sample));
            while (i < G_N_ELEMENTS(pts) && pts[i] != stamp) i++;
            g_assert_cmpuint(i, <, G_N_ELEMENTS(pts));
            i++; count++;
            gst_sample_unref(sample);
        }
        g_assert_true(gst_app_sink_is_eos(GST_APP_SINK(sink)));
        if (ceiling == 120) g_assert_cmpuint(count, ==, G_N_ELEMENTS(pts));
        else { g_assert_cmpuint(count, >=, 3); g_assert_cmpuint(count, <, G_N_ELEMENTS(pts)); }
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(src); gst_object_unref(sink); gst_object_unref(pipeline);
    }
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv); g_test_init(&argc, &argv, NULL);
    g_assert_true(gst_element_register(NULL, "v4l2h265enc", GST_RANK_NONE, fake_encoder_get_type()));
    g_assert_true(gst_element_register(NULL, "v4l2h264enc", GST_RANK_NONE, fake_encoder_get_type()));
    g_test_add_func("/wave5/unknown-input-avc-limit", avc_without_receiver_is_limited);
    g_test_add_func("/wave5/capture-budget", capture_budget_follows_fixed_or_vtem_timing);
    g_test_add_func("/wave5/variable-capture-pts", variable_capture_keeps_source_pts);
    g_test_add_func("/wave5/negotiated-profile-controls", configure_profiles);
    return g_test_run();
}
