/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/video/gstvideoencoder.h>
#include <gst/allocators/gstdmabuf.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* Amlogic 5.15 jpegenc UAPI. These legacy ioctls encode a u32 size even
 * when the payload is an array. CONFIG_DMA_INPUT imports into the driver's
 * releasable pool; NEW_CMD's DMA mode leaks its per-command attachments. */
#define JPEG_IOCTL(n) _IOW('J', (n), uint32_t)
enum { BUFFINFO = 0, INIT = 1, COMMAND = 2, STAGE = 3, OUTPUT = 4,
       IMPORT_DMA = 7, RELEASE_DMA = 8 };
enum { PROP_0, PROP_QUALITY };

typedef struct {
  GstVideoEncoder parent;
  GstVideoInfo info;
  gint quality;
  int fd;
  guint8 *mapping;
  guint32 buffers[7];
} GstVim4JpegEnc;
typedef struct { GstVideoEncoderClass parent; } GstVim4JpegEncClass;
G_DEFINE_TYPE(GstVim4JpegEnc, gst_vim4_jpeg_enc, GST_TYPE_VIDEO_ENCODER)

static gboolean stop(GstVideoEncoder *base)
{
  GstVim4JpegEnc *self = (GstVim4JpegEnc *)base;
  if (self->fd >= 0) {
    if (self->mapping) munmap(self->mapping, self->buffers[0]);
    /* Driver close stops hardware before releasing outstanding imports. */
    close(self->fd);
  }
  self->mapping = NULL;
  self->fd = -1;
  return TRUE;
}

static gboolean start(GstVideoEncoder *base)
{
  GstVim4JpegEnc *self = (GstVim4JpegEnc *)base;
  self->fd = open("/dev/jpegenc", O_RDWR | O_CLOEXEC);
  if (self->fd < 0 || ioctl(self->fd, JPEG_IOCTL(BUFFINFO), self->buffers) < 0)
    goto fail;
  for (guint i = 1; i < 7; i += 2) {
    if (self->buffers[i] > self->buffers[0] ||
        self->buffers[i + 1] > self->buffers[0] - self->buffers[i]) {
      errno = EOVERFLOW;
      goto fail;
    }
  }
  self->mapping = mmap(NULL, self->buffers[0], PROT_READ | PROT_WRITE,
                       MAP_SHARED, self->fd, 0);
  if (self->mapping == MAP_FAILED) { self->mapping = NULL; goto fail; }
  if (ioctl(self->fd, JPEG_IOCTL(INIT), 0) < 0) goto fail;
  return TRUE;
fail:
  GST_ELEMENT_ERROR(self, RESOURCE, OPEN_READ_WRITE,
                    ("Cannot initialize hardware JPEG encoder"), ("%s", g_strerror(errno)));
  stop(base);
  return FALSE;
}

static gboolean set_format(GstVideoEncoder *base, GstVideoCodecState *state)
{
  GstVim4JpegEnc *self = (GstVim4JpegEnc *)base;
  self->info = state->info;
  GstVideoCodecState *output = gst_video_encoder_set_output_state(base,
      gst_caps_new_empty_simple("image/jpeg"), state);
  gst_video_codec_state_unref(output);
  return gst_video_encoder_negotiate(base);
}

