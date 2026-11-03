/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <fcntl.h>
#include <unistd.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include "hdr10.h"
#include "../wave5/devices.h"

/* One encoded source owns capture and encoding so all applications use the
 * same HDR policy, contiguous-memory bootstrap and teardown order. JPEG stays
 * on VDIN1 and must never consume these raw PQ frames. */
typedef struct {
    GstBin parent;
    GstElement *chain;
    GstPad *src, *raw;
    guint bitrate, framerate, gop, height;
    gint qp_max, bootstrap, metadata_fd;
    gboolean hdr, changing, wave5, have_metadata, waiting_irap, recovery_key_requested;
    Vim4Hdr10 metadata;
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

static int video_ioctl(int fd, unsigned long request, void *argument)
{ return ioctl(fd, request, argument); }

static gboolean factory_p010(GstElementFactory *factory, GstPadDirection direction)
{
    GstCaps *required = gst_caps_from_string("video/x-raw,format=P010_10LE");
    gboolean found = FALSE;
    for (const GList *item = gst_element_factory_get_static_pad_templates(factory); item; item = item->next) {
        GstStaticPadTemplate *pad = item->data;
        if (pad->direction != direction) continue;
        GstCaps *caps = gst_static_pad_template_get_caps(pad);
        found |= gst_caps_can_intersect(caps, required);
        gst_caps_unref(caps);
    }
    gst_caps_unref(required);
    return found;
}

static gboolean hdr_capable(void)
{
    if (!g_strcmp0(g_getenv("VIM4_VIDEO_BACKEND"), "wave5")) {
        /* The encoder is fixed for the lifetime of this process. Never open
         * a new encoder context on the once-per-second policy poll. */
        static gsize probed;
        static gboolean supported;
        if (g_once_init_enter(&probed)) {
            const gchar *factories[] = {"vim4amlysrc", "vim4wave5enc", "v4l2h265enc"};
            gboolean plugins = TRUE;
            for (guint i = 0; i < G_N_ELEMENTS(factories); i++) {
                GstElementFactory *factory = gst_element_factory_find(factories[i]);
                plugins &= factory != NULL;
                if (factory && i == 0) plugins &= factory_p010(factory, GST_PAD_SRC);
                if (factory && i == 1) plugins &= !g_strcmp0(
                    gst_element_factory_get_metadata(factory, "vim4-wave5-main10-api"), "1");
                if (factory && i == 2) plugins &= factory_p010(factory, GST_PAD_SINK);
                if (factory) gst_object_unref(factory);
            }
            gchar *capture = vim4_video_device("t7-vdin-test"), *encoder = vim4_encoder_device();
            int cap = capture ? open(capture, O_RDONLY | O_CLOEXEC | O_NONBLOCK) : -1;
            int enc = plugins && encoder ? open(encoder, O_RDWR | O_CLOEXEC | O_NONBLOCK) : -1;
            supported = cap >= 0 && enc >= 0 && vim4_hdr10_probe(cap, enc, video_ioctl);
            if (cap >= 0) close(cap);
            if (enc >= 0) close(enc);
            g_free(capture); g_free(encoder);
            g_once_init_leave(&probed, 1);
        }
        return supported;
    }
    return
        !g_strcmp0(g_getenv("VIM4_SECONDARY_HDMI_CAPTURE"), "1") &&
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
    if (!g_strcmp0(g_getenv("VIM4_VIDEO_BACKEND"), "wave5")) {
        gchar *device = vim4_video_device("t7-vdin-test");
        int fd = device ? open(device, O_RDONLY | O_CLOEXEC | O_NONBLOCK) : -1;
        Vim4Hdr10 metadata;
        /* The capture control returns ENODATA outside fresh PQ input. Zero
         * metadata values remain valid; they are never replaced by defaults. */
        gboolean hdr = hdr_capable() && fd >= 0 && vim4_hdr10_read(fd, &metadata) &&
            g_file_get_contents("/etc/default/vim4-kvm-hdr", &settings, NULL, NULL) &&
            preserve_hdr(settings, "HDR EOTF: SMPTE_ST_2048\n");
        if (fd >= 0) close(fd);
        g_free(device); g_free(settings);
        return hdr;
    }
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
    guint height = self->height ? MAX(self->height & ~1u, 2) : 0;
    if (self->wave5) {
        guint fps = hdr ? MIN(self->framerate, 60u) : self->framerate;
        g_autofree gchar *capture = hdr ?
            g_strdup_printf("vim4amlysrc name=capture height=%u", height) :
            g_strdup_printf("vim4v4l2src name=capture height=%u max-fps=%u", height, fps);
        g_autofree gchar *raw_caps = hdr ?
            g_strdup_printf("videorate ! video/x-raw,format=P010_10LE,framerate=%u/1,colorimetry=bt2100-pq", fps) :
            g_strdup("video/x-raw,format=NV12");
        return g_strdup_printf(
            "%s ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! %s ! "
            "tee name=raw_capture ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! "
            "vim4wave5enc name=encoder bitrate=%u gop=%u framerate=%u ! "
            "video/x-h265,profile=%s ! h265parse config-interval=-1 ! "
            "capsfilter caps=\"video/x-h265,stream-format=byte-stream,alignment=au,profile=%s\"",
            capture, raw_caps, self->bitrate,
            self->gop, fps, hdr ? "main-10" : "main", hdr ? "main-10" : "main");
    }
    if (hdr) {
        /* The capture converter derives the even aspect-preserving width and
         * keeps resized P010 in contiguous DMA. A system-memory videoscale
         * here would also force the SDK's slow CPU upload. */
        return g_strdup_printf(
            "vfmcapsrc name=capture height=%u ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! "
            "videorate drop-only=true ! video/x-raw,format=P010_10LE,framerate=%u/1 ! "
            "vim4hevc10enc name=encoder bitrate=%u framerate=%u ! h265parse config-interval=-1 ! "
            "capsfilter caps=\"video/x-h265,stream-format=byte-stream,alignment=au,profile=main-10\"",
            height, self->framerate, self->bitrate, self->framerate);
    }
    /* VDIN1 rounds scaled widths down to a canvas multiple. Keep its native
     * visible width and let GE2D preserve aspect ratio for arbitrary heights
     * and ultrawide inputs, using the source's actual VideoMeta pitch. */
    gchar *scale = height ? g_strdup_printf(
        "vim4scale name=scaler height=%u width-alignment=16 ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! ",
        height) : g_strdup("");
    gchar *text = g_strdup_printf(
        "vim4vdin1src name=capture ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! "
        "videorate drop-only=true ! video/x-raw,framerate=%u/1 ! "
        "%s"
        "amlvenc bitrate=%u framerate=%u gop=%u qp-max=%d min-force-key-unit-interval=250000000 ! "
        "h265parse config-interval=-1 ! capsfilter caps=\"video/x-h265,stream-format=byte-stream,alignment=au\"",
        self->framerate, scale, self->bitrate, self->framerate, self->gop, self->qp_max);
    g_free(scale);
    return text;
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
    if (self->changing) return GST_PAD_PROBE_DROP;
    if (self->wave5 && self->hdr) {
        Vim4Hdr10 metadata;
        if (!vim4_hdr10_read(self->metadata_fd, &metadata)) {
            /* The encoder has advanced even though this AU cannot be sent.
             * Its successors may reference the missing picture. */
            self->waiting_irap = TRUE;
            self->recovery_key_requested = self->have_metadata = FALSE;
            return GST_PAD_PROBE_DROP;
        }
        GstBuffer *original = GST_PAD_PROBE_INFO_BUFFER(probe);
        if (self->waiting_irap && !vim4_hdr10_is_irap(original)) {
            if (!self->recovery_key_requested) {
                self->recovery_key_requested = TRUE;
                gst_pad_send_event(pad, gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0));
            }
            return GST_PAD_PROBE_DROP;
        }
        GstBuffer *with_sei = vim4_hdr10_insert(original, &metadata,
            !self->have_metadata || memcmp(&metadata, &self->metadata, sizeof(metadata)));
        if (!with_sei) {
            self->waiting_irap = TRUE;
            self->recovery_key_requested = self->have_metadata = FALSE;
            GST_ELEMENT_ERROR(self, STREAM, DECODE, ("Invalid HEVC access unit or HDR SEI"), (NULL));
            return GST_PAD_PROBE_DROP;
        }
        if (self->waiting_irap) GST_BUFFER_FLAG_SET(with_sei, GST_BUFFER_FLAG_DISCONT);
        self->waiting_irap = self->recovery_key_requested = FALSE;
        self->metadata = metadata; self->have_metadata = TRUE;
        GST_PAD_PROBE_INFO_DATA(probe) = with_sei;
        gst_buffer_unref(original);
    }
    return GST_PAD_PROBE_OK;
}

