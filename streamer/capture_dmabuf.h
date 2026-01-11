#ifndef WLCAST_CAPTURE_DMABUF_H
#define WLCAST_CAPTURE_DMABUF_H

#include <stddef.h>
#include <stdint.h>

struct dmabuf_capture_context;
struct dmabuf_pending_frame;

/* Information about a captured DMABUF frame */
struct dmabuf_frame {
  uint32_t width;
  uint32_t height;
  uint32_t format;      /* DRM format (e.g., DRM_FORMAT_XRGB8888) */
  uint64_t modifier;    /* DRM format modifier */
  int num_objects;      /* Number of DMA buffer objects (planes) */
  struct {
    int fd;             /* File descriptor for this object */
    uint32_t size;      /* Size in bytes */
    uint32_t offset;    /* Offset within the fd */
    uint32_t stride;    /* Line stride in bytes */
    uint32_t plane_idx; /* Which plane this object represents */
  } objects[4];
  int flags;            /* Frame flags (transient, etc.) */
  void *mapped_data;    /* Mapped memory (set by dmabuf_frame_map) */
  size_t mapped_size;   /* Size of mapped region */
};

/* Initialize DMABUF capture using wlr-export-dmabuf protocol.
 * Returns 0 on success, -1 on failure (protocol not supported, etc.) */
int dmabuf_capture_init(struct dmabuf_capture_context **out_ctx,
                        int overlay_cursor);

/* Capture the next frame. Returns DMABUF file descriptors.
 * Caller must call dmabuf_frame_release when done.
 * Returns 0 on success, -1 on failure. */
int dmabuf_capture_next_frame(struct dmabuf_capture_context *ctx,
                              struct dmabuf_frame *out);

/* Map the DMABUF to CPU-accessible memory.
 * Sets frame->mapped_data and frame->mapped_size.
 * Returns 0 on success, -1 on failure. */
int dmabuf_frame_map(struct dmabuf_frame *frame);

/* Release a captured frame (unmap memory and close FDs) */
void dmabuf_frame_release(struct dmabuf_frame *frame);

/* Shutdown and free resources */
void dmabuf_capture_shutdown(struct dmabuf_capture_context *ctx);

/* === Async capture API for pipelining === */

/* Start capturing a frame asynchronously.
 * Returns a pending frame handle, or NULL on error.
 * Must be finished with dmabuf_capture_finish() or dmabuf_capture_cancel(). */
struct dmabuf_pending_frame *dmabuf_capture_request(struct dmabuf_capture_context *ctx);

/* Check if a pending frame is ready (non-blocking).
 * Returns 1 if ready, 0 if still pending, -1 on error. */
int dmabuf_capture_poll(struct dmabuf_capture_context *ctx,
                        struct dmabuf_pending_frame *pending);

/* Wait for pending frame and retrieve result (blocking).
 * Returns 0 on success, -1 on failure.
 * Frees the pending frame handle. */
int dmabuf_capture_finish(struct dmabuf_capture_context *ctx,
                          struct dmabuf_pending_frame *pending,
                          struct dmabuf_frame *out);

/* Cancel a pending frame request.
 * Frees the pending frame handle. */
void dmabuf_capture_cancel(struct dmabuf_pending_frame *pending);

/* Get the wayland display FD for external polling.
 * Can be used with poll()/select() to wait for frame readiness. */
int dmabuf_capture_get_fd(struct dmabuf_capture_context *ctx);

#endif /* WLCAST_CAPTURE_DMABUF_H */
