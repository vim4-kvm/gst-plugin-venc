/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include <gst/gst.h>
#include <linux/videodev2.h>

typedef struct { guint8 mastering[24], cll[4]; } Vim4Hdr10;
/* Bufferless negotiation: never allocates buffers or starts either device. */
typedef int (*Vim4Ioctl)(int, unsigned long, void *);
gboolean vim4_hdr10_probe(int capture, int encoder, Vim4Ioctl call);
gboolean vim4_hdr10_read(int capture, Vim4Hdr10 *metadata);
/* NULL means malformed Annex B / SEI, not a replacement frame. */
GstBuffer *vim4_hdr10_insert(GstBuffer *au, const Vim4Hdr10 *metadata, gboolean changed);
