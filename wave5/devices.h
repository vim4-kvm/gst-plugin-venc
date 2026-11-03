/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include <gst/gst.h>
#include <linux/videodev2.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Video node numbers depend on probe order, including unrelated USB devices. */
static inline gchar *vim4_video_device(const gchar *name)
{
    GDir *directory = g_dir_open("/sys/class/video4linux", 0, NULL);
    if (!directory) return NULL;
    gchar *result = NULL;
    const gchar *entry;
    while ((entry = g_dir_read_name(directory))) {
        gchar *path = g_build_filename("/sys/class/video4linux", entry, "name", NULL);
        gchar *label = NULL;
        if (g_file_get_contents(path, &label, NULL, NULL) && !strcmp(g_strstrip(label), name)) {
            if (result) {
                g_clear_pointer(&result, g_free);
                g_free(label); g_free(path);
                break;
            }
            result = g_build_filename("/dev", entry, NULL);
        }
        g_free(label); g_free(path);
    }
    g_dir_close(directory);
    return result;
}

/* The sysfs node name and QUERYCAP labels are distinct, unstable identifiers.
 * Keep legacy kernels eligible: crop support is probed separately by V4L2. */
static inline gchar *vim4_encoder_device(void)
{
    GDir *directory = g_dir_open("/sys/class/video4linux", 0, NULL);
    if (!directory) return NULL;
    gchar *result = NULL;
    const gchar *entry;
    while ((entry = g_dir_read_name(directory))) {
        gchar *path = g_build_filename("/dev", entry, NULL);
        int fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
        struct v4l2_capability cap = {0};
        gboolean matched = FALSE;
        if (fd >= 0 && ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            guint32 caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
            if ((caps & (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING)) ==
                (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING)) {
                struct v4l2_fmtdesc fmt = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
                while (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0) {
                    if (fmt.pixelformat == V4L2_PIX_FMT_HEVC) { matched = TRUE; break; }
                    fmt.index++;
                }
            }
        }
        if (fd >= 0) close(fd);
        if (matched && result) { g_free(path); g_clear_pointer(&result, g_free); break; }
        if (matched) result = path;
        else g_free(path);
    }
    g_dir_close(directory);
    return result;
}

/* Keep this fallback for builds using distribution UAPI headers. */
#ifndef V4L2_CID_USER_T7_VDIN_VRR_RATE
#define V4L2_CID_USER_T7_VDIN_VRR_RATE (V4L2_CID_USER_BASE + 0x1240)
#endif

typedef struct {
    struct v4l2_dv_timings timings;
    gint num, den;       /* Measured fixed rate, or 0/1 for Gaming VRR. */
    gint vrr;            /* 0 fixed, -1 unknown base, positive base in Hz. */
    gboolean enumerated; /* v4l2src can negotiate its own measured fraction. */
} Vim4Input;

static inline gboolean vim4_input_timings(const gchar *path, Vim4Input *input, gboolean probe_intervals)
{
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return FALSE;
    *input = (Vim4Input){.den = 1};
    struct v4l2_control ctrl = {.id = V4L2_CID_USER_T7_VDIN_VRR_RATE};
    gboolean ok = ioctl(fd, VIDIOC_QUERY_DV_TIMINGS, &input->timings) == 0 &&
        input->timings.type == V4L2_DV_BT_656_1120 && !input->timings.bt.interlaced;
    if (ok && ioctl(fd, VIDIOC_G_CTRL, &ctrl) == 0)
        input->vrr = ctrl.value;
    else if (ok && errno != ENOTTY && errno != EINVAL)
        ok = FALSE;
    if (ok && !input->vrr && probe_intervals) {
        struct v4l2_frmivalenum interval = {
            .pixel_format = V4L2_PIX_FMT_NV12,
            .width = input->timings.bt.width, .height = input->timings.bt.height,
        };
        input->enumerated = ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == 0 &&
            interval.type == V4L2_FRMIVAL_TYPE_DISCRETE;
    }
    close(fd);
    if (!ok || input->vrr) return ok;
    const struct v4l2_bt_timings *bt = &input->timings.bt;
    guint64 width = (guint64)bt->width + bt->hfrontporch + bt->hsync + bt->hbackporch;
    guint64 height = (guint64)bt->height + bt->vfrontporch + bt->vsync + bt->vbackporch;
    if (!width || !height) return FALSE;
    gdouble rate = (gdouble)bt->pixelclock / width / height;
    if (rate < 20 || rate > 120.001) return FALSE;
    gst_util_double_to_fraction(MIN(rate, 120.0), &input->num, &input->den);
    return TRUE;
}

static inline guint vim4_input_budget(const Vim4Input *input)
{
    /* Unknown Gaming VRR base: use the local 60 fps output policy. */
    if (input->vrr) return (input->vrr > 0 ? input->vrr : 60) * 100;
    return gst_util_uint64_scale_round(input->num, 100, input->den);
}

static inline guint vim4_input_rate(void)
{
    g_autofree gchar *path = vim4_video_device("t7-vdin-test");
    Vim4Input input;
    if (!path || !vim4_input_timings(path, &input, FALSE)) return 0;
    return vim4_input_budget(&input);
}

/* The V4L2 source can only see the sampled frame interval. Relabel VRR at
 * the capture boundary; the metadata operation leaves the DMA buffer and PTS
 * untouched. A fixed source below the ceiling needs no rate element. */
static inline gchar *vim4_capture_rate_chain(const Vim4Input *input, guint ceiling)
{
    GString *chain = g_string_new(NULL);
    if (input->vrr) {
        g_string_append(chain, " ! capssetter caps=\"video/x-raw,framerate=(fraction)0/1");
        if (input->vrr > 0)
            g_string_append_printf(chain, ",max-framerate=(fraction)%d/1", input->vrr);
        g_string_append_c(chain, '"');
        if (input->vrr < 0) {
            guint budget = vim4_input_budget(input) / 100;
            ceiling = ceiling ? MIN(ceiling, budget) : budget;
        }
        if (ceiling && (input->vrr < 0 || (guint)input->vrr > ceiling))
            g_string_append_printf(chain, " ! videorate drop-only=true average-period=1 max-rate=%u ! "
                "video/x-raw,framerate=%u/1 ! capssetter caps=\"video/x-raw,"
                "framerate=(fraction)0/1,max-framerate=(fraction)%u/1\"", ceiling, ceiling, ceiling);
    } else {
        /* Two readings of the RX counter may differ by a tick. On kernels
         * with enumeration let v4l2src use the fraction from its own probe. */
        if (!input->enumerated)
            g_string_append_printf(chain, ",framerate=%d/%d", input->num, input->den);
        if (ceiling && input->num > (gint64)ceiling * input->den)
            g_string_append_printf(chain, " ! videorate drop-only=true max-rate=%u", ceiling);
    }
    return g_string_free(chain, FALSE);
}
