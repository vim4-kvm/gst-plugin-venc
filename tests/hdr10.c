/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../hdr/hdr10.h"
#include <sys/ioctl.h>
#include <string.h>

static guint fault, calls;
static int emulate(int fd, unsigned long request, void *argument)
{
    (void)fd;
    calls++;
    if (fault == calls) return -1;
    switch (request) {
    case VIDIOC_ENUM_FMT: {
        struct v4l2_fmtdesc *entry = argument;
        if (entry->index > 1) return -1;
        entry->pixelformat = entry->index ? v4l2_fourcc('A','M','L','Y') : V4L2_PIX_FMT_NV12;
        return 0;
    }
    case VIDIOC_QUERY_EXT_CTRL: {
        struct v4l2_query_ext_ctrl *ctrl = argument;
        ctrl->elems = 1;
        ctrl->elem_size = ctrl->id == V4L2_CID_COLORIMETRY_HDR10_CLL_INFO ? 4 : 24;
        if (fault == 10) ctrl->elem_size++;
        if (fault == 15) ctrl->flags |= V4L2_CTRL_FLAG_DISABLED;
        if (fault == 16) ctrl->elems = 0;
        return 0;
    }
    case VIDIOC_S_CTRL:
        if (fault == 17) ((struct v4l2_control *)argument)->value = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN;
        return 0;
    case VIDIOC_S_FMT: {
        struct v4l2_format *format = argument;
        if (format->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            struct v4l2_pix_format_mplane *raw = &format->fmt.pix_mp;
            raw->num_planes = 1; raw->plane_fmt[0].bytesperline = 1280;
            raw->plane_fmt[0].sizeimage = 1280 * 480 * 3 / 2;
            /* Exercise drivers silently substituting eight-bit/default VUI. */
            if (fault == 11) raw->pixelformat = V4L2_PIX_FMT_NV12;
            if (fault == 12) raw->xfer_func = V4L2_XFER_FUNC_709;
            if (fault == 13) raw->plane_fmt[0].sizeimage--;
            if (fault == 14) raw->quantization = V4L2_QUANTIZATION_FULL_RANGE;
        }
        return 0;
    }
    default: g_error("Probe unexpectedly issued request %lu", request);
    }
    return -1;
}

static void probe_contract(void)
{
    fault = calls = 0;
    g_assert_true(vim4_hdr10_probe(1, 2, emulate));
    const guint count = calls;
    for (guint i = 1; i <= count; i++) {
        calls = 0; fault = i;
        g_assert_false(vim4_hdr10_probe(1, 2, emulate));
    }
    for (guint i = 10; i <= 17; i++) {
        calls = 0; fault = i;
        g_assert_false(vim4_hdr10_probe(1, 2, emulate));
    }
}

static GstBuffer *bytes(const guint8 *data, gsize size)
{ return gst_buffer_new_wrapped(g_memdup2(data, size), size); }

static void equals(GstBuffer *buffer, const guint8 *expected, gsize size)
{
    g_assert_nonnull(buffer);
    GstMapInfo map;
    g_assert_true(gst_buffer_map(buffer, &map, GST_MAP_READ));
    g_assert_cmpmem(map.data, map.size, expected, size);
    gst_buffer_unmap(buffer, &map);
}

static void prefix_sei(void)
{
    const guint8 idr[] = {0,0,0,1,0x26,1,0xaf};
    GstBuffer *input = bytes(idr, sizeof(idr));
    GST_BUFFER_PTS(input) = 7 * GST_SECOND;
    Vim4Hdr10 metadata;
    for (guint i = 0; i < sizeof(metadata.mastering); i++) metadata.mastering[i] = i + 1;
    memcpy(metadata.cll, (guint8[]){0x03, 0xe8, 0x01, 0x90}, 4);
    /* Independent expected HEVC prefix SEI: type137/24B then144/4B, BE. */
    const guint8 expected[] = {0,0,0,1,0x4e,1,137,24,
        1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,
        144,4,0x03,0xe8,0x01,0x90,0x80,0,0,0,1,0x26,1,0xaf};
    GstBuffer *output = vim4_hdr10_insert(input, &metadata, FALSE);
    equals(output, expected, sizeof(expected));
    g_assert_cmpuint(GST_BUFFER_PTS(output), ==, GST_BUFFER_PTS(input));
    /* Already inserted metadata is replaced, never accumulated on an IRAP. */
    GstBuffer *again = vim4_hdr10_insert(output, &metadata, FALSE);
    equals(again, expected, sizeof(expected));
    gst_buffer_unref(again); gst_buffer_unref(output); gst_buffer_unref(input);

    const guint8 delta[] = {0,0,1,2,1,0xaf};
    const guint8 normalized[] = {0,0,0,1,2,1,0xaf};
    input = bytes(delta, sizeof(delta));
    output = vim4_hdr10_insert(input, &metadata, FALSE);
    equals(output, normalized, sizeof(normalized));
    gst_buffer_unref(output);
    output = vim4_hdr10_insert(input, &metadata, TRUE);
    g_assert_cmpuint(gst_buffer_get_size(output), >, sizeof(delta));
    gst_buffer_unref(output); gst_buffer_unref(input);
}

static void mixed_and_invalid_sei(void)
{
    Vim4Hdr10 metadata = {0};
    const guint8 mixed[] = {0,0,0,1,0x4e,1,137,1,7,5,1,0xaa,144,1,9,0x80,0,0,0,1,2,1,0xaf};
    const guint8 expected[] = {0,0,0,1,0x4e,1,5,1,0xaa,0x80,0,0,0,1,2,1,0xaf};
    GstBuffer *input = bytes(mixed, sizeof(mixed));
    GstBuffer *output = vim4_hdr10_insert(input, &metadata, FALSE);
    equals(output, expected, sizeof(expected));
    gst_buffer_unref(output); gst_buffer_unref(input);
    /* Truncated payload must not produce a seemingly valid replacement AU. */
    const guint8 invalid[] = {0,0,1,0x4e,1,137,24,1,0x80};
    input = bytes(invalid, sizeof(invalid));
    g_assert_null(vim4_hdr10_insert(input, &metadata, TRUE));
    gst_buffer_unref(input);
    input = bytes((guint8[]){0,0,1,0x26}, 4);
    g_assert_null(vim4_hdr10_insert(input, &metadata, TRUE));
    gst_buffer_unref(input);
}

int main(int argc, char **argv)
{
    gst_init(&argc, &argv); g_test_init(&argc, &argv, NULL);
    g_test_add_func("/hdr10/bufferless-negotiation", probe_contract);
    g_test_add_func("/hdr10/prefix-sei", prefix_sei);
    g_test_add_func("/hdr10/mixed-invalid-sei", mixed_and_invalid_sei);
    return g_test_run();
}
