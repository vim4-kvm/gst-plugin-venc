/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include <gst/gst.h>
#include <linux/videodev2.h>
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

static inline gboolean vim4_input_timings(struct v4l2_dv_timings *timings)
{
    gchar *path = vim4_video_device("t7-vdin-test");
    if (!path) return FALSE;
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    g_free(path);
    if (fd < 0) return FALSE;
    memset(timings, 0, sizeof(*timings));
    gboolean ok = ioctl(fd, VIDIOC_QUERY_DV_TIMINGS, timings) == 0 &&
        timings->type == V4L2_DV_BT_656_1120 && !timings->bt.interlaced;
    close(fd);
    return ok;
}

static inline guint vim4_input_rate(void)
{
    struct v4l2_dv_timings timings;
    if (!vim4_input_timings(&timings)) return 0;
    const struct v4l2_bt_timings *bt = &timings.bt;
    guint64 width = (guint64)bt->width + bt->hfrontporch + bt->hsync + bt->hbackporch;
    guint64 height = (guint64)bt->height + bt->vfrontporch + bt->vsync + bt->vbackporch;
    return width && height ? (bt->pixelclock * 100 + width * height / 2) / (width * height) : 0;
}