static void clear_chain(GstVim4H265Src *self)
{
    gst_ghost_pad_set_target(GST_GHOST_PAD(self->src), NULL);
    if (self->raw) gst_ghost_pad_set_target(GST_GHOST_PAD(self->raw), NULL);
    if (self->chain) {
        /* A failed NULL->READY may leave earlier children in READY, holding
         * capture/codec resources. Stop them before dropping bin ownership. */
        gst_element_set_state(self->chain, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(self), self->chain);
        self->chain = NULL;
    }
    release_bootstrap(self);
    if (self->metadata_fd >= 0) close(self->metadata_fd);
    self->metadata_fd = -1;
    self->have_metadata = FALSE;
    self->waiting_irap = self->recovery_key_requested = FALSE;
}

static GstPadProbeReturn capture_allocation(GstPad *pad, GstPadProbeInfo *probe, gpointer data)
{
    GstQuery *query = GST_PAD_PROBE_INFO_QUERY(probe);
    if (GST_QUERY_TYPE(query) != GST_QUERY_ALLOCATION) return GST_PAD_PROBE_OK;
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
    /* Same proven HDR2 lease budget as One-KVM: queues, screenshot and encoder
     * commands retain capture buffers. v4l2src adds its two working slots. */
    gst_query_add_allocation_pool(query, NULL, 0, 12, 12);
    return GST_PAD_PROBE_HANDLED;
}

