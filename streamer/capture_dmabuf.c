#define _GNU_SOURCE

#include "capture_dmabuf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <wayland-client.h>

#include "wlr-export-dmabuf-unstable-v1-client-protocol.h"

struct dmabuf_capture_context {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_output *output;
  struct zwlr_export_dmabuf_manager_v1 *manager;
  int overlay_cursor;
};

struct frame_state {
  struct dmabuf_capture_context *ctx;
  struct zwlr_export_dmabuf_frame_v1 *frame;
  struct dmabuf_frame *out;
  int done;
  int failed;
  int frame_received;
  int objects_received;
  int expected_objects;
};

static void frame_handle_frame(void *data,
                               struct zwlr_export_dmabuf_frame_v1 *frame,
                               uint32_t width, uint32_t height,
                               uint32_t offset_x, uint32_t offset_y,
                               uint32_t buffer_flags, uint32_t flags,
                               uint32_t format, uint32_t mod_high,
                               uint32_t mod_low, uint32_t num_objects) {
  (void)frame;
  (void)offset_x;
  (void)offset_y;
  (void)buffer_flags;
  struct frame_state *state = data;

  state->out->width = width;
  state->out->height = height;
  state->out->format = format;
  state->out->modifier = ((uint64_t)mod_high << 32) | mod_low;
  state->out->flags = (int)flags;
  state->out->num_objects = 0;
  state->expected_objects = (int)num_objects;
  state->frame_received = 1;

  /* Initialize all object FDs to -1 */
  for (int i = 0; i < 4; i++) {
    state->out->objects[i].fd = -1;
  }
}

static void frame_handle_object(void *data,
                                struct zwlr_export_dmabuf_frame_v1 *frame,
                                uint32_t index, int fd, uint32_t size,
                                uint32_t offset, uint32_t stride,
                                uint32_t plane_index) {
  (void)frame;
  struct frame_state *state = data;

  if (index >= 4) {
    fprintf(stderr, "dmabuf: object index %u out of range\n", index);
    close(fd);
    return;
  }

  state->out->objects[index].fd = fd;
  state->out->objects[index].size = size;
  state->out->objects[index].offset = offset;
  state->out->objects[index].stride = stride;
  state->out->objects[index].plane_idx = plane_index;
  state->objects_received++;

  if (index >= (uint32_t)state->out->num_objects) {
    state->out->num_objects = (int)index + 1;
  }
}

static void frame_handle_ready(void *data,
                               struct zwlr_export_dmabuf_frame_v1 *frame,
                               uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                               uint32_t tv_nsec) {
  (void)frame;
  (void)tv_sec_hi;
  (void)tv_sec_lo;
  (void)tv_nsec;
  struct frame_state *state = data;
  state->done = 1;
}

static void frame_handle_cancel(void *data,
                                struct zwlr_export_dmabuf_frame_v1 *frame,
                                uint32_t reason) {
  (void)frame;
  struct frame_state *state = data;

  const char *reason_str = "unknown";
  switch (reason) {
    case ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_TEMPORARY:
      reason_str = "temporary";
      break;
    case ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_PERMANENT:
      reason_str = "permanent";
      break;
    case ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_RESIZING:
      reason_str = "resizing";
      break;
  }
  fprintf(stderr, "dmabuf: frame cancelled (%s)\n", reason_str);

  state->failed = 1;
  state->done = 1;
}

