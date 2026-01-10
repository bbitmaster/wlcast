#include "compress.h"

#include <stdio.h>
#include <string.h>

#include <turbojpeg.h>

#include <wayland-client.h>

static int map_pixel_format(uint32_t format) {
  switch (format) {
    case WL_SHM_FORMAT_XRGB8888:
      return TJPF_BGRX;
    case WL_SHM_FORMAT_ARGB8888:
      return TJPF_BGRA;
    case WL_SHM_FORMAT_XBGR8888:
      return TJPF_RGBX;
    case WL_SHM_FORMAT_ABGR8888:
      return TJPF_RGBA;
    case WL_SHM_FORMAT_RGB888:
      return TJPF_RGB;
    case WL_SHM_FORMAT_BGR888:
      return TJPF_BGR;
    default:
      return -1;
  }
}

int jpeg_encoder_init(struct jpeg_encoder *enc, int quality) {
  memset(enc, 0, sizeof(*enc));
  enc->handle = tjInitCompress();
  if (!enc->handle) {
    fprintf(stderr, "tjInitCompress failed: %s\n", tjGetErrorStr());
    return -1;
  }
  enc->quality = quality;
  enc->subsamp = TJSAMP_420;
  return 0;
}

int jpeg_encode_frame(struct jpeg_encoder *enc, const struct capture_frame *frame,
                      unsigned char **out_buf, unsigned long *out_size) {
  int pixel_format = map_pixel_format(frame->format);
  if (pixel_format < 0) {
    fprintf(stderr, "Unsupported wl_shm format: %u\n", frame->format);
    return -1;
  }

  if (enc->width != (int)frame->width || enc->height != (int)frame->height ||
      enc->buffer == NULL) {
    unsigned long needed = tjBufSize(frame->width, frame->height, enc->subsamp);
    unsigned char *new_buf = tjAlloc(needed);
    if (!new_buf) {
      fprintf(stderr, "tjAlloc failed\n");
      return -1;
    }
    if (enc->buffer) {
      tjFree(enc->buffer);
    }
    enc->buffer = new_buf;
    enc->buffer_size = needed;
    enc->width = (int)frame->width;
    enc->height = (int)frame->height;
  }

  unsigned char *src = (unsigned char *)frame->data;
  int pitch = (int)frame->stride;
  if (frame->y_invert) {
    src = (unsigned char *)frame->data + (frame->height - 1) * frame->stride;
    pitch = -pitch;
  }

  unsigned long jpeg_size = enc->buffer_size;
  int flags = TJFLAG_NOREALLOC | TJFLAG_FASTDCT;
  int rc = tjCompress2(enc->handle, src, frame->width, pitch, frame->height,
                       pixel_format, &enc->buffer, &jpeg_size, enc->subsamp,
                       enc->quality, flags);
  if (rc != 0) {
    fprintf(stderr, "tjCompress2 failed: %s\n", tjGetErrorStr());
    return -1;
  }

  *out_buf = enc->buffer;
  *out_size = jpeg_size;
  return 0;
}

void jpeg_encoder_destroy(struct jpeg_encoder *enc) {
  if (!enc) {
    return;
  }
  if (enc->handle) {
    tjDestroy(enc->handle);
  }
  if (enc->buffer) {
    tjFree(enc->buffer);
  }
  memset(enc, 0, sizeof(*enc));
}
