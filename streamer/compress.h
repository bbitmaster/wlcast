#ifndef WLCAST_COMPRESS_H
#define WLCAST_COMPRESS_H

#include <stddef.h>
#include <stdint.h>

#include "capture.h"

struct jpeg_encoder {
  void *handle;
  int quality;
  int subsamp;
  int width;
  int height;
  unsigned char *buffer;
  unsigned long buffer_size;
};

int jpeg_encoder_init(struct jpeg_encoder *enc, int quality);
int jpeg_encode_frame(struct jpeg_encoder *enc, const struct capture_frame *frame,
                      unsigned char **out_buf, unsigned long *out_size);
void jpeg_encoder_destroy(struct jpeg_encoder *enc);

#endif