static GstFlowReturn handle_frame(GstVideoEncoder *base, GstVideoCodecFrame *frame)
{
  GstVim4JpegEnc *self = (GstVim4JpegEnc *)base;
  guint32 cmd[30] = {0}, sizes[2] = {0}, stage = 0;
  GstVideoMeta *meta = gst_buffer_get_video_meta(frame->input_buffer);
  GstMemory *memory = gst_buffer_peek_memory(frame->input_buffer, 0);
  guint width = GST_VIDEO_INFO_WIDTH(&self->info), height = GST_VIDEO_INFO_HEIGHT(&self->info);
  gint stride = meta ? meta->stride[0] : self->info.stride[0];
  gsize uv_offset = meta ? meta->offset[1] : self->info.offset[1];
  gboolean imported = FALSE;
  if (self->fd < 0) goto fail;
  cmd[1] = GST_VIDEO_INFO_FORMAT(&self->info) == GST_VIDEO_FORMAT_NV21 ? 2 : 3;
  cmd[2] = 4; /* JPEG 4:2:0 */
  cmd[3] = width;
  cmd[4] = height;
  cmd[7] = g_atomic_int_get(&self->quality);
  cmd[9] = 2; /* flush encoded output */

  if (gst_is_dmabuf_memory(memory) && gst_buffer_n_memory(frame->input_buffer) == 1) {
    gsize offset;
    gsize length = gst_memory_get_sizes(memory, &offset, NULL);
    if (offset != 0 || (meta && meta->offset[0] != 0) || stride < (gint)width ||
        uv_offset < (gsize)stride * height || uv_offset % stride != 0 ||
        (meta && meta->stride[1] != stride) ||
        length < uv_offset + (gsize)stride * ((height + 1) / 2)) {
      GST_ERROR_OBJECT(self, "Unsupported DMA plane layout");
      goto fail;
    }
    int dma_fd = gst_dmabuf_memory_get_fd(memory);
    if (ioctl(self->fd, JPEG_IOCTL(IMPORT_DMA), &dma_fd) < 0) goto fail;
    imported = TRUE;
    cmd[11] = cmd[12] = cmd[13] = stride;
    cmd[14] = uv_offset / stride;
    cmd[5] = uv_offset + stride * ((height + 1) / 2);
  } else {
    /* System-memory frames are useful for diagnostics. Live capture and GE2D
     * scaling use the DMA path above, avoiding a full raw-frame CPU copy. */
    GstVideoFrame input;
    cmd[11] = cmd[12] = cmd[13] = (width + 31) & ~31u;
    cmd[14] = height;
    cmd[5] = cmd[11] * (height + (height + 1) / 2);
    if (cmd[5] > self->buffers[2] ||
        !gst_video_frame_map(&input, &self->info, frame->input_buffer, GST_MAP_READ)) goto fail;
    guint8 *dest = self->mapping + self->buffers[1];
    for (guint plane = 0; plane < 2; plane++) {
      guint rows = plane == 0 ? height : (height + 1) / 2;
      for (guint y = 0; y < rows; y++) {
        memcpy(dest, (guint8 *)GST_VIDEO_FRAME_PLANE_DATA(&input, plane) +
               y * GST_VIDEO_FRAME_PLANE_STRIDE(&input, plane), width);
        dest += cmd[11];
      }
    }
    gst_video_frame_unmap(&input);
    cmd[9] |= 1;
  }

  if (ioctl(self->fd, JPEG_IOCTL(COMMAND), cmd) < 0) goto fail;
  struct pollfd pollfd = { .fd = self->fd, .events = POLLIN };
  int ready;
  do { ready = poll(&pollfd, 1, 1000); } while (ready < 0 && errno == EINTR);
  if (ready <= 0 || !(pollfd.revents & POLLIN) ||
      ioctl(self->fd, JPEG_IOCTL(STAGE), &stage) < 0 || stage != 4 ||
      ioctl(self->fd, JPEG_IOCTL(OUTPUT), sizes) < 0 ||
      sizes[0] == 0 || sizes[0] > self->buffers[4] ||
      sizes[1] == 0 || sizes[1] > self->buffers[6]) goto fail;

  frame->output_buffer = gst_video_encoder_allocate_output_buffer(base,
      (gsize)sizes[0] + sizes[1]);
  if (!frame->output_buffer) goto fail;
  gst_buffer_fill(frame->output_buffer, 0, self->mapping + self->buffers[3], sizes[0]);
  gst_buffer_fill(frame->output_buffer, sizes[0], self->mapping + self->buffers[5], sizes[1]);
  /* Release the kernel import before GStreamer returns the capture buffer.
   * Keeping even four imports pins the complete VDIN capture ring. */
  if (imported && ioctl(self->fd, JPEG_IOCTL(RELEASE_DMA), 0) < 0) goto fail;
  frame->dts = frame->pts;
  GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT(frame);
  return gst_video_encoder_finish_frame(base, frame);
fail:
  GST_ELEMENT_ERROR(self, STREAM, ENCODE, ("Hardware JPEG frame failed"),
                    ("stage=%u errno=%d (%s)", stage, errno, g_strerror(errno)));
  /* Closing stops the hardware before the input frame can be recycled. */
  stop(base);
  gst_video_codec_frame_unref(frame);
  return GST_FLOW_ERROR;
}

static void set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
  if (id == PROP_QUALITY) g_atomic_int_set(&((GstVim4JpegEnc *)object)->quality, g_value_get_int(value));
  else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}
static void get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
  if (id == PROP_QUALITY) g_value_set_int(value, g_atomic_int_get(&((GstVim4JpegEnc *)object)->quality));
  else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}
static void gst_vim4_jpeg_enc_init(GstVim4JpegEnc *self) { self->fd = -1; self->quality = 80; }
static void gst_vim4_jpeg_enc_class_init(GstVim4JpegEncClass *klass)
{
  GstVideoEncoderClass *video = GST_VIDEO_ENCODER_CLASS(klass);
  GObjectClass *object = G_OBJECT_CLASS(klass);
  GstElementClass *element = GST_ELEMENT_CLASS(klass);
  object->set_property = set_property;
  object->get_property = get_property;
  g_object_class_install_property(object, PROP_QUALITY, g_param_spec_int("quality", "Quality",
      "JPEG quality", 1, 100, 80, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING));
  video->start = start;
  video->stop = stop;
  video->set_format = set_format;
  video->handle_frame = handle_frame;
  GstCaps *sink = gst_caps_from_string("video/x-raw,format=(string){NV12,NV21},width=(int)[16,4096],height=(int)[16,2160],framerate=(fraction)[0/1,120/1]");
  GstCaps *src = gst_caps_from_string("image/jpeg,width=(int)[16,4096],height=(int)[16,2160],framerate=(fraction)[0/1,120/1]");
  gst_element_class_add_pad_template(element, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, sink));
  gst_element_class_add_pad_template(element, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, src));
  gst_caps_unref(sink);
  gst_caps_unref(src);
  gst_element_class_set_static_metadata(element, "VIM4 hardware JPEG encoder", "Codec/Encoder/Image",
      "Continuous JPEG encoding with bounded DMA input lifetime", "VIM4 KVM contributors");
}
static gboolean plugin_init(GstPlugin *plugin)
{ return gst_element_register(plugin, "vim4jpegenc", GST_RANK_NONE, gst_vim4_jpeg_enc_get_type()); }
#define PACKAGE "vim4-kvm"
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, vim4jpegenc, "VIM4 hardware JPEG encoder",
                  plugin_init, "1.0", "LGPL", PACKAGE, "https://github.com/vim4-kvm/gst-plugin-venc")
