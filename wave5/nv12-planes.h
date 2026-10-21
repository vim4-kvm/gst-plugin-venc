/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include <gst/gst.h>

GstBuffer *vim4_nv12_plane_view(GstBuffer *original, guint width, guint height);
GType gst_vim4_nv12_planes_get_type(void);
