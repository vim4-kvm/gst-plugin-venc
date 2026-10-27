/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <fcntl.h>
#include <unistd.h>
#include <gst/gst.h>

/* One encoded source owns capture and encoding so all applications use the
 * same HDR policy, contiguous-memory bootstrap and teardown order. JPEG stays
 * on VDIN1 and must never consume these raw PQ frames. */
typedef struct {
    GstBin parent;
    GstElement *chain;
    GstPad *src;
    guint bitrate, framerate, gop, height;
    gint qp_max, bootstrap;
    gboolean hdr, changing;
    gint64 checked_at;
} GstVim4H265Src;
typedef struct { GstBinClass parent; } GstVim4H265SrcClass;
G_DEFINE_TYPE(GstVim4H265Src, gst_vim4_h265_src, GST_TYPE_BIN)
enum { PROP_0, PROP_BITRATE, PROP_FRAMERATE, PROP_GOP, PROP_HEIGHT,
       PROP_QP_MAX, PROP_HDR_CAPABLE, PROP_HDR_REQUESTED };

static gboolean file_equals(const gchar *path, const gchar *expected)
{
    gchar *text = NULL;
    gboolean equal = g_file_get_contents(path, &text, NULL, NULL) &&
                     g_strcmp0(g_strstrip(text), expected) == 0;
    g_free(text);
    return equal;
}

static gboolean hdr_capable(void)
{
    return g_strcmp0(g_getenv("VIM4_VIDEO_BACKEND"), "wave5") != 0 &&
        file_equals("/sys/module/aml_media/parameters/vdin0_native_hdr", "Y") &&
        file_equals("/sys/module/vfm_cap/parameters/hdr_metadata_version", "1");
}

/* An absent/invalid global preference retains the safe SDR default. The last
 * valid assignment has the same meaning as One-KVM's shared file reader. */
static gboolean preserve_hdr(const gchar *settings, const gchar *input)
{
    gboolean enabled = FALSE, pq = FALSE;
    gchar **lines = g_strsplit(settings, "\n", -1);
    for (gchar **line = lines; *line; ++line) {
        gchar **pair = g_strsplit(*line, "=", 2);
        if (pair[1] && g_str_equal(g_strstrip(pair[0]), "HDR_SDR_ENABLED")) {
            gchar *value = g_strstrip(pair[1]);
            if (g_str_equal(value, "0")) enabled = TRUE;
            if (g_str_equal(value, "1")) enabled = FALSE;
        }
        g_strfreev(pair);
    }
    g_strfreev(lines);
    lines = g_strsplit(input, "\n", -1);
    for (gchar **line = lines; *line; ++line) {
        gchar **pair = g_strsplit(*line, ":", 2);
        /* The vendor RX ABI spells ST2084 as ST_2048. */
        if (pair[1] && g_str_equal(g_strstrip(pair[0]), "HDR EOTF") &&
            g_str_equal(g_strstrip(pair[1]), "SMPTE_ST_2048")) pq = TRUE;
        g_strfreev(pair);
    }
    g_strfreev(lines);
    return enabled && pq;
}

static gboolean use_hdr(void)
{
    gchar *settings = NULL, *input = NULL;
    gboolean hdr = hdr_capable() &&
        g_file_get_contents("/etc/default/vim4-kvm-hdr", &settings, NULL, NULL) &&
        g_file_get_contents("/sys/class/hdmirx/hdmirx0/info", &input, NULL, NULL) &&
        preserve_hdr(settings, input);
    g_free(settings);
    g_free(input);
    return hdr;
}

static gchar *description(GstVim4H265Src *self, gboolean hdr)
{
    if (hdr) {
        /* The capture converter derives the even aspect-preserving width and
         * keeps resized P010 in contiguous DMA. A system-memory videoscale
         * here would also force the SDK's slow CPU upload. */
        guint height = self->height ? MAX(self->height & ~1u, 2) : 0;
        return g_strdup_printf(
            "vfmcapsrc name=capture height=%u ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! "
            "videorate drop-only=true ! video/x-raw,format=P010_10LE,framerate=%u/1 ! "
            "vim4hevc10enc name=encoder bitrate=%u framerate=%u ! h265parse config-interval=-1 ! "
            "capsfilter caps=\"video/x-h265,stream-format=byte-stream,alignment=au,profile=main-10\"",
            height, self->framerate, self->bitrate, self->framerate);
    }
    return g_strdup_printf(
        "vim4vdin1src height=%u ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! "
        "videorate drop-only=true ! video/x-raw,framerate=%u/1 ! "
        "amlvenc bitrate=%u framerate=%u gop=%u qp-max=%d min-force-key-unit-interval=250000000 ! "
        "h265parse config-interval=-1 ! capsfilter caps=\"video/x-h265,stream-format=byte-stream,alignment=au\"",
        self->height, self->framerate, self->bitrate, self->framerate, self->gop, self->qp_max);
}

