/*
 * Copyright (C) 2014-2019 Amlogic, Inc. All rights reserved.
 *
 * All information contained herein is Amlogic confidential.
 *
 */

#ifndef __GST_AMLVENC_H__
#define __GST_AMLVENC_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideoencoder.h>

#include "amvenc.h"

#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif

#define SUPPORT_SCALE 1

#if SUPPORT_SCALE
#include "aml_ge2d.h"
#endif

#define PTS_UINT_4_RESET 4294967295

G_BEGIN_DECLS

#define GST_TYPE_AMLVENC \
  (gst_amlvenc_get_type())
#define GST_AMLVENC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_AMLVENC,GstAmlVEnc))
#define GST_AMLVENC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_AMLVENC,GstAmlVEncClass))
#define GST_IS_AMLVENC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_AMLVENC))
#define GST_IS_AMLVENC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_AMLVENC))

typedef struct _GstAmlVEnc      GstAmlVEnc;
typedef struct _GstAmlVEncClass GstAmlVEncClass;
typedef struct _GstAmlVEncVTable GstAmlVEncVTable;

struct _GstAmlVEnc
{
  GstVideoEncoder element;

  /*< private >*/
  struct codec_info {
    amvenc_handle_t handle;
    amvenc_codec_id_t id;
    guchar *buf;
  } codec;

#if SUPPORT_SCALE
    guint ge2d_initial_done;
    aml_ge2d_t amlge2d;
    gboolean INIT_GE2D;
#endif

  /* properties */
  gint fd[3];
  gint gop;
  gint framerate;
  gint encoding_framerate;
  gint i_qp_min;
  gint intra_refresh_rows;
  gint qp_max;
  guint bitrate;
  guint min_buffers;
  guint max_buffers;
  guint encoder_bufsize;
  guint u4_first_pts_index;

  /* input description */
  GstVideoCodecState *input_state;

};

struct _GstAmlVEncClass
{
  GstVideoEncoderClass parent_class;
};

GType gst_amlvenc_get_type (void);

G_END_DECLS

#endif /* __GST_AMLVENC_H__ */