static GstStateChangeReturn change_state(GstElement *element, GstStateChange transition)
{
    GstVim4H265Src *self = (GstVim4H265Src *)element;
    if (transition == GST_STATE_CHANGE_NULL_TO_READY) {
        self->wave5 = !g_strcmp0(g_getenv("VIM4_VIDEO_BACKEND"), "wave5");
        if ((!self->wave5 && g_strcmp0(g_getenv("VIM4_SECONDARY_HDMI_CAPTURE"), "1") != 0) || !hdr_capable()) {
            GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
                ("Shared HEVC capture requires qualified capture, Main10 and plugins"), (NULL));
            return GST_STATE_CHANGE_FAILURE;
        }
        /* Reserve the 200 MiB encoder workspace before GPU or VDIN1 pools. */
        if (!self->wave5) self->bootstrap = open("/dev/amvenc_multi", O_RDWR | O_CLOEXEC);
        if (!self->wave5 && self->bootstrap < 0) {
            GST_ELEMENT_ERROR(self, RESOURCE, OPEN_READ_WRITE,
                ("Cannot reserve vendor encoder memory"), (NULL));
            return GST_STATE_CHANGE_FAILURE;
        }
        self->hdr = use_hdr();
        if (self->wave5 && self->hdr) {
            gchar *device = vim4_video_device("t7-vdin-test");
            self->metadata_fd = device ? open(device, O_RDONLY | O_CLOEXEC | O_NONBLOCK) : -1;
            g_free(device);
            if (self->metadata_fd < 0) return GST_STATE_CHANGE_FAILURE;
        }
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
            clear_chain(self);
            return GST_STATE_CHANGE_FAILURE;
        }
        gst_bin_add(GST_BIN(self), self->chain);
        if (self->wave5 && !self->hdr) {
            GstElement *capture = gst_bin_get_by_name(GST_BIN(self->chain), "capture");
            GstPad *src = capture ? gst_element_get_static_pad(capture, "src") : NULL;
            GstPad *downstream = src ? gst_pad_get_peer(src) : NULL;
            if (downstream) gst_pad_add_probe(downstream, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
                capture_allocation, NULL, NULL);
            if (downstream) gst_object_unref(downstream);
            if (src) gst_object_unref(src);
            if (capture) gst_object_unref(capture);
            if (!downstream) { clear_chain(self); return GST_STATE_CHANGE_FAILURE; }
        }
        if (self->raw) {
            GstElement *tee = gst_bin_get_by_name(GST_BIN(self->chain), "raw_capture");
            GstPad *raw = tee ? gst_element_request_pad_simple(tee, "src_%u") : NULL;
            gboolean connected = raw && gst_ghost_pad_set_target(GST_GHOST_PAD(self->raw), raw);
            if (raw) gst_object_unref(raw);
            if (tee) gst_object_unref(tee);
            if (!connected) { clear_chain(self); return GST_STATE_CHANGE_FAILURE; }
        }
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
    self->metadata_fd = -1;
    self->src = gst_ghost_pad_new_no_target("src", GST_PAD_SRC);
    gst_element_add_pad(GST_ELEMENT(self), self->src);
    GST_OBJECT_FLAG_SET(self, GST_ELEMENT_FLAG_SOURCE);
}

