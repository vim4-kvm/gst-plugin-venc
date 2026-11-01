/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "hdr10.h"
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>

gboolean vim4_hdr10_probe(int capture, int encoder, Vim4Ioctl call)
{
    /* AMLY is the private LE packed Cb/Y0/Cr/Y1 format, five bytes per pair.
     * It must never be advertised as a standard P010 or Y210 capture format. */
    struct v4l2_fmtdesc entry = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    while (!call(capture, VIDIOC_ENUM_FMT, &entry)) {
        if (entry.pixelformat == v4l2_fourcc('A','M','L','Y')) break;
        entry.index++;
    }
    if (entry.pixelformat != v4l2_fourcc('A','M','L','Y')) return FALSE;
    const guint ids[] = {V4L2_CID_COLORIMETRY_HDR10_MASTERING_DISPLAY,
                        V4L2_CID_COLORIMETRY_HDR10_CLL_INFO};
    const guint sizes[] = {sizeof(struct v4l2_ctrl_hdr10_mastering_display),
                          sizeof(struct v4l2_ctrl_hdr10_cll_info)};
    for (guint i = 0; i < G_N_ELEMENTS(ids); i++) {
        struct v4l2_query_ext_ctrl ctrl = {.id = ids[i]};
        if (call(capture, VIDIOC_QUERY_EXT_CTRL, &ctrl) ||
            (ctrl.flags & V4L2_CTRL_FLAG_DISABLED) || ctrl.elems != 1 ||
            ctrl.elem_size != sizes[i]) return FALSE;
    }
    struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    format.fmt.pix_mp.width = 640; format.fmt.pix_mp.height = 480;
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_HEVC;
    if (call(encoder, VIDIOC_S_FMT, &format) ||
        format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_HEVC) return FALSE;
    struct v4l2_control profile = {.id = V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
                                  .value = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10};
    if (call(encoder, VIDIOC_S_CTRL, &profile) || profile.value != V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10) return FALSE;
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    struct v4l2_pix_format_mplane *raw = &format.fmt.pix_mp;
    raw->width = 640; raw->height = 480; raw->pixelformat = V4L2_PIX_FMT_P010;
    raw->colorspace = V4L2_COLORSPACE_BT2020; raw->xfer_func = V4L2_XFER_FUNC_SMPTE2084;
    raw->ycbcr_enc = V4L2_YCBCR_ENC_BT2020; raw->quantization = V4L2_QUANTIZATION_LIM_RANGE;
    if (call(encoder, VIDIOC_S_FMT, &format)) return FALSE;
    guint stride = raw->plane_fmt[0].bytesperline;
    return raw->pixelformat == V4L2_PIX_FMT_P010 && raw->width == 640 && raw->height == 480 &&
        raw->num_planes == 1 && stride >= 1280 && stride % 64 == 0 &&
        raw->plane_fmt[0].sizeimage >= (guint64)stride * 480 * 3 / 2 &&
        raw->colorspace == V4L2_COLORSPACE_BT2020 && raw->xfer_func == V4L2_XFER_FUNC_SMPTE2084 &&
        raw->ycbcr_enc == V4L2_YCBCR_ENC_BT2020 && raw->quantization == V4L2_QUANTIZATION_LIM_RANGE;
}

static void be16(guint8 *out, guint16 value) { out[0] = value >> 8; out[1] = value; }
static void be32(guint8 *out, guint32 value)
{ out[0] = value >> 24; out[1] = value >> 16; out[2] = value >> 8; out[3] = value; }

gboolean vim4_hdr10_read(int capture, Vim4Hdr10 *metadata)
{
    struct v4l2_ctrl_hdr10_mastering_display mastering = {0};
    struct v4l2_ctrl_hdr10_cll_info cll = {0};
    struct v4l2_ext_control values[] = {
        {.id = V4L2_CID_COLORIMETRY_HDR10_MASTERING_DISPLAY,
         .size = sizeof(mastering), .ptr = &mastering},
        {.id = V4L2_CID_COLORIMETRY_HDR10_CLL_INFO, .size = sizeof(cll), .ptr = &cll}};
    struct v4l2_ext_controls controls = {.count = G_N_ELEMENTS(values), .controls = values};
    int result;
    do { result = ioctl(capture, VIDIOC_G_EXT_CTRLS, &controls); } while (result < 0 && errno == EINTR);
    if (result) return FALSE;
    for (guint i = 0; i < 3; i++) {
        be16(metadata->mastering + i * 4, mastering.display_primaries_x[i]);
        be16(metadata->mastering + i * 4 + 2, mastering.display_primaries_y[i]);
    }
    be16(metadata->mastering + 12, mastering.white_point_x);
    be16(metadata->mastering + 14, mastering.white_point_y);
    be32(metadata->mastering + 16, mastering.max_display_mastering_luminance);
    be32(metadata->mastering + 20, mastering.min_display_mastering_luminance);
    be16(metadata->cll, cll.max_content_light_level);
    be16(metadata->cll + 2, cll.max_pic_average_light_level);
    return TRUE;
}

static void append_nal(GByteArray *out, const guint8 *nal, gsize size)
{
    const guint8 start[] = {0, 0, 0, 1};
    g_byte_array_append(out, start, sizeof(start));
    g_byte_array_append(out, nal, size);
}