static void release_bootstrap(GstVim4H265Src *self)
{
    if (self->bootstrap >= 0) {
        close(self->bootstrap);
        self->bootstrap = -1;
    }
}

static GstPadProbeReturn encoded_frame(GstPad *pad, GstPadProbeInfo *probe, gpointer data)
{
    GstVim4H265Src *self = data;
    /* The SDK now owns the reservation. Keeping this fd past final SDK close
     * would prevent a clean reset when switching between the two codec ABIs. */
    release_bootstrap(self);
    gint64 now = g_get_monotonic_time();
    if (!self->changing && now - self->checked_at >= G_USEC_PER_SEC) {
        self->checked_at = now;
        if (use_hdr() != self->hdr) {
            self->changing = TRUE;
            /* All consumers already rebuild capture on a bus error. Reopen
             * after old DMA inputs retire; do not mutate a running SDK. */
            GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
                ("HDR input or global mapping changed; reopen capture"), (NULL));
        }
    }
    return self->changing ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
}

static void clear_chain(GstVim4H265Src *self)
{
    gst_ghost_pad_set_target(GST_GHOST_PAD(self->src), NULL);
    if (self->chain) {
        /* A failed NULL->READY may leave earlier children in READY, holding
         * capture/codec resources. Stop them before dropping bin ownership. */
        gst_element_set_state(self->chain, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(self), self->chain);
        self->chain = NULL;
    }
    release_bootstrap(self);
}

static GstStateChangeReturn change_state(GstElement *element, GstStateChange transition)
{
    GstVim4H265Src *self = (GstVim4H265Src *)element;
    if (transition == GST_STATE_CHANGE_NULL_TO_READY) {
        if (g_strcmp0(g_getenv("VIM4_SECONDARY_HDMI_CAPTURE"), "1") != 0 || !hdr_capable()) {
            GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
                ("Shared HEVC capture requires the native HDR 5.15 runtime"), (NULL));
            return GST_STATE_CHANGE_FAILURE;
        }
        /* Reserve the 200 MiB encoder workspace before GPU or VDIN1 pools. */
        self->bootstrap = open("/dev/amvenc_multi", O_RDWR | O_CLOEXEC);
        if (self->bootstrap < 0) {
            GST_ELEMENT_ERROR(self, RESOURCE, OPEN_READ_WRITE,
                ("Cannot reserve vendor encoder memory"), (NULL));
            return GST_STATE_CHANGE_FAILURE;
        }
        self->hdr = use_hdr();
        self->changing = FALSE;
        self->checked_at = g_get_monotonic_time();
        gchar *text = description(self, self->hdr);
        GError *error = NULL;
        self->chain = gst_parse_bin_from_description(text, TRUE, &error);
        g_free(text);
        if (error || !self->chain) {
            GST_ELEMENT_ERROR(self, CORE, MISSING_PLUGIN,
                ("Cannot construct shared HEVC capture"), ("%s", error ? error->message : "empty bin"));
            g_clear_error(&error);
            if (self->chain) gst_object_unref(self->chain);
            self->chain = NULL;
            release_bootstrap(self);
            return GST_STATE_CHANGE_FAILURE;
        }
        gst_bin_add(GST_BIN(self), self->chain);
        GstPad *src = gst_element_get_static_pad(self->chain, "src");
        gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, encoded_frame, self, NULL);
        gboolean linked = gst_ghost_pad_set_target(GST_GHOST_PAD(self->src), src);
        gst_object_unref(src);
        if (!linked) {
            clear_chain(self);
            return GST_STATE_CHANGE_FAILURE;
        }
    }
    GstStateChangeReturn result = GST_ELEMENT_CLASS(gst_vim4_h265_src_parent_class)->change_state(element, transition);
    if (transition == GST_STATE_CHANGE_READY_TO_NULL ||
        (transition == GST_STATE_CHANGE_NULL_TO_READY && result == GST_STATE_CHANGE_FAILURE))
        clear_chain(self);
    return result;
}

