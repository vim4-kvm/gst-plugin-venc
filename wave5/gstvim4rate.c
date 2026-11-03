/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/base/gstbasetransform.h>

typedef struct {
    GstBaseTransform parent;
    GstClockTime origin;
    guint64 slot;
    gint rate;
} GstVim4Rate;
typedef GstBaseTransformClass GstVim4RateClass;
G_DEFINE_TYPE(GstVim4Rate, gst_vim4_rate, GST_TYPE_BASE_TRANSFORM)

static GstCaps *rate_caps(GstBaseTransform *base, GstPadDirection direction,
                          GstCaps *caps, GstCaps *filter)
{
    gint limit = ((GstVim4Rate *)base)->rate;
    GstCaps *result = gst_caps_copy(caps);
    for (guint i = 0; i < gst_caps_get_size(result); i++) {
        GstStructure *s = gst_caps_get_structure(result, i);
        gint num, den;
        gst_structure_remove_field(s, "max-framerate");
        if (direction == GST_PAD_SINK && gst_structure_get_fraction(s, "framerate", &num, &den)) {
            if (!num)
                gst_structure_set(s, "max-framerate", GST_TYPE_FRACTION, limit, 1, NULL);
            else if (num > (gint64)limit * den)
                gst_structure_set(s, "framerate", GST_TYPE_FRACTION, limit, 1, NULL);
        } else {
            /* The same output budget can admit faster inputs. */
            gst_structure_remove_field(s, "framerate");
        }
    }
    if (filter) {
        GstCaps *intersection = gst_caps_intersect_full(filter, result, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(result);
        return intersection;
    }
    return result;
}

static gboolean sink_event(GstBaseTransform *base, GstEvent *event)
{
    if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT)
        ((GstVim4Rate *)base)->origin = GST_CLOCK_TIME_NONE;
    return GST_BASE_TRANSFORM_CLASS(gst_vim4_rate_parent_class)->sink_event(base, event);
}

static GstFlowReturn limit_pts(GstBaseTransform *base, GstBuffer *buffer)
{
    GstVim4Rate *self = (GstVim4Rate *)base;
    GstClockTime pts = GST_BUFFER_PTS(buffer);
    if (!GST_CLOCK_TIME_IS_VALID(pts)) return GST_FLOW_OK;
    /* Upstream pacing also marks dropped frames DISCONT. Their timestamps
     * still belong to this segment and must consume its slots. */
    if (!GST_CLOCK_TIME_IS_VALID(self->origin) || pts < self->origin) {
        self->origin = pts;
        self->slot = 0;
        return GST_FLOW_OK;
    }
    /* Accept minor IRQ jitter just before a slot boundary. A full half-slot
     * would select the intervening frame for an exact 2:1 conversion. */
    GstClockTime tolerance = GST_SECOND / (16 * self->rate);
    guint64 slot = gst_util_uint64_scale(pts - self->origin + tolerance, self->rate, GST_SECOND);
    if (slot <= self->slot) return GST_BASE_TRANSFORM_FLOW_DROPPED;
    self->slot = slot;
    return GST_FLOW_OK;
}

static void set_property(GObject *obj, guint id, const GValue *value, GParamSpec *spec)
{
    GstVim4Rate *self = (GstVim4Rate *)obj;
    if (id == 1) self->rate = g_value_get_int(value);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, spec);
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
    GstBaseTransformClass *transform = GST_BASE_TRANSFORM_CLASS(klass);
    transform->transform_caps = rate_caps;
    transform->transform_ip = limit_pts;
    transform->sink_event = sink_event;
    object->set_property = set_property;
    object->get_property = get_property;
    g_object_class_install_property(object, 1, g_param_spec_int("max-rate", "Maximum rate",
        "Output frame slots per second", 1, 120, 60, G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
        G_PARAM_STATIC_STRINGS));
    GstCaps *caps = gst_caps_from_string("video/x-raw(ANY)");
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
    /* Pacing only reads PTS. Forward the original DMA buffer and its lease. */
    gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), TRUE);
}

static gboolean plugin_init(GstPlugin *plugin)
{
    return gst_element_register(plugin, "vim4ptsrate", GST_RANK_NONE, gst_vim4_rate_get_type());
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, vim4rate, "VIM4 capture pacing",
    plugin_init, "1.0", "LGPL", "vim4-kvm", "https://github.com/vim4-kvm")