static GstPad *request_pad(GstElement *element, GstPadTemplate *template,
                          const gchar *name, const GstCaps *caps)
{
    GstVim4H265Src *self = (GstVim4H265Src *)element;
    /* Request before READY, so one capture pipeline owns both raw and HEVC.
     * Raw is PQ P010 in native mode, NV12 in mapping mode. Consumers must use
     * vim4pq2sdr before JPEG; the encoder branch never sees mapped pixels. */
    if (self->raw || self->chain || g_strcmp0(g_getenv("VIM4_VIDEO_BACKEND"), "wave5")) return NULL;
    self->raw = gst_ghost_pad_new_no_target_from_template("raw", template);
    if (!gst_element_add_pad(element, self->raw)) { gst_object_unref(self->raw); self->raw = NULL; }
    return self->raw;
}
static void release_pad(GstElement *element, GstPad *pad)
{
    GstVim4H265Src *self = (GstVim4H265Src *)element;
    if (pad != self->raw) return;
    gst_ghost_pad_set_target(GST_GHOST_PAD(pad), NULL);
    gst_element_remove_pad(element, pad); self->raw = NULL;
}

static void gst_vim4_h265_src_class_init(GstVim4H265SrcClass *klass)
{
    GObjectClass *object = G_OBJECT_CLASS(klass);
    GstElementClass *element = GST_ELEMENT_CLASS(klass);
    object->set_property = set_property;
    object->get_property = get_property;
    element->change_state = change_state;
    element->request_new_pad = request_pad; element->release_pad = release_pad;
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
        "hdr-capable", "HDR capable", "Native HDR bufferless capability probe; no streaming", 0, 1, 0,
        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(object, PROP_HDR_REQUESTED, g_param_spec_int(
        "hdr-requested", "HDR requested", "Fresh PQ input and global policy select Main10; no streaming", 0, 1, 0,
        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
    GstCaps *caps = gst_caps_from_string("video/x-h265,stream-format=byte-stream,alignment=au");
    gst_element_class_add_pad_template(element, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
    gst_caps_unref(caps);
    caps = gst_caps_from_string("video/x-raw,format=(string){NV12,P010_10LE}");
    gst_element_class_add_pad_template(element, gst_pad_template_new("raw", GST_PAD_SRC, GST_PAD_REQUEST, caps));
    gst_caps_unref(caps);
    gst_element_class_set_static_metadata(element, "VIM4 shared HEVC capture", "Source/Video",
        "Global HDR mapping with independent Main10 or SDR capture", "vim4-kvm");
}