static void append_rbsp(GByteArray *out, const guint8 header[2], const guint8 *rbsp, gsize size)
{
    append_nal(out, header, 2);
    guint zeros = 0;
    for (gsize i = 0; i < size; i++) {
        if (zeros == 2 && rbsp[i] <= 3) {
            const guint8 escape = 3;
            g_byte_array_append(out, &escape, 1);
            zeros = 0;
        }
        g_byte_array_append(out, rbsp + i, 1);
        zeros = rbsp[i] == 0 ? zeros + 1 : 0;
    }
}

static gboolean number(const guint8 *data, gsize size, gsize *cursor, gsize *value)
{
    *value = 0;
    while (*cursor < size) {
        guint8 byte = data[(*cursor)++];
        if (*value > G_MAXSIZE - byte) return FALSE;
        *value += byte;
        if (byte != 255) return TRUE;
    }
    return FALSE;
}

/* Preserve unrelated messages even when firmware puts them in a mixed SEI NAL. */
static gboolean keep_sei(GByteArray *out, const guint8 *nal, gsize size)
{
    GByteArray *rbsp = g_byte_array_new(), *kept = g_byte_array_new();
    guint zeros = 0;
    for (gsize i = 2; i < size; i++) {
        if (zeros == 2 && nal[i] == 3) { zeros = 0; continue; }
        g_byte_array_append(rbsp, nal + i, 1);
        zeros = nal[i] == 0 ? zeros + 1 : 0;
    }
    gsize cursor = 0;
    gboolean ok = FALSE;
    while (cursor < rbsp->len) {
        if (rbsp->data[cursor] == 0x80 && cursor + 1 == rbsp->len) { ok = TRUE; break; }
        gsize start = cursor, kind, length;
        if (!number(rbsp->data, rbsp->len, &cursor, &kind) ||
            !number(rbsp->data, rbsp->len, &cursor, &length) || length > rbsp->len - cursor) break;
        cursor += length;
        if (kind != 137 && kind != 144)
            g_byte_array_append(kept, rbsp->data + start, cursor - start);
    }
    if (ok && kept->len) {
        const guint8 trailing = 0x80;
        g_byte_array_append(kept, &trailing, 1);
        append_rbsp(out, nal, kept->data, kept->len);
    }
    g_byte_array_unref(rbsp); g_byte_array_unref(kept);
    return ok;
}

static gsize start_code(const guint8 *data, gsize size, gsize from, gsize *length)
{
    for (gsize i = from; i + 3 <= size; i++) {
        if (data[i] || data[i + 1]) continue;
        if (i + 4 <= size && !data[i + 2] && data[i + 3] == 1) { *length = 4; return i; }
        if (data[i + 2] == 1) { *length = 3; return i; }
    }
    return size;
}

gboolean vim4_hdr10_is_irap(GstBuffer *au)
{
    GstMapInfo map;
    if (!gst_buffer_map(au, &map, GST_MAP_READ)) return FALSE;
    gsize length = 0, cursor = start_code(map.data, map.size, 0, &length);
    gboolean irap = FALSE;
    while (cursor < map.size) {
        gsize begin = cursor + length;
        if (begin + 2 <= map.size) {
            guint kind = (map.data[begin] >> 1) & 63;
            if (kind >= 16 && kind <= 23) { irap = TRUE; break; }
        }
        cursor = start_code(map.data, map.size, begin, &length);
    }
    gst_buffer_unmap(au, &map);
    return irap;
}

GstBuffer *vim4_hdr10_insert(GstBuffer *au, const Vim4Hdr10 *metadata, gboolean changed)
{
    GstMapInfo map;
    if (!gst_buffer_map(au, &map, GST_MAP_READ)) return NULL;
    GByteArray *out = g_byte_array_new();
    gsize length = 0, cursor = start_code(map.data, map.size, 0, &length);
    gboolean ok = cursor == 0, inserted = FALSE;
    while (ok && cursor < map.size) {
        gsize begin = cursor + length, next_length = 0;
        gsize next = start_code(map.data, map.size, begin, &next_length), end = next;
        while (end > begin && map.data[end - 1] == 0) end--;
        if (end < begin + 2 || (map.data[begin] & 0x80) || !(map.data[begin + 1] & 7)) { ok = FALSE; break; }
        const guint8 *nal = map.data + begin;
        guint kind = (nal[0] >> 1) & 63;
        if (kind <= 31 && !inserted && (changed || (kind >= 16 && kind <= 23))) {
            guint8 rbsp[33] = {137, 24};
            memcpy(rbsp + 2, metadata->mastering, 24);
            rbsp[26] = 144; rbsp[27] = 4;
            memcpy(rbsp + 28, metadata->cll, 4); rbsp[32] = 0x80;
            const guint8 header[] = {39 << 1, 1};
            append_rbsp(out, header, rbsp, sizeof(rbsp));
            inserted = TRUE;
        }
        if (kind == 39 || kind == 40) ok = keep_sei(out, nal, end - begin);
        else append_nal(out, nal, end - begin);
        cursor = next; length = next_length;
    }
    gst_buffer_unmap(au, &map);
    if (!ok || !out->len) { g_byte_array_unref(out); return NULL; }
    gsize size = out->len;
    GstBuffer *result = gst_buffer_new_wrapped(g_byte_array_free(out, FALSE), size);
    gst_buffer_copy_into(result, au, GST_BUFFER_COPY_TIMESTAMPS | GST_BUFFER_COPY_FLAGS, 0, -1);
    return result;
}
