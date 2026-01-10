#define _GNU_SOURCE

#include "capture.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <wayland-client.h>

#include "wlr-screencopy-unstable-v1-client-protocol.h"

struct capture_buffer {
  int fd;
  size_t size;
  void *data;
  struct wl_shm_pool *pool;
  struct wl_buffer *buffer;
  uint32_t format;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
};

struct capture_context {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_shm *shm;
  struct wl_output *output;
  struct zwlr_screencopy_manager_v1 *manager;
  struct capture_buffer buffer;
  int overlay_cursor;
  int has_region;
  int region_x;
  int region_y;
  int region_width;
  int region_height;
};

struct frame_state {
  struct capture_context *ctx;
  struct zwlr_screencopy_frame_v1 *frame;
  int done;
  int failed;
  int y_invert;
  int shm_ready;
  int dmabuf_ready;
  int copy_sent;
  uint32_t format;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
};

static int create_shm_file(size_t size) {
  int fd = -1;
#ifdef __linux__
  fd = memfd_create("wlcast", MFD_CLOEXEC);
  if (fd >= 0) {
    if (ftruncate(fd, (off_t)size) < 0) {
      close(fd);
      return -1;
    }
    return fd;
  }
#endif
  char name[] = "/wlcast-XXXXXX";
  fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0) {
    return -1;
  }
  shm_unlink(name);
  if (ftruncate(fd, (off_t)size) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static void buffer_release(void *data, struct wl_buffer *buffer) {
  (void)data;
  (void)buffer;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static int recreate_buffer(struct capture_context *ctx, uint32_t format,
                           uint32_t width, uint32_t height, uint32_t stride) {
  struct capture_buffer *buf = &ctx->buffer;
  size_t size = (size_t)stride * height;

  if (buf->buffer) {
    wl_buffer_destroy(buf->buffer);
    buf->buffer = NULL;
  }
  if (buf->pool) {
    wl_shm_pool_destroy(buf->pool);
    buf->pool = NULL;
  }
  if (buf->data) {
    munmap(buf->data, buf->size);
    buf->data = NULL;
  }
  if (buf->fd >= 0) {
    close(buf->fd);
    buf->fd = -1;
  }

  buf->fd = create_shm_file(size);
  if (buf->fd < 0) {
    perror("create_shm_file");
    return -1;
  }

  buf->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, buf->fd, 0);
  if (buf->data == MAP_FAILED) {
    perror("mmap");
    close(buf->fd);
    buf->fd = -1;
    return -1;
  }

  buf->pool = wl_shm_create_pool(ctx->shm, buf->fd, (int)size);
  if (!buf->pool) {
    fprintf(stderr, "wl_shm_create_pool failed\n");
    munmap(buf->data, size);
    buf->data = NULL;
    close(buf->fd);
    buf->fd = -1;
    return -1;
  }

  buf->buffer = wl_shm_pool_create_buffer(buf->pool, 0, (int)width,
                                          (int)height, (int)stride, format);
  if (!buf->buffer) {
    fprintf(stderr, "wl_shm_pool_create_buffer failed\n");
    wl_shm_pool_destroy(buf->pool);
    buf->pool = NULL;
    munmap(buf->data, size);
    buf->data = NULL;
    close(buf->fd);
    buf->fd = -1;
    return -1;
  }

  wl_buffer_add_listener(buf->buffer, &buffer_listener, NULL);

  buf->size = size;
  buf->format = format;
  buf->width = width;
  buf->height = height;
  buf->stride = stride;

  return 0;
}

static void frame_handle_buffer(void *data,
                                struct zwlr_screencopy_frame_v1 *frame,
                                uint32_t format, uint32_t width,
                                uint32_t height, uint32_t stride) {
  (void)frame;
  struct frame_state *state = data;
  struct capture_context *ctx = state->ctx;

  state->format = format;
  state->width = width;
  state->height = height;
  state->stride = stride;
  state->shm_ready = 1;

  if (state->copy_sent) {
    return;
  }

  int version = wl_proxy_get_version((struct wl_proxy *)state->frame);
  if (version >= 3) {
    return;
  }

  if (!ctx->buffer.buffer || ctx->buffer.format != format ||
      ctx->buffer.width != width || ctx->buffer.height != height ||
      ctx->buffer.stride != stride) {
    if (recreate_buffer(ctx, format, width, height, stride) != 0) {
      state->failed = 1;
      state->done = 1;
      return;
    }
  }

  state->copy_sent = 1;
  zwlr_screencopy_frame_v1_copy(state->frame, ctx->buffer.buffer);
  wl_display_flush(ctx->display);
}

static void frame_handle_flags(void *data,
                               struct zwlr_screencopy_frame_v1 *frame,
                               uint32_t flags) {
  (void)frame;
  struct frame_state *state = data;
  state->y_invert =
      (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) ? 1 : 0;
}

static void frame_handle_linux_dmabuf(void *data,
                                      struct zwlr_screencopy_frame_v1 *frame,
                                      uint32_t format, uint32_t width,
                                      uint32_t height) {
  (void)frame;
  (void)format;
  (void)width;
  (void)height;
  struct frame_state *state = data;
  state->dmabuf_ready = 1;
}

static void frame_handle_buffer_done(void *data,
                                     struct zwlr_screencopy_frame_v1 *frame) {
  (void)frame;
  struct frame_state *state = data;
  struct capture_context *ctx = state->ctx;

  if (state->copy_sent) {
    return;
  }

  if (!state->shm_ready) {
    fprintf(stderr,
            "Compositor reported only dmabuf buffers (unsupported in this build)\n");
    state->failed = 1;
    state->done = 1;
    return;
  }

  if (!ctx->buffer.buffer || ctx->buffer.format != state->format ||
      ctx->buffer.width != state->width || ctx->buffer.height != state->height ||
      ctx->buffer.stride != state->stride) {
    if (recreate_buffer(ctx, state->format, state->width, state->height,
                        state->stride) != 0) {
      state->failed = 1;
      state->done = 1;
      return;
    }
  }

  state->copy_sent = 1;
  zwlr_screencopy_frame_v1_copy(state->frame, ctx->buffer.buffer);
  wl_display_flush(ctx->display);
}

static void frame_handle_ready(void *data,
                               struct zwlr_screencopy_frame_v1 *frame,
                               uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                               uint32_t tv_nsec) {
  (void)frame;
  (void)tv_sec_hi;
  (void)tv_sec_lo;
  (void)tv_nsec;
  struct frame_state *state = data;
  state->done = 1;
}

static void frame_handle_failed(void *data,
                                struct zwlr_screencopy_frame_v1 *frame) {
  (void)frame;
  struct frame_state *state = data;
  state->failed = 1;
  state->done = 1;
}

static void frame_handle_damage(void *data,
                                struct zwlr_screencopy_frame_v1 *frame,
                                uint32_t x, uint32_t y, uint32_t width,
                                uint32_t height) {
  (void)data;
  (void)frame;
  (void)x;
  (void)y;
  (void)width;
  (void)height;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_handle_buffer,
    .linux_dmabuf = frame_handle_linux_dmabuf,
    .buffer_done = frame_handle_buffer_done,
    .flags = frame_handle_flags,
    .ready = frame_handle_ready,
    .failed = frame_handle_failed,
    .damage = frame_handle_damage,
};

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version) {
  struct capture_context *ctx = data;
  if (strcmp(interface, wl_shm_interface.name) == 0) {
    ctx->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    if (!ctx->output) {
      ctx->output = wl_registry_bind(registry, name, &wl_output_interface, 1);
    }
  } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
    uint32_t bind_version = version < 3 ? version : 3;
    ctx->manager = wl_registry_bind(registry, name,
                                    &zwlr_screencopy_manager_v1_interface,
                                    bind_version);
  }
}

