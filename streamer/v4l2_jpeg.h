#ifndef WLCAST_V4L2_JPEG_H
#define WLCAST_V4L2_JPEG_H

#include <stdint.h>

#include "capture.h"

struct v4l2_jpeg_encoder {
  int fd;
  int width;
  int height;
  int quality;
  uint32_t out_format;
  unsigned int out_num_planes;
  unsigned int out_bytesperline[3];
  unsigned int out_plane_size[3];
  unsigned int out_memory;
  unsigned int out_dmabuf_offset[3];
  void *out_map[3];
  unsigned int out_map_size[3];
  void *out_map_base;
  unsigned int out_map_base_size;
  void *out_userptr[3];
  int out_dmabuf_fd[3];
  uint32_t cap_format;
  unsigned int cap_num_planes;
  unsigned int cap_plane_size[3];
  void *cap_map[3];
  unsigned int cap_map_size[3];
  int cap_queued;
};

int v4l2_jpeg_init(struct v4l2_jpeg_encoder *enc, int width, int height,
                   int quality);

/**
 * Initialize encoder specifically for NV12 input (for use with RGA).
 * This avoids the YUYV fallback that happens in v4l2_jpeg_init.
 */
int v4l2_jpeg_init_nv12(struct v4l2_jpeg_encoder *enc, int width, int height,
                        int quality);

int v4l2_jpeg_encode_frame(struct v4l2_jpeg_encoder *enc,
                           const struct capture_frame *frame,
                           unsigned char **out_buf,
                           unsigned long *out_size);

/**
 * Encode pre-converted NV12 data to JPEG.
 * Use this when color conversion is done externally (e.g., by RGA).
 */
int v4l2_jpeg_encode_nv12(struct v4l2_jpeg_encoder *enc,
                          const void *y_plane, unsigned int y_stride,
                          const void *uv_plane, unsigned int uv_stride,
                          unsigned char **out_buf, unsigned long *out_size);

void v4l2_jpeg_destroy(struct v4l2_jpeg_encoder *enc);

#endif
