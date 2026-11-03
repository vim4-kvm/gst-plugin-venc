/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>

/* videorate negotiates the downstream fraction and preserves PTS in
 * drop-only mode. Admit one buffer per output slot before it can catch up
 * on slots missed during a slow HDMI interval. */
typedef struct {
    GstBin parent;
    GstElement *videorate;
    GstClockTime origin;
    guint64 slot;
    gint rate;
} GstVim4Rate;
typedef GstBinClass GstVim4RateClass;
G_DEFINE_TYPE(GstVim4Rate, gst_vim4_rate, GST_TYPE_BIN)

static GstPadProbeReturn limit_pts(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    GstVim4Rate *self = data;
    (void)pad;
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
        if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_SEGMENT)
            self->origin = GST_CLOCK_TIME_NONE;
        return GST_PAD_PROBE_OK;
    }
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    GstClockTime pts = GST_BUFFER_PTS(buffer);
    if (!GST_CLOCK_TIME_IS_VALID(pts)) return GST_PAD_PROBE_OK;
    if (!GST_CLOCK_TIME_IS_VALID(self->origin) || pts < self->origin ||
        GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT)) {
        self->origin = pts;
        self->slot = 0;
        return GST_PAD_PROBE_OK;
    }
    /* Accept minor IRQ jitter just before a slot boundary. A full half-slot
     * would select the intervening frame for an exact 2:1 conversion. */
    GstClockTime tolerance = GST_SECOND / (16 * self->rate);
    guint64 slot = gst_util_uint64_scale(pts - self->origin + tolerance, self->rate, GST_SECOND);
    if (slot <= self->slot) return GST_PAD_PROBE_DROP;
    self->slot = slot;
    return GST_PAD_PROBE_OK;
}

static void set_property(GObject *obj, guint id, const GValue *value, GParamSpec *spec)
{
    GstVim4Rate *self = (GstVim4Rate *)obj;
    if (id == 1) {
        self->rate = g_value_get_int(value);
        if (self->videorate) g_object_set(self->videorate, "max-rate", self->rate, NULL);
    } else G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, spec);
}

static void get_property(GObject *obj, guint id, GValue *value, GParamSpec *spec)
{
    if (id == 1) g_value_set_int(value, ((GstVim4Rate *)obj)->rate);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, spec);
}

static void gst_vim4_rate_class_init(GstVim4RateClass *klass)
{
    GObjectClass *object = G_OBJECT_CLASS(klass);
    GstElementClass *element = GST_ELEMENT_CLASS(klass);
    object->set_property = set_property;
    object->get_property = get_property;
    g_object_class_install_property(object, 1, g_param_spec_int("max-rate", "Maximum rate",
        "Output frame slots per second", 1, 120, 60, G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
        G_PARAM_STATIC_STRINGS));
    GstCaps *caps = gst_caps_new_any();
    gst_element_class_add_pad_template(element, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
    gst_element_class_add_pad_template(element, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
    gst_caps_unref(caps);
    gst_element_class_set_static_metadata(element, "VIM4 capture rate limit", "Filter/Video",
        "Drop frames by input PTS without accumulating unused output slots", "vim4-kvm");
}

static void gst_vim4_rate_init(GstVim4Rate *self)
{
    self->origin = GST_CLOCK_TIME_NONE;
    self->rate = 60;
    self->videorate = gst_element_factory_make("videorate", NULL);
    if (!self->videorate) return;
    g_object_set(self->videorate, "drop-only", TRUE, "max-rate", self->rate, NULL);
    gst_bin_add(GST_BIN(self), self->videorate);
    GstPad *sink = gst_element_get_static_pad(self->videorate, "sink");
    GstPad *src = gst_element_get_static_pad(self->videorate, "src");
    gst_pad_add_probe(sink, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
        limit_pts, self, NULL);
    gst_element_add_pad(GST_ELEMENT(self), gst_ghost_pad_new("sink", sink));
    gst_element_add_pad(GST_ELEMENT(self), gst_ghost_pad_new("src", src));
    gst_object_unref(src);
    gst_object_unref(sink);
}

static gboolean plugin_init(GstPlugin *plugin)
{
    return gst_element_register(plugin, "vim4ptsrate", GST_RANK_NONE, gst_vim4_rate_get_type());
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, vim4rate, "VIM4 capture pacing",
    plugin_init, "1.0", "LGPL", "vim4-kvm", "https://github.com/vim4-kvm")
