/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "devices.h"

typedef struct {
    GstBin parent;
    GstElement *source, *filter;
    gint height, num_buffers;
} GstVim4V4l2Src;
typedef struct { GstBinClass parent; } GstVim4V4l2SrcClass;
G_DEFINE_TYPE(GstVim4V4l2Src, gst_vim4_v4l2_src, GST_TYPE_BIN)
enum { PROP_0, PROP_HEIGHT, PROP_NUM_BUFFERS, PROP_INPUT_RATE };

static void set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
    GstVim4V4l2Src *self = (GstVim4V4l2Src *)object;
    if (id == PROP_HEIGHT) self->height = g_value_get_int(value);
    else if (id == PROP_NUM_BUFFERS) self->num_buffers = g_value_get_int(value);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
    GstVim4V4l2Src *self = (GstVim4V4l2Src *)object;
    if (id == PROP_HEIGHT) g_value_set_int(value, self->height);
    else if (id == PROP_NUM_BUFFERS) g_value_set_int(value, self->num_buffers);
    else if (id == PROP_INPUT_RATE) g_value_set_int(value, vim4_input_rate());
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static GstStateChangeReturn change_state(GstElement *element, GstStateChange transition)
{
    GstVim4V4l2Src *self = (GstVim4V4l2Src *)element;
    if (transition == GST_STATE_CHANGE_NULL_TO_READY) {
        struct v4l2_dv_timings timings;
        gchar *device = vim4_video_device("t7-vdin-test");
        if (!device || !self->source || !self->filter || !vim4_input_timings(&timings)) {
            g_free(device);
            GST_ELEMENT_ERROR(element, RESOURCE, NOT_FOUND, ("VIM4 HDMI capture or input signal is unavailable"), (NULL));
            return GST_STATE_CHANGE_FAILURE;
        }
        guint width = timings.bt.width, height = timings.bt.height;
        if (self->height && (guint)self->height < height) {
            width = MAX(320u, ((guint64)width * self->height / height) & ~15u);
            height = self->height & ~1u;
        }
        GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "NV12",
            "width", G_TYPE_INT, width, "height", G_TYPE_INT, height,
            "framerate", GST_TYPE_FRACTION, 60, 1, NULL);
        g_object_set(self->source, "device", device, "io-mode", 4, "num-buffers", self->num_buffers, NULL);
        g_object_set(self->filter, "caps", caps, NULL);
        gst_caps_unref(caps); g_free(device);
    }
    return GST_ELEMENT_CLASS(gst_vim4_v4l2_src_parent_class)->change_state(element, transition);
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
    g_object_class_install_property(object, PROP_INPUT_RATE, g_param_spec_int("input-rate", "HDMI rate",
        "Measured input rate in hundredths of a frame per second; zero without signal", 0, 12000, 0,
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
    self->source = gst_element_factory_make("v4l2src", "capture");
    self->filter = gst_element_factory_make("capsfilter", "capture_format");
    if (!self->source || !self->filter) {
        if (self->source) gst_object_unref(self->source);
        if (self->filter) gst_object_unref(self->filter);
        self->source = self->filter = NULL;
        return;
    }
    gst_bin_add_many(GST_BIN(self), self->source, self->filter, NULL);
    gst_element_link(self->source, self->filter);
    GstPad *pad = gst_element_get_static_pad(self->filter, "src");
    gst_element_add_pad(GST_ELEMENT(self), gst_ghost_pad_new("src", pad));
    gst_object_unref(pad);
}
