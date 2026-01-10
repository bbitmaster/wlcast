#ifndef WLCAST_V4L2_COMMON_H
#define WLCAST_V4L2_COMMON_H

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>

/* DRM format fourcc codes */
#define DRM_FORMAT_XRGB8888 0x34325258 /* XR24 */
#define DRM_FORMAT_ARGB8888 0x34325241 /* AR24 */

/* V4L2 pixel format fourcc (matches V4L2_PIX_FMT_YUYV) */
#define FOURCC_YUYV 0x56595559 /* YUYV */

/* Convert fourcc to printable string (uses static buffer) */
static inline const char *fourcc_to_str(uint32_t fmt) {
  static char str[5];
  str[0] = (char)(fmt & 0x7f);
  str[1] = (char)((fmt >> 8) & 0x7f);
  str[2] = (char)((fmt >> 16) & 0x7f);
  str[3] = (char)((fmt >> 24) & 0x7f);
  str[4] = '\0';
  return str;
}

/* EINTR-safe ioctl wrapper */
static inline int xioctl(int fd, unsigned long request, void *arg) {
  int rc;
  do {
    rc = ioctl(fd, request, arg);
  } while (rc == -1 && errno == EINTR);
  return rc;
}

/* Check if debug is enabled via environment variable */
static inline int v4l2_debug_enabled(const char *env_var) {
  return getenv(env_var) != NULL;
}

#endif /* WLCAST_V4L2_COMMON_H */
