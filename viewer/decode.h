#ifndef WLCAST_VIEWER_DECODE_H
#define WLCAST_VIEWER_DECODE_H

#include <stddef.h>
#include <stdint.h>

struct jpeg_decoder {
  void *handle;
  int width;
  int height;
  uint8_t *pixels;
  size_t capacity;
};

struct decoded_frame {
  uint8_t *pixels;
  int width;
  int height;
  int pitch;
};

int jpeg_decoder_init(struct jpeg_decoder *dec);
int jpeg_decode_frame(struct jpeg_decoder *dec, const uint8_t *data,
                      size_t size, struct decoded_frame *out);
void jpeg_decoder_destroy(struct jpeg_decoder *dec);

#endif
