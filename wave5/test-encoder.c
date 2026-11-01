/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exercise the actual bin configuration without a video device/firmware. */
#include "gstvim4wave5enc.c"
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
    caps = gst_caps_from_string("video/x-h265");
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
int main(int argc, char **argv)
{
    gst_init(&argc, &argv); g_test_init(&argc, &argv, NULL);
    g_assert_true(gst_element_register(NULL, "v4l2h265enc", GST_RANK_NONE, fake_encoder_get_type()));
    g_test_add_func("/wave5/negotiated-profile-controls", configure_profiles);
    return g_test_run();
}