static const struct zwlr_export_dmabuf_frame_v1_listener frame_listener = {
    .frame = frame_handle_frame,
    .object = frame_handle_object,
    .ready = frame_handle_ready,
    .cancel = frame_handle_cancel,
};

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version) {
  (void)version;
  struct dmabuf_capture_context *ctx = data;

  if (strcmp(interface, wl_output_interface.name) == 0) {
    if (!ctx->output) {
      ctx->output = wl_registry_bind(registry, name, &wl_output_interface, 1);
    }
  } else if (strcmp(interface,
                    zwlr_export_dmabuf_manager_v1_interface.name) == 0) {
    ctx->manager = wl_registry_bind(
        registry, name, &zwlr_export_dmabuf_manager_v1_interface, 1);
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

int dmabuf_capture_init(struct dmabuf_capture_context **out_ctx,
                        int overlay_cursor) {
  struct dmabuf_capture_context *ctx = calloc(1, sizeof(*ctx));
  if (!ctx) {
    return -1;
  }
  ctx->overlay_cursor = overlay_cursor;

  ctx->display = wl_display_connect(NULL);
  if (!ctx->display) {
    fprintf(stderr, "dmabuf: wl_display_connect failed\n");
    free(ctx);
    return -1;
  }

  ctx->registry = wl_display_get_registry(ctx->display);
  if (!ctx->registry) {
    fprintf(stderr, "dmabuf: wl_display_get_registry failed\n");
    wl_display_disconnect(ctx->display);
    free(ctx);
    return -1;
  }

  wl_registry_add_listener(ctx->registry, &registry_listener, ctx);
  wl_display_roundtrip(ctx->display);
  wl_display_roundtrip(ctx->display);

  if (!ctx->manager) {
    fprintf(stderr,
            "dmabuf: wlr-export-dmabuf-unstable-v1 not supported by compositor\n");
    dmabuf_capture_shutdown(ctx);
    return -1;
  }

  if (!ctx->output) {
    fprintf(stderr, "dmabuf: no output found\n");
    dmabuf_capture_shutdown(ctx);
    return -1;
  }

  *out_ctx = ctx;
  return 0;
}

int dmabuf_capture_next_frame(struct dmabuf_capture_context *ctx,
                              struct dmabuf_frame *out) {
  struct frame_state state;
  memset(&state, 0, sizeof(state));
  memset(out, 0, sizeof(*out));
  state.ctx = ctx;
  state.out = out;

  /* Initialize FDs to -1 */
  for (int i = 0; i < 4; i++) {
    out->objects[i].fd = -1;
  }

  state.frame = zwlr_export_dmabuf_manager_v1_capture_output(
      ctx->manager, ctx->overlay_cursor, ctx->output);
  if (!state.frame) {
    fprintf(stderr, "dmabuf: capture_output failed\n");
    return -1;
  }

  zwlr_export_dmabuf_frame_v1_add_listener(state.frame, &frame_listener,
                                           &state);
  wl_display_flush(ctx->display);

  while (!state.done) {
    if (wl_display_dispatch(ctx->display) < 0) {
      fprintf(stderr, "dmabuf: wl_display_dispatch failed\n");
      state.failed = 1;
      break;
    }
  }

  zwlr_export_dmabuf_frame_v1_destroy(state.frame);

  if (state.failed) {
    /* Close any FDs we received before failure */
    for (int i = 0; i < 4; i++) {
      if (out->objects[i].fd >= 0) {
        close(out->objects[i].fd);
        out->objects[i].fd = -1;
      }
    }
    return -1;
  }

  return 0;
}

int dmabuf_frame_map(struct dmabuf_frame *frame) {
  if (!frame || frame->num_objects < 1) {
    return -1;
  }

  /* For simple single-plane formats, map the first object */
  int fd = frame->objects[0].fd;
  if (fd < 0) {
    fprintf(stderr, "dmabuf: invalid fd for mapping\n");
    return -1;
  }

  size_t size = frame->objects[0].size;
  if (size == 0) {
    /* Estimate size from stride and height */
    size = (size_t)frame->objects[0].stride * frame->height;
  }

  void *data = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    fprintf(stderr, "dmabuf: mmap failed: %s\n", strerror(errno));
    return -1;
  }

  frame->mapped_data = data;
  frame->mapped_size = size;
  return 0;
}

void dmabuf_frame_release(struct dmabuf_frame *frame) {
  if (!frame) {
    return;
  }

  /* Unmap if mapped */
  if (frame->mapped_data && frame->mapped_data != MAP_FAILED) {
    munmap(frame->mapped_data, frame->mapped_size);
    frame->mapped_data = NULL;
    frame->mapped_size = 0;
  }

  /* Close all FDs */
  for (int i = 0; i < 4; i++) {
    if (frame->objects[i].fd >= 0) {
      close(frame->objects[i].fd);
      frame->objects[i].fd = -1;
    }
  }
}

void dmabuf_capture_shutdown(struct dmabuf_capture_context *ctx) {
  if (!ctx) {
    return;
  }

  if (ctx->manager) {
    zwlr_export_dmabuf_manager_v1_destroy(ctx->manager);
  }
  if (ctx->output) {
    wl_output_destroy(ctx->output);
  }
  if (ctx->registry) {
    wl_registry_destroy(ctx->registry);
  }
  if (ctx->display) {
    wl_display_disconnect(ctx->display);
  }

  free(ctx);
}
