#ifndef WLCAST_CAPTURE_H
#define WLCAST_CAPTURE_H

#include <stdint.h>

struct capture_context;

struct capture_frame {
  uint32_t format;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  void *data;
  int y_invert;
};

int capture_init(struct capture_context **out_ctx, int overlay_cursor);
void capture_set_region(struct capture_context *ctx, int x, int y, int width,
                        int height);
int capture_next_frame(struct capture_context *ctx, struct capture_frame *out);
void capture_shutdown(struct capture_context *ctx);

#endif
