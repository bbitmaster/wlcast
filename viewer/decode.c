#include "decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <turbojpeg.h>

int jpeg_decoder_init(struct jpeg_decoder *dec) {
  memset(dec, 0, sizeof(*dec));
  dec->handle = tjInitDecompress();
  if (!dec->handle) {
    fprintf(stderr, "tjInitDecompress failed: %s\n", tjGetErrorStr());
    return -1;
  }
  return 0;
}

int jpeg_decode_frame(struct jpeg_decoder *dec, const uint8_t *data,
                      size_t size, struct decoded_frame *out) {
  int width = 0;
  int height = 0;

  if (tjDecompressHeader(dec->handle, (unsigned char *)data, (unsigned long)size,
                         &width, &height) != 0) {
    fprintf(stderr, "tjDecompressHeader failed: %s\n", tjGetErrorStr());
    return -1;
  }

  size_t needed = (size_t)width * height * 4u;
  if (needed > dec->capacity) {
    uint8_t *new_pixels = realloc(dec->pixels, needed);
    if (!new_pixels) {
      fprintf(stderr, "realloc decode buffer failed\n");
      return -1;
    }
    dec->pixels = new_pixels;
    dec->capacity = needed;
  }

  int pitch = width * 4;
  int flags = TJFLAG_FASTDCT;
  int pixfmt = TJPF_BGRX;
  if (tjDecompress2(dec->handle, data, (unsigned long)size, dec->pixels,
                    width, pitch, height, pixfmt, flags) != 0) {
    fprintf(stderr, "tjDecompress2 failed: %s\n", tjGetErrorStr());
    return -1;
  }

  out->pixels = dec->pixels;
  out->width = width;
  out->height = height;
  out->pitch = pitch;
  dec->width = width;
  dec->height = height;

  return 0;
}

void jpeg_decoder_destroy(struct jpeg_decoder *dec) {
  if (!dec) {
    return;
  }
  if (dec->handle) {
    tjDestroy(dec->handle);
  }
  free(dec->pixels);
  memset(dec, 0, sizeof(*dec));
}
