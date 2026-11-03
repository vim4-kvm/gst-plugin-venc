/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "devices.h"

typedef struct {
    GstBin parent;
    GstElement *chain;
    GstPad *src;
    gint height, num_buffers, max_fps;
} GstVim4V4l2Src;
typedef struct { GstBinClass parent; } GstVim4V4l2SrcClass;
G_DEFINE_TYPE(GstVim4V4l2Src, gst_vim4_v4l2_src, GST_TYPE_BIN)
enum { PROP_0, PROP_HEIGHT, PROP_NUM_BUFFERS, PROP_INPUT_RATE, PROP_MAX_FPS };

static void set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
    GstVim4V4l2Src *self = (GstVim4V4l2Src *)object;
    if (id == PROP_HEIGHT) self->height = g_value_get_int(value);
    else if (id == PROP_MAX_FPS) self->max_fps = g_value_get_int(value);
    else if (id == PROP_NUM_BUFFERS) self->num_buffers = g_value_get_int(value);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
    GstVim4V4l2Src *self = (GstVim4V4l2Src *)object;
    if (id == PROP_HEIGHT) g_value_set_int(value, self->height);
    else if (id == PROP_MAX_FPS) g_value_set_int(value, self->max_fps);
    else if (id == PROP_NUM_BUFFERS) g_value_set_int(value, self->num_buffers);
    else if (id == PROP_INPUT_RATE) g_value_set_int(value, vim4_input_rate());
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static GstStateChangeReturn change_state(GstElement *element, GstStateChange transition)
{
    GstVim4V4l2Src *self = (GstVim4V4l2Src *)element;
    if (transition == GST_STATE_CHANGE_NULL_TO_READY) {
        g_autofree gchar *device = vim4_video_device("t7-vdin-test");
        Vim4Input input;
        if (!device || !vim4_input_timings(device, &input, TRUE)) {
            GST_ELEMENT_ERROR(element, RESOURCE, NOT_FOUND, ("VIM4 HDMI input is unavailable"), (NULL));
            return GST_STATE_CHANGE_FAILURE;
        }
        guint width = input.timings.bt.width, height = input.timings.bt.height;
        if (self->height && (guint)self->height < height) {
            width = MAX(320u, ((guint64)width * self->height / height) & ~15u);
            height = self->height & ~1u;
        }
        g_autofree gchar *rate = vim4_capture_rate_chain(&input, self->max_fps);
        g_autofree gchar *pipeline = g_strdup_printf("v4l2src name=capture device=%s io-mode=dmabuf "
            "num-buffers=%d ! capsfilter caps=video/x-raw,format=NV12,width=%u,height=%u%s",
            device, self->num_buffers, width, height, rate);
        GError *error = NULL;
        self->chain = gst_parse_bin_from_description(pipeline, TRUE, &error);
        if (error || !self->chain) {
            GST_ELEMENT_ERROR(element, CORE, NEGOTIATION, ("Cannot build capture chain"),
                              ("%s", error ? error->message : "missing bin"));
            g_clear_error(&error);
            gst_clear_object(&self->chain);
            return GST_STATE_CHANGE_FAILURE;
        }
        gst_bin_add(GST_BIN(self), self->chain);
        GstPad *pad = gst_element_get_static_pad(self->chain, "src");
        gst_ghost_pad_set_target(GST_GHOST_PAD(self->src), pad);
        gst_object_unref(pad);
    }
    GstStateChangeReturn result = GST_ELEMENT_CLASS(gst_vim4_v4l2_src_parent_class)->change_state(element, transition);
    if (transition == GST_STATE_CHANGE_READY_TO_NULL ||
        (transition == GST_STATE_CHANGE_NULL_TO_READY && result == GST_STATE_CHANGE_FAILURE)) {
        gst_ghost_pad_set_target(GST_GHOST_PAD(self->src), NULL);
        gst_element_set_state(self->chain, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(self), self->chain);
        self->chain = NULL;
    }
    return result;
}

static void gst_vim4_v4l2_src_class_init(GstVim4V4l2SrcClass *klass)
{
    GObjectClass *object = G_OBJECT_CLASS(klass);
    object->set_property = set_property; object->get_property = get_property;
    g_object_class_install_property(object, PROP_HEIGHT, g_param_spec_int("height", "Output height",
        "VDIN downscale height; zero keeps the input dimensions", 0, 2160, 0,
        G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(object, PROP_NUM_BUFFERS, g_param_spec_int("num-buffers", "Number of buffers",
        "Capture limit; minus one is unlimited", -1, G_MAXINT, -1,
        G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(object, PROP_MAX_FPS, g_param_spec_int("max-fps", "Maximum FPS",
        "Drop frames above this ceiling; zero follows the input", 0, 120, 0,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(object, PROP_INPUT_RATE, g_param_spec_int("input-rate", "HDMI rate",
        "Capture budget in hundredths of a frame per second; zero without signal", 0, 12000, 0,
        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
    GstElementClass *element = GST_ELEMENT_CLASS(klass);
    element->change_state = change_state;
    GstCaps *caps = gst_caps_from_string("video/x-raw,format=NV12,width=[320,4096],height=[240,2160]");
    gst_element_class_add_pad_template(element, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
    gst_caps_unref(caps);
    gst_element_class_set_static_metadata(element, "VIM4 mainline capture", "Source/Video",
        "Leased VDIN DMA-buf capture using the standard V4L2 interface", "vim4-kvm");
}

static void gst_vim4_v4l2_src_init(GstVim4V4l2Src *self)
{
    self->num_buffers = -1;
    GstPadTemplate *templ = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(self), "src");
    self->src = gst_ghost_pad_new_no_target_from_template("src", templ);
    gst_element_add_pad(GST_ELEMENT(self), self->src);
}