static void registry_handle_global_remove(void *data,
                                          struct wl_registry *registry,
                                          uint32_t name) {
  (void)data;
  (void)registry;
  (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

int capture_init(struct capture_context **out_ctx, int overlay_cursor) {
  struct capture_context *ctx = calloc(1, sizeof(*ctx));
  if (!ctx) {
    return -1;
  }
  ctx->buffer.fd = -1;
  ctx->overlay_cursor = overlay_cursor;

  ctx->display = wl_display_connect(NULL);
  if (!ctx->display) {
    fprintf(stderr, "wl_display_connect failed\n");
    free(ctx);
    return -1;
  }

  ctx->registry = wl_display_get_registry(ctx->display);
  if (!ctx->registry) {
    fprintf(stderr, "wl_display_get_registry failed\n");
    wl_display_disconnect(ctx->display);
    free(ctx);
    return -1;
  }

  wl_registry_add_listener(ctx->registry, &registry_listener, ctx);
  wl_display_roundtrip(ctx->display);
  wl_display_roundtrip(ctx->display);

  if (!ctx->shm || !ctx->manager || !ctx->output) {
    fprintf(stderr, "Missing Wayland globals (shm=%p manager=%p output=%p)\n",
            (void *)ctx->shm, (void *)ctx->manager, (void *)ctx->output);
    capture_shutdown(ctx);
    return -1;
  }

  *out_ctx = ctx;
  return 0;
}

void capture_set_region(struct capture_context *ctx, int x, int y, int width,
                        int height) {
  if (!ctx) {
    return;
  }
  if (width <= 0 || height <= 0) {
    ctx->has_region = 0;
    return;
  }
  ctx->has_region = 1;
  ctx->region_x = x;
  ctx->region_y = y;
  ctx->region_width = width;
  ctx->region_height = height;
}

int capture_next_frame(struct capture_context *ctx, struct capture_frame *out) {
  struct frame_state state;
  memset(&state, 0, sizeof(state));
  state.ctx = ctx;

  if (ctx->has_region) {
    state.frame = zwlr_screencopy_manager_v1_capture_output_region(
        ctx->manager, ctx->overlay_cursor, ctx->output, ctx->region_x,
        ctx->region_y, ctx->region_width, ctx->region_height);
  } else {
    state.frame = zwlr_screencopy_manager_v1_capture_output(
        ctx->manager, ctx->overlay_cursor, ctx->output);
  }
  if (!state.frame) {
    fprintf(stderr, "capture_output failed\n");
    return -1;
  }

  zwlr_screencopy_frame_v1_add_listener(state.frame, &frame_listener, &state);
  wl_display_flush(ctx->display);

  while (!state.done) {
    if (wl_display_dispatch(ctx->display) < 0) {
      fprintf(stderr, "wl_display_dispatch failed\n");
      state.failed = 1;
      break;
    }
  }

  zwlr_screencopy_frame_v1_destroy(state.frame);

  if (state.failed) {
    return -1;
  }

  if (!ctx->buffer.data) {
    fprintf(stderr, "No buffer data available\n");
    return -1;
  }

  out->format = ctx->buffer.format;
  out->width = ctx->buffer.width;
  out->height = ctx->buffer.height;
  out->stride = ctx->buffer.stride;
  out->data = ctx->buffer.data;
  out->y_invert = state.y_invert;

  return 0;
}

void capture_shutdown(struct capture_context *ctx) {
  if (!ctx) {
    return;
  }

  if (ctx->buffer.buffer) {
    wl_buffer_destroy(ctx->buffer.buffer);
  }
  if (ctx->buffer.pool) {
    wl_shm_pool_destroy(ctx->buffer.pool);
  }
  if (ctx->buffer.data) {
    munmap(ctx->buffer.data, ctx->buffer.size);
  }
  if (ctx->buffer.fd >= 0) {
    close(ctx->buffer.fd);
  }

  if (ctx->manager) {
    zwlr_screencopy_manager_v1_destroy(ctx->manager);
  }
  if (ctx->output) {
    wl_output_destroy(ctx->output);
  }
  if (ctx->shm) {
    wl_shm_destroy(ctx->shm);
  }
  if (ctx->registry) {
    wl_registry_destroy(ctx->registry);
  }
  if (ctx->display) {
    wl_display_disconnect(ctx->display);
  }

  free(ctx);
}