static void set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
    GstVim4H265Src *self = (GstVim4H265Src *)object;
    switch (id) {
    case PROP_BITRATE: self->bitrate = g_value_get_uint(value); break;
    case PROP_FRAMERATE: self->framerate = g_value_get_uint(value); break;
    case PROP_GOP: self->gop = g_value_get_uint(value); break;
    case PROP_HEIGHT: self->height = g_value_get_uint(value); break;
    case PROP_QP_MAX: self->qp_max = g_value_get_int(value); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    }
}

static void get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
    GstVim4H265Src *self = (GstVim4H265Src *)object;
    switch (id) {
    case PROP_BITRATE: g_value_set_uint(value, self->bitrate); break;
    case PROP_FRAMERATE: g_value_set_uint(value, self->framerate); break;
    case PROP_GOP: g_value_set_uint(value, self->gop); break;
    case PROP_HEIGHT: g_value_set_uint(value, self->height); break;
    case PROP_QP_MAX: g_value_set_int(value, self->qp_max); break;
    case PROP_HDR_CAPABLE: g_value_set_int(value, hdr_capable()); break;
    case PROP_HDR_REQUESTED: g_value_set_int(value, use_hdr()); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    }
}

static void gst_vim4_h265_src_init(GstVim4H265Src *self)
{
    self->bitrate = 12000;
    self->framerate = 60;
    self->gop = 120;
    self->qp_max = -1;
    self->bootstrap = -1;
    self->src = gst_ghost_pad_new_no_target("src", GST_PAD_SRC);
    gst_element_add_pad(GST_ELEMENT(self), self->src);
    GST_OBJECT_FLAG_SET(self, GST_ELEMENT_FLAG_SOURCE);
}

static void gst_vim4_h265_src_class_init(GstVim4H265SrcClass *klass)
{
    GObjectClass *object = G_OBJECT_CLASS(klass);
    GstElementClass *element = GST_ELEMENT_CLASS(klass);
    object->set_property = set_property;
    object->get_property = get_property;
    element->change_state = change_state;
    const GParamFlags flags = G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY;
    g_object_class_install_property(object, PROP_BITRATE, g_param_spec_uint(
        "bitrate", "Bitrate", "Target kbps", 1000, 60000, 12000, flags));
    g_object_class_install_property(object, PROP_FRAMERATE, g_param_spec_uint(
        "framerate", "Framerate", "Drop-only output and encoder budget", 1, 120, 60, flags));
    g_object_class_install_property(object, PROP_GOP, g_param_spec_uint(
        "gop", "GOP", "SDR GOP; Main10 uses gradual refresh and explicit IDR", 0, 1000, 120, flags));
    g_object_class_install_property(object, PROP_HEIGHT, g_param_spec_uint(
        "height", "Height", "Output height; zero preserves input geometry", 0, 2160, 0, flags));
    g_object_class_install_property(object, PROP_QP_MAX, g_param_spec_int(
        "qp-max", "Maximum QP", "SDR quality ceiling; Main10 uses target bitrate", -1, 51, -1, flags));
    g_object_class_install_property(object, PROP_HDR_CAPABLE, g_param_spec_int(
        "hdr-capable", "HDR capable", "Native HDR runtime available without opening hardware", 0, 1, 0,
        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(object, PROP_HDR_REQUESTED, g_param_spec_int(
        "hdr-requested", "HDR requested", "Current input and global policy select Main10; opens no hardware", 0, 1, 0,
        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
    GstCaps *caps = gst_caps_from_string("video/x-h265,stream-format=byte-stream,alignment=au");
    gst_element_class_add_pad_template(element, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
    gst_caps_unref(caps);
    gst_element_class_set_static_metadata(element, "VIM4 shared HEVC capture", "Source/Video",
        "Global HDR mapping with independent Main10 or SDR capture", "vim4-kvm");
}
