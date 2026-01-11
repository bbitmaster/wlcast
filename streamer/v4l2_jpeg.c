#include "v4l2_jpeg.h"
#include "v4l2_common.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <wayland-client.h>

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#define USE_NEON 1
#endif

static unsigned int max_u32(unsigned int a, unsigned int b) {
  return a > b ? a : b;
}

static int debug_enabled(void) {
  return v4l2_debug_enabled("SM_V4L2_DEBUG");
}

static void dump_pix_mp(const char *label, const struct v4l2_format *fmt) {
  if (!debug_enabled()) {
    return;
  }
  fprintf(stderr, "%s: fmt=%s planes=%u %ux%u\n", label,
          fourcc_to_str(fmt->fmt.pix_mp.pixelformat),
          fmt->fmt.pix_mp.num_planes, fmt->fmt.pix_mp.width,
          fmt->fmt.pix_mp.height);
  for (unsigned int i = 0; i < fmt->fmt.pix_mp.num_planes; ++i) {
    fprintf(stderr, "  plane[%u]: bpl=%u size=%u\n", i,
            fmt->fmt.pix_mp.plane_fmt[i].bytesperline,
            fmt->fmt.pix_mp.plane_fmt[i].sizeimage);
  }
}

static void dump_qbuf_planes(const struct v4l2_jpeg_encoder *enc,
                             const struct v4l2_plane *planes) {
  if (!debug_enabled()) {
    return;
  }
  fprintf(stderr, "v4l2 qbuf output: memory=%u planes=%u\n", enc->out_memory,
          enc->out_num_planes);
  for (unsigned int i = 0; i < enc->out_num_planes; ++i) {
    fprintf(stderr,
            "  plane[%u]: bytesused=%u length=%u offset=%u bpl=%u sizeimage=%u map=%u",
            i, planes[i].bytesused, planes[i].length, planes[i].data_offset,
            enc->out_bytesperline[i], enc->out_plane_size[i],
            enc->out_map_size[i]);
    if (enc->out_memory == V4L2_MEMORY_DMABUF) {
      fprintf(stderr, " fd=%d\n", enc->out_dmabuf_fd[i]);
    } else if (enc->out_memory == V4L2_MEMORY_USERPTR) {
      fprintf(stderr, " userptr=%p\n", (void *)(uintptr_t)planes[i].m.userptr);
    } else {
      fprintf(stderr, "\n");
    }
  }
}

static inline unsigned char clamp_u8(int value) {
  return (unsigned char)(value < 0 ? 0 : (value > 255 ? 255 : value));
}

struct format_info {
  uint32_t format;
  int bpp;
  int r_off;
  int g_off;
  int b_off;
};

/* Note: wl_shm uses enum 0/1 for ARGB8888/XRGB8888 (legacy), but fourcc for others.
 * DRM always uses fourcc codes (defined in v4l2_common.h). */

static int get_format_info(uint32_t format, struct format_info *info) {
  switch (format) {
    /* wl_shm formats use fourcc (same as DRM) except for legacy ARGB/XRGB */
    case WL_SHM_FORMAT_BGR888:   /* = DRM_FORMAT_BGR888 = 0x34324742 */
      *info = (struct format_info){format, 3, 2, 1, 0};
      return 0;
    case WL_SHM_FORMAT_RGB888:   /* = DRM_FORMAT_RGB888 = 0x34324752 */
      *info = (struct format_info){format, 3, 0, 1, 2};
      return 0;
    case WL_SHM_FORMAT_XRGB8888: /* = 1 (legacy wl_shm enum) */
    case WL_SHM_FORMAT_ARGB8888: /* = 0 (legacy wl_shm enum) */
    case DRM_FORMAT_XRGB8888:    /* = 0x34325258 (fourcc XR24) */
    case DRM_FORMAT_ARGB8888:    /* = 0x34325241 (fourcc AR24) */
      /* Memory order is B, G, R, X/A on little endian */
      *info = (struct format_info){format, 4, 2, 1, 0};
      return 0;
    case WL_SHM_FORMAT_XBGR8888: /* = DRM_FORMAT_XBGR8888 = 0x34324258 */
    case WL_SHM_FORMAT_ABGR8888: /* = DRM_FORMAT_ABGR8888 = 0x34324241 */
      /* Memory order is R, G, B, X/A */
      *info = (struct format_info){format, 4, 0, 1, 2};
      return 0;
    default:
      return -1;
  }
}

static void bgr_to_yuv(uint8_t b, uint8_t g, uint8_t r, uint8_t *y,
                       uint8_t *u, uint8_t *v) {
  /* JFIF full-range YCbCr (Y: 0-255, Cb/Cr: 0-255 with 128 neutral)
   * Y  =  0.299*R + 0.587*G + 0.114*B
   * Cb = -0.169*R - 0.331*G + 0.500*B + 128
   * Cr =  0.500*R - 0.419*G - 0.081*B + 128
   * Coefficients scaled by 256 for fixed-point math */
  int y_val = (77 * r + 150 * g + 29 * b + 128) >> 8;
  int u_val = ((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128;
  int v_val = ((128 * r - 107 * g - 21 * b + 128) >> 8) + 128;
  *y = clamp_u8(y_val);
  *u = clamp_u8(u_val);
  *v = clamp_u8(v_val);
}

static int convert_to_uyvy(const struct capture_frame *frame, void *dst,
                           unsigned int dst_stride) {
  struct format_info info;
  if (get_format_info(frame->format, &info) != 0) {
    fprintf(stderr, "Unsupported wl_shm format for HW JPEG: %u\n",
            frame->format);
    return -1;
  }

  if (frame->width % 2 != 0) {
    fprintf(stderr, "Width must be even for UYVY conversion\n");
    return -1;
  }

  for (uint32_t y = 0; y < frame->height; ++y) {
    const uint8_t *src_row = (const uint8_t *)frame->data + y * frame->stride;
    if (frame->y_invert) {
      src_row = (const uint8_t *)frame->data +
                (frame->height - 1 - y) * frame->stride;
    }
    uint8_t *dst_row = (uint8_t *)dst + y * dst_stride;

    for (uint32_t x = 0; x < frame->width; x += 2) {
      const uint8_t *p0 = src_row + x * info.bpp;
      const uint8_t *p1 = src_row + (x + 1) * info.bpp;

      uint8_t y0, u0, v0;
      uint8_t y1, u1, v1;
      bgr_to_yuv(p0[info.b_off], p0[info.g_off], p0[info.r_off], &y0, &u0, &v0);
      bgr_to_yuv(p1[info.b_off], p1[info.g_off], p1[info.r_off], &y1, &u1, &v1);

      uint8_t u = (uint8_t)(((int)u0 + (int)u1) / 2);
      uint8_t v = (uint8_t)(((int)v0 + (int)v1) / 2);

      dst_row[x * 2 + 0] = u;
      dst_row[x * 2 + 1] = y0;
      dst_row[x * 2 + 2] = v;
      dst_row[x * 2 + 3] = y1;
    }
  }

  return 0;
}

static int convert_to_nv12(const struct capture_frame *frame, uint8_t *y_plane,
                           unsigned int y_stride, uint8_t *uv_plane,
                           unsigned int uv_stride) {
  struct format_info info;
  if (get_format_info(frame->format, &info) != 0) {
    fprintf(stderr, "Unsupported wl_shm format for HW JPEG: %u\n",
            frame->format);
    return -1;
  }

  if ((frame->width % 2) != 0 || (frame->height % 2) != 0) {
    fprintf(stderr, "Width/height must be even for NV12 conversion\n");
    return -1;
  }

  for (uint32_t y = 0; y < frame->height; ++y) {
    const uint8_t *src_row = (const uint8_t *)frame->data + y * frame->stride;
    if (frame->y_invert) {
      src_row = (const uint8_t *)frame->data +
                (frame->height - 1 - y) * frame->stride;
    }
    uint8_t *y_row = y_plane + y * y_stride;

    for (uint32_t x = 0; x < frame->width; ++x) {
      const uint8_t *p = src_row + x * info.bpp;
      uint8_t y_val, u_val, v_val;
      bgr_to_yuv(p[info.b_off], p[info.g_off], p[info.r_off], &y_val, &u_val,
                 &v_val);
      y_row[x] = y_val;
    }
  }

  for (uint32_t y = 0; y < frame->height; y += 2) {
    const uint8_t *src_row0 =
        (const uint8_t *)frame->data + y * frame->stride;
    const uint8_t *src_row1 =
        (const uint8_t *)frame->data + (y + 1) * frame->stride;
    if (frame->y_invert) {
      src_row0 = (const uint8_t *)frame->data +
                 (frame->height - 1 - y) * frame->stride;
      src_row1 = (const uint8_t *)frame->data +
                 (frame->height - 2 - y) * frame->stride;
    }
    uint8_t *uv_row = uv_plane + (y / 2) * uv_stride;

    for (uint32_t x = 0; x < frame->width; x += 2) {
      const uint8_t *p0 = src_row0 + x * info.bpp;
      const uint8_t *p1 = src_row0 + (x + 1) * info.bpp;
      const uint8_t *p2 = src_row1 + x * info.bpp;
      const uint8_t *p3 = src_row1 + (x + 1) * info.bpp;

      uint8_t y0, u0, v0;
      uint8_t y1, u1, v1;
      uint8_t y2, u2, v2;
      uint8_t y3, u3, v3;
      bgr_to_yuv(p0[info.b_off], p0[info.g_off], p0[info.r_off], &y0, &u0,
                 &v0);
      bgr_to_yuv(p1[info.b_off], p1[info.g_off], p1[info.r_off], &y1, &u1,
                 &v1);
      bgr_to_yuv(p2[info.b_off], p2[info.g_off], p2[info.r_off], &y2, &u2,
                 &v2);
      bgr_to_yuv(p3[info.b_off], p3[info.g_off], p3[info.r_off], &y3, &u3,
                 &v3);

      uint8_t u = (uint8_t)(((int)u0 + (int)u1 + (int)u2 + (int)u3) / 4);
      uint8_t v = (uint8_t)(((int)v0 + (int)v1 + (int)v2 + (int)v3) / 4);

      uv_row[x] = u;
      uv_row[x + 1] = v;
    }
  }

  return 0;
}

static int convert_to_yuv420p(const struct capture_frame *frame, uint8_t *y_plane,
                              unsigned int y_stride, uint8_t *u_plane,
                              unsigned int u_stride, uint8_t *v_plane,
                              unsigned int v_stride) {
  struct format_info info;
  if (get_format_info(frame->format, &info) != 0) {
    fprintf(stderr, "Unsupported wl_shm format for HW JPEG: %u\n",
            frame->format);
    return -1;
  }

  if ((frame->width % 2) != 0 || (frame->height % 2) != 0) {
    fprintf(stderr, "Width/height must be even for YUV420 conversion\n");
    return -1;
  }

  for (uint32_t y = 0; y < frame->height; ++y) {
    const uint8_t *src_row = (const uint8_t *)frame->data + y * frame->stride;
    if (frame->y_invert) {
      src_row = (const uint8_t *)frame->data +
                (frame->height - 1 - y) * frame->stride;
    }
    uint8_t *y_row = y_plane + y * y_stride;

    for (uint32_t x = 0; x < frame->width; ++x) {
      const uint8_t *p = src_row + x * info.bpp;
      uint8_t y_val, u_val, v_val;
      bgr_to_yuv(p[info.b_off], p[info.g_off], p[info.r_off], &y_val, &u_val,
                 &v_val);
      y_row[x] = y_val;
    }
  }

  for (uint32_t y = 0; y < frame->height; y += 2) {
    const uint8_t *src_row0 =
        (const uint8_t *)frame->data + y * frame->stride;
    const uint8_t *src_row1 =
        (const uint8_t *)frame->data + (y + 1) * frame->stride;
    if (frame->y_invert) {
      src_row0 = (const uint8_t *)frame->data +
                 (frame->height - 1 - y) * frame->stride;
      src_row1 = (const uint8_t *)frame->data +
                 (frame->height - 2 - y) * frame->stride;
    }
    uint8_t *u_row = u_plane + (y / 2) * u_stride;
    uint8_t *v_row = v_plane + (y / 2) * v_stride;

    for (uint32_t x = 0; x < frame->width; x += 2) {
      const uint8_t *p0 = src_row0 + x * info.bpp;
      const uint8_t *p1 = src_row0 + (x + 1) * info.bpp;
      const uint8_t *p2 = src_row1 + x * info.bpp;
      const uint8_t *p3 = src_row1 + (x + 1) * info.bpp;

      uint8_t y0, u0, v0;
      uint8_t y1, u1, v1;
      uint8_t y2, u2, v2;
      uint8_t y3, u3, v3;
      bgr_to_yuv(p0[info.b_off], p0[info.g_off], p0[info.r_off], &y0, &u0,
                 &v0);
      bgr_to_yuv(p1[info.b_off], p1[info.g_off], p1[info.r_off], &y1, &u1,
                 &v1);
      bgr_to_yuv(p2[info.b_off], p2[info.g_off], p2[info.r_off], &y2, &u2,
                 &v2);
      bgr_to_yuv(p3[info.b_off], p3[info.g_off], p3[info.r_off], &y3, &u3,
                 &v3);

      uint8_t u = (uint8_t)(((int)u0 + (int)u1 + (int)u2 + (int)u3) / 4);
      uint8_t v = (uint8_t)(((int)v0 + (int)v1 + (int)v2 + (int)v3) / 4);

      u_row[x / 2] = u;
      v_row[x / 2] = v;
    }
  }

  return 0;
}

static unsigned int bytes_used_for_plane(const struct v4l2_jpeg_encoder *enc,
                                         unsigned int plane) {
  unsigned int h = (unsigned int)enc->height;
  unsigned int bpl = enc->out_bytesperline[plane];
  if (enc->out_format == V4L2_PIX_FMT_YUYV ||
      enc->out_format == V4L2_PIX_FMT_UYVY) {
    if (plane == 0) {
      return bpl * h;
    }
  } else if (enc->out_format == V4L2_PIX_FMT_NV12M ||
             enc->out_format == V4L2_PIX_FMT_NV12) {
    if (plane == 0) {
      return bpl * h;
    }
    if (plane == 1) {
      return bpl * (h / 2u);
    }
  } else if (enc->out_format == V4L2_PIX_FMT_YUV420M) {
    if (plane == 0) {
      return bpl * h;
    }
    if (plane == 1 || plane == 2) {
      return bpl * (h / 2u);
    }
  }
  return enc->out_plane_size[plane];
}

#ifdef USE_NEON
/* NEON-optimized BGRX to YUYV conversion - processes 8 pixels at a time */
static void convert_row_bgrx_to_yuyv_neon(const uint8_t *src, uint8_t *dst,
                                          uint32_t width) {
  /* Coefficients for Y = (77*R + 150*G + 29*B + 128) >> 8 */
  const uint8x8_t coef_ry = vdup_n_u8(77);
  const uint8x8_t coef_gy = vdup_n_u8(150);
  const uint8x8_t coef_by = vdup_n_u8(29);

  uint32_t x = 0;

  /* Process 8 pixels (32 bytes in, 16 bytes out) at a time */
  for (; x + 8 <= width; x += 8) {
    /* Load 8 BGRX pixels, deinterleaved into B, G, R, X channels */
    uint8x8x4_t bgrx = vld4_u8(src);
    src += 32;

    uint8x8_t b = bgrx.val[0];
    uint8x8_t g = bgrx.val[1];
    uint8x8_t r = bgrx.val[2];

    /* Compute Y for all 8 pixels: Y = (77*R + 150*G + 29*B + 128) >> 8 */
    uint16x8_t y16 = vmull_u8(r, coef_ry);
    y16 = vmlal_u8(y16, g, coef_gy);
    y16 = vmlal_u8(y16, b, coef_by);
    y16 = vaddq_u16(y16, vdupq_n_u16(128));
    uint8x8_t y = vshrn_n_u16(y16, 8);

    /* For U/V, we need signed arithmetic. Widen to 16-bit signed. */
    int16x8_t rs = vreinterpretq_s16_u16(vmovl_u8(r));
    int16x8_t gs = vreinterpretq_s16_u16(vmovl_u8(g));
    int16x8_t bs = vreinterpretq_s16_u16(vmovl_u8(b));

    /* U = (-43*R - 85*G + 128*B + 128) >> 8 + 128
       V = (128*R - 107*G - 21*B + 128) >> 8 + 128 */
    int16x8_t u16 = vmulq_n_s16(bs, 128);
    u16 = vmlaq_n_s16(u16, rs, -43);
    u16 = vmlaq_n_s16(u16, gs, -85);
    u16 = vaddq_s16(u16, vdupq_n_s16(128));
    u16 = vshrq_n_s16(u16, 8);
    u16 = vaddq_s16(u16, vdupq_n_s16(128));

    int16x8_t v16 = vmulq_n_s16(rs, 128);
    v16 = vmlaq_n_s16(v16, gs, -107);
    v16 = vmlaq_n_s16(v16, bs, -21);
    v16 = vaddq_s16(v16, vdupq_n_s16(128));
    v16 = vshrq_n_s16(v16, 8);
    v16 = vaddq_s16(v16, vdupq_n_s16(128));

    /* Clamp to 0-255 and narrow to 8-bit */
    uint8x8_t u8 = vqmovun_s16(u16);
    uint8x8_t v8 = vqmovun_s16(v16);

    /* Average adjacent U and V values for 4:2:2 subsampling
       u8 = [U0, U1, U2, U3, U4, U5, U6, U7]
       We want: [(U0+U1)/2, (U2+U3)/2, (U4+U5)/2, (U6+U7)/2] */
    uint8x8_t u_even = vuzp1_u8(u8, u8); /* U0, U2, U4, U6, U0, U2, U4, U6 */
    uint8x8_t u_odd = vuzp2_u8(u8, u8);  /* U1, U3, U5, U7, U1, U3, U5, U7 */
    uint8x8_t v_even = vuzp1_u8(v8, v8);
    uint8x8_t v_odd = vuzp2_u8(v8, v8);

    uint8x8_t u_avg = vhadd_u8(u_even, u_odd); /* Halving add = (a+b)/2 */
    uint8x8_t v_avg = vhadd_u8(v_even, v_odd);

    /* Interleave Y with U/V to create YUYV
       y = [Y0, Y1, Y2, Y3, Y4, Y5, Y6, Y7]
       We need: Y0 U0 Y1 V0 Y2 U1 Y3 V1 Y4 U2 Y5 V2 Y6 U3 Y7 V3 */
    uint8x8_t y_even = vuzp1_u8(y, y); /* Y0, Y2, Y4, Y6, ... */
    uint8x8_t y_odd = vuzp2_u8(y, y);  /* Y1, Y3, Y5, Y7, ... */

    /* Build YUYV output: each macro-pixel is Y0 U Y1 V */
    uint8x8x4_t yuyv;
    yuyv.val[0] = y_even;                               /* Y0, Y2, Y4, Y6 */
    yuyv.val[1] = vget_low_u8(vcombine_u8(u_avg, u_avg)); /* U0, U1, U2, U3 */
    yuyv.val[2] = y_odd;                                /* Y1, Y3, Y5, Y7 */
    yuyv.val[3] = vget_low_u8(vcombine_u8(v_avg, v_avg)); /* V0, V1, V2, V3 */

    /* Store interleaved as Y0 U0 Y1 V0 Y2 U1 Y3 V1 ... */
    vst4_lane_u8(dst + 0, yuyv, 0);
    vst4_lane_u8(dst + 4, yuyv, 1);
    vst4_lane_u8(dst + 8, yuyv, 2);
    vst4_lane_u8(dst + 12, yuyv, 3);
    dst += 16;
  }

  /* Handle remaining pixels with scalar code */
  for (; x < width; x += 2) {
    int b0 = src[0], g0 = src[1], r0 = src[2];
    int b1 = src[4], g1 = src[5], r1 = src[6];
    src += 8;

    int y0 = (77 * r0 + 150 * g0 + 29 * b0 + 128) >> 8;
    int y1 = (77 * r1 + 150 * g1 + 29 * b1 + 128) >> 8;
    int r_avg = r0 + r1, g_avg = g0 + g1, b_avg = b0 + b1;
    int u = ((-43 * r_avg - 85 * g_avg + 128 * b_avg + 256) >> 9) + 128;
    int v = ((128 * r_avg - 107 * g_avg - 21 * b_avg + 256) >> 9) + 128;

    *dst++ = (uint8_t)y0;
    *dst++ = clamp_u8(u);
    *dst++ = (uint8_t)y1;
    *dst++ = clamp_u8(v);
  }
}
#endif

static int convert_to_yuyv(const struct capture_frame *frame, void *dst,
                           unsigned int dst_stride) {
  struct format_info info;
  if (get_format_info(frame->format, &info) != 0) {
    fprintf(stderr, "Unsupported wl_shm format for HW JPEG: %u (0x%08x)\n",
            frame->format, frame->format);
    return -1;
  }
  if (frame->width % 2 != 0) {
    fprintf(stderr, "Width must be even for YUYV conversion\n");
    return -1;
  }

  const uint32_t width = frame->width;
  const uint32_t height = frame->height;
  const uint32_t src_stride = frame->stride;
  const int y_invert = frame->y_invert;
  const uint8_t *src_base = (const uint8_t *)frame->data;

#ifdef USE_NEON
  /* Use NEON for BGRX (4 bpp) format which is most common from Wayland */
  if (info.bpp == 4 && info.b_off == 0 && info.g_off == 1 && info.r_off == 2) {
    for (uint32_t row = 0; row < height; ++row) {
      const uint8_t *src_row = y_invert
          ? src_base + (height - 1 - row) * src_stride
          : src_base + row * src_stride;
      uint8_t *dst_row = (uint8_t *)dst + row * dst_stride;
      convert_row_bgrx_to_yuyv_neon(src_row, dst_row, width);
    }
    return 0;
  }
#endif

  /* Scalar fallback for other formats */
  const int bpp = info.bpp;
  const int r_off = info.r_off;
  const int g_off = info.g_off;
  const int b_off = info.b_off;

  for (uint32_t row = 0; row < height; ++row) {
    const uint8_t *src_row = y_invert
        ? src_base + (height - 1 - row) * src_stride
        : src_base + row * src_stride;
    uint8_t *dst_ptr = (uint8_t *)dst + row * dst_stride;
    const uint8_t *src_ptr = src_row;

    for (uint32_t x = 0; x < width; x += 2) {
      int r0 = src_ptr[r_off];
      int g0 = src_ptr[g_off];
      int b0 = src_ptr[b_off];
      src_ptr += bpp;

      int r1 = src_ptr[r_off];
      int g1 = src_ptr[g_off];
      int b1 = src_ptr[b_off];
      src_ptr += bpp;

      int y0 = (77 * r0 + 150 * g0 + 29 * b0 + 128) >> 8;
      int y1 = (77 * r1 + 150 * g1 + 29 * b1 + 128) >> 8;

      int r_avg = r0 + r1;
      int g_avg = g0 + g1;
      int b_avg = b0 + b1;
      int u = ((-43 * r_avg - 85 * g_avg + 128 * b_avg + 256) >> 9) + 128;
      int v = ((128 * r_avg - 107 * g_avg - 21 * b_avg + 256) >> 9) + 128;

      *dst_ptr++ = (uint8_t)y0;
      *dst_ptr++ = clamp_u8(u);
      *dst_ptr++ = (uint8_t)y1;
      *dst_ptr++ = clamp_u8(v);
    }
  }

  return 0;
}

static int queue_capture(struct v4l2_jpeg_encoder *enc) {
  struct v4l2_buffer buf;
  struct v4l2_plane plane[3];
  memset(&buf, 0, sizeof(buf));
  memset(&plane, 0, sizeof(plane));

  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = 0;
  buf.length = enc->cap_num_planes;
  buf.m.planes = plane;

  if (xioctl(enc->fd, VIDIOC_QBUF, &buf) != 0) {
    perror("VIDIOC_QBUF capture");
    return -1;
  }
  enc->cap_queued = 1;
  return 0;
}

static void release_output_buffers(struct v4l2_jpeg_encoder *enc) {
  if (enc->out_memory == V4L2_MEMORY_MMAP) {
    for (unsigned int i = 0; i < enc->out_num_planes; ++i) {
      if (enc->out_map[i] && enc->out_map[i] != MAP_FAILED) {
        munmap(enc->out_map[i], enc->out_map_size[i]);
        enc->out_map[i] = NULL;
      }
    }
  } else if (enc->out_memory == V4L2_MEMORY_USERPTR) {
    for (unsigned int i = 0; i < enc->out_num_planes; ++i) {
      free(enc->out_userptr[i]);
      enc->out_userptr[i] = NULL;
    }
  } else if (enc->out_memory == V4L2_MEMORY_DMABUF) {
    /* Check if using contiguous buffer (out_map_base is set) */
    if (enc->out_map_base && enc->out_map_base != MAP_FAILED) {
      /* Contiguous mode: one buffer, one fd */
      munmap(enc->out_map_base, enc->out_map_base_size);
      enc->out_map_base = NULL;
      enc->out_map_base_size = 0;
      /* Close only the first fd (all planes share same fd) */
      if (enc->out_dmabuf_fd[0] >= 0) {
        close(enc->out_dmabuf_fd[0]);
      }
      for (unsigned int i = 0; i < enc->out_num_planes; ++i) {
        enc->out_map[i] = NULL;
        enc->out_dmabuf_fd[i] = -1;
        enc->out_dmabuf_offset[i] = 0;
      }
    } else {
      /* Separate mode: each plane has its own buffer and fd */
      for (unsigned int i = 0; i < enc->out_num_planes; ++i) {
        if (enc->out_map[i] && enc->out_map[i] != MAP_FAILED) {
          munmap(enc->out_map[i], enc->out_map_size[i]);
          enc->out_map[i] = NULL;
        }
        if (enc->out_dmabuf_fd[i] >= 0) {
          close(enc->out_dmabuf_fd[i]);
          enc->out_dmabuf_fd[i] = -1;
        }
        enc->out_dmabuf_offset[i] = 0;
      }
    }
  }
}

static int allocate_output_userptr(struct v4l2_jpeg_encoder *enc) {
  for (unsigned int i = 0; i < enc->out_num_planes; ++i) {
    unsigned int size = enc->out_plane_size[i];
    if (size == 0) {
      size = enc->out_bytesperline[i] * (unsigned int)enc->height;
    }
    if (size == 0) {
      fprintf(stderr, "Invalid output plane size\n");
      return -1;
    }
    enc->out_userptr[i] = malloc(size);
    if (!enc->out_userptr[i]) {
      fprintf(stderr, "malloc output plane failed\n");
      return -1;
    }
  }
  return 0;
}

/* Allocate CONTIGUOUS dmabuf for all planes (single buffer with offsets) */
static int allocate_output_dmabuf_contiguous(struct v4l2_jpeg_encoder *enc) {
  int heap_fd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
  if (heap_fd < 0) {
    perror("open /dev/dma_heap/system");
    return -1;
  }

  unsigned int sizes[3] = {0};
  unsigned int total_size = 0;

  for (unsigned int i = 0; i < enc->out_num_planes; ++i) {
    unsigned int size = enc->out_plane_size[i];
    if (size == 0) {
      size = enc->out_bytesperline[i] * (unsigned int)enc->height;
      if (enc->out_num_planes == 2 && i == 1) {
        size /= 2u;
      } else if (enc->out_num_planes == 3 && i > 0) {
        size /= 2u;
      }
    }
    sizes[i] = size;
    total_size += size;
  }

  /* Allocate single contiguous buffer */
  struct dma_heap_allocation_data data;
  memset(&data, 0, sizeof(data));
  data.len = total_size;
  data.fd_flags = O_CLOEXEC | O_RDWR;
  data.heap_flags = 0;

  if (xioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) != 0) {
    perror("DMA_HEAP_IOCTL_ALLOC contiguous");
    close(heap_fd);
    return -1;
  }

  int dmabuf_fd = (int)data.fd;
  void *map_base = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        dmabuf_fd, 0);
  if (map_base == MAP_FAILED) {
    perror("mmap contiguous dmabuf");
    close(dmabuf_fd);
    close(heap_fd);
    return -1;
  }

  /* Set up planes with offsets into contiguous buffer */
  unsigned int offset = 0;
  for (unsigned int i = 0; i < enc->out_num_planes; ++i) {
    enc->out_dmabuf_fd[i] = dmabuf_fd;  /* Same fd for all planes */
    enc->out_dmabuf_offset[i] = offset;
    enc->out_map_size[i] = sizes[i];
    enc->out_map[i] = (char *)map_base + offset;
    offset += sizes[i];
  }

  /* Store base mapping info for cleanup */
  enc->out_map_base = map_base;
  enc->out_map_base_size = total_size;

  close(heap_fd);
  fprintf(stderr, "Allocated contiguous DMABUF: fd=%d total_size=%u\n",
          dmabuf_fd, total_size);
  return 0;
}

/* Allocate SEPARATE dmabufs per plane */
static int allocate_output_dmabuf_separate(struct v4l2_jpeg_encoder *enc) {
  int heap_fd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
  if (heap_fd < 0) {
    perror("open /dev/dma_heap/system");
    return -1;
  }

  unsigned int sizes[3] = {0};

  for (unsigned int i = 0; i < enc->out_num_planes; ++i) {
    unsigned int size = enc->out_plane_size[i];
    if (size == 0) {
      size = enc->out_bytesperline[i] * (unsigned int)enc->height;
      if (enc->out_num_planes == 2 && i == 1) {
        size /= 2u;
      } else if (enc->out_num_planes == 3 && i > 0) {
        size /= 2u;
      }
    }
    sizes[i] = size;
  }

  for (unsigned int i = 0; i < enc->out_num_planes; ++i) {
    struct dma_heap_allocation_data data;
    memset(&data, 0, sizeof(data));
    data.len = sizes[i];
    data.fd_flags = O_CLOEXEC | O_RDWR;
    data.heap_flags = 0;

    if (xioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) != 0) {
      perror("DMA_HEAP_IOCTL_ALLOC separate");
      for (unsigned int j = 0; j < i; ++j) {
        if (enc->out_map[j] && enc->out_map[j] != MAP_FAILED) {
          munmap(enc->out_map[j], enc->out_map_size[j]);
        }
        if (enc->out_dmabuf_fd[j] >= 0) {
          close(enc->out_dmabuf_fd[j]);
        }
      }
      close(heap_fd);
      return -1;
    }

    enc->out_dmabuf_fd[i] = (int)data.fd;
    enc->out_dmabuf_offset[i] = 0;
    enc->out_map_size[i] = sizes[i];

    enc->out_map[i] = mmap(NULL, sizes[i], PROT_READ | PROT_WRITE, MAP_SHARED,
                           enc->out_dmabuf_fd[i], 0);
    if (enc->out_map[i] == MAP_FAILED) {
      perror("mmap dmabuf plane");
      close(enc->out_dmabuf_fd[i]);
      for (unsigned int j = 0; j < i; ++j) {
        if (enc->out_map[j] && enc->out_map[j] != MAP_FAILED) {
          munmap(enc->out_map[j], enc->out_map_size[j]);
        }
        if (enc->out_dmabuf_fd[j] >= 0) {
          close(enc->out_dmabuf_fd[j]);
        }
      }
      close(heap_fd);
      return -1;
    }
  }

  close(heap_fd);
  fprintf(stderr, "Allocated separate DMABUFs per plane\n");
  return 0;
}

static int allocate_output_dmabuf(struct v4l2_jpeg_encoder *enc) {
  /* Try contiguous first for multi-plane, then fall back to separate */
  if (enc->out_num_planes > 1) {
    if (allocate_output_dmabuf_contiguous(enc) == 0) {
      return 0;
    }
    fprintf(stderr, "Contiguous DMABUF failed, trying separate buffers\n");
  }
  return allocate_output_dmabuf_separate(enc);
}

static int set_output_format(struct v4l2_jpeg_encoder *enc, int width,
                             int height, uint32_t pixfmt) {
  struct v4l2_format fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  fmt.fmt.pix_mp.width = width;
  fmt.fmt.pix_mp.height = height;
  fmt.fmt.pix_mp.pixelformat = pixfmt;
  fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;

  if (pixfmt == V4L2_PIX_FMT_NV12M) {
    fmt.fmt.pix_mp.num_planes = 2;
    fmt.fmt.pix_mp.plane_fmt[0].bytesperline = (unsigned int)width;
    fmt.fmt.pix_mp.plane_fmt[1].bytesperline = (unsigned int)width;
  } else if (pixfmt == V4L2_PIX_FMT_YUV420M) {
    fmt.fmt.pix_mp.num_planes = 3;
    fmt.fmt.pix_mp.plane_fmt[0].bytesperline = (unsigned int)width;
    fmt.fmt.pix_mp.plane_fmt[1].bytesperline = (unsigned int)width / 2u;
    fmt.fmt.pix_mp.plane_fmt[2].bytesperline = (unsigned int)width / 2u;
  } else {
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].bytesperline = (unsigned int)width * 2u;
  }

  if (xioctl(enc->fd, VIDIOC_S_FMT, &fmt) != 0) {
    return -1;
  }
  if (xioctl(enc->fd, VIDIOC_G_FMT, &fmt) != 0) {
    return -1;
  }

  enc->out_format = fmt.fmt.pix_mp.pixelformat;
  enc->out_num_planes = fmt.fmt.pix_mp.num_planes;
  for (unsigned int i = 0; i < enc->out_num_planes; ++i) {
    enc->out_bytesperline[i] = fmt.fmt.pix_mp.plane_fmt[i].bytesperline;
    enc->out_plane_size[i] = fmt.fmt.pix_mp.plane_fmt[i].sizeimage;
    if (enc->out_plane_size[i] == 0) {
      enc->out_plane_size[i] =
          enc->out_bytesperline[i] * (unsigned int)height;
      if (enc->out_num_planes == 2 && i == 1) {
        enc->out_plane_size[i] /=
            2u;
      } else if (enc->out_num_planes == 3 && i > 0) {
        enc->out_plane_size[i] /=
            2u;
      }
    }
  }

  dump_pix_mp("v4l2 output", &fmt);
  return 0;
}

static int find_jpeg_encoder(void) {
  /* Probe video devices to find the JPEG encoder (hantro-vpu VEPU) */
  const char *devices[] = {"/dev/video1", "/dev/video2", "/dev/video0",
                           "/dev/video3", "/dev/video4", NULL};

  for (int i = 0; devices[i]; i++) {
    int fd = open(devices[i], O_RDWR | O_NONBLOCK);
    if (fd < 0) continue;

    /* Check if this device supports JPEG capture */
    struct v4l2_fmtdesc fmtdesc;
    memset(&fmtdesc, 0, sizeof(fmtdesc));
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    int found_jpeg = 0;
    while (xioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
      if (fmtdesc.pixelformat == V4L2_PIX_FMT_JPEG) {
        found_jpeg = 1;
        break;
      }
      fmtdesc.index++;
    }

    if (found_jpeg) {
      if (debug_enabled()) {
        fprintf(stderr, "Found JPEG encoder at %s\n", devices[i]);
      }
      return fd;
    }
    close(fd);
  }

  fprintf(stderr, "No JPEG encoder found\n");
  return -1;
}

int v4l2_jpeg_init(struct v4l2_jpeg_encoder *enc, int width, int height,
                   int quality) {
  memset(enc, 0, sizeof(*enc));
  enc->fd = find_jpeg_encoder();
  if (enc->fd < 0) {
    return -1;
  }

  enc->width = width;
  enc->height = height;
  enc->quality = quality;

  /* Try NV12M first, then re-negotiate to YUYV for DMABUF later */
  if (set_output_format(enc, width, height, V4L2_PIX_FMT_NV12M) != 0 &&
      set_output_format(enc, width, height, V4L2_PIX_FMT_YUV420M) != 0 &&
      set_output_format(enc, width, height, V4L2_PIX_FMT_YUYV) != 0 &&
      set_output_format(enc, width, height, V4L2_PIX_FMT_UYVY) != 0) {
    perror("VIDIOC_S_FMT output");
    close(enc->fd);
    enc->fd = -1;
    return -1;
  }

  struct v4l2_format cap_fmt;
  memset(&cap_fmt, 0, sizeof(cap_fmt));
  cap_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  cap_fmt.fmt.pix_mp.width = width;
  cap_fmt.fmt.pix_mp.height = height;
  cap_fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_JPEG;
  cap_fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
  cap_fmt.fmt.pix_mp.num_planes = 1;
  cap_fmt.fmt.pix_mp.plane_fmt[0].sizeimage = (unsigned int)width * height * 2u;

  if (xioctl(enc->fd, VIDIOC_S_FMT, &cap_fmt) != 0) {
    perror("VIDIOC_S_FMT capture");
    close(enc->fd);
    enc->fd = -1;
    return -1;
  }
  if (xioctl(enc->fd, VIDIOC_G_FMT, &cap_fmt) == 0) {
    dump_pix_mp("v4l2 capture", &cap_fmt);
  }

  enc->cap_format = cap_fmt.fmt.pix_mp.pixelformat;
  enc->cap_num_planes = cap_fmt.fmt.pix_mp.num_planes;
  for (unsigned int i = 0; i < enc->cap_num_planes; ++i) {
    enc->cap_plane_size[i] = cap_fmt.fmt.pix_mp.plane_fmt[i].sizeimage;
  }

  struct v4l2_control ctrl;
  memset(&ctrl, 0, sizeof(ctrl));
  ctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
  ctrl.value = quality;
  if (xioctl(enc->fd, VIDIOC_S_CTRL, &ctrl) != 0) {
    fprintf(stderr, "Warning: JPEG quality control not supported\n");
  }

  enc->out_memory = V4L2_MEMORY_MMAP;
  for (unsigned int i = 0; i < 3; ++i) {
    enc->out_dmabuf_fd[i] = -1;
  }
  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof(req));
  req.count = 1;
  req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(enc->fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 1) {
    perror("VIDIOC_REQBUFS output");
    close(enc->fd);
    enc->fd = -1;
    return -1;
  }

  struct v4l2_buffer out_buf;
  struct v4l2_plane out_plane[3];
  memset(&out_buf, 0, sizeof(out_buf));
  memset(&out_plane, 0, sizeof(out_plane));
  out_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  out_buf.memory = V4L2_MEMORY_MMAP;
  out_buf.index = 0;
  out_buf.length = enc->out_num_planes;
  out_buf.m.planes = out_plane;

  if (xioctl(enc->fd, VIDIOC_QUERYBUF, &out_buf) != 0) {
    if (errno != EINVAL) {
      perror("VIDIOC_QUERYBUF output");
      close(enc->fd);
      enc->fd = -1;
      return -1;
    }
    fprintf(stderr,
            "MMAP output not supported; falling back to USERPTR buffers\n");
    struct v4l2_requestbuffers zero;
    memset(&zero, 0, sizeof(zero));
    zero.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    zero.memory = V4L2_MEMORY_MMAP;
    xioctl(enc->fd, VIDIOC_REQBUFS, &zero);

    enc->out_memory = V4L2_MEMORY_USERPTR;
    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_USERPTR;
    if (xioctl(enc->fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 1) {
      perror("VIDIOC_REQBUFS output USERPTR");
      fprintf(stderr, "Trying DMABUF output buffers...\n");
      enc->out_memory = V4L2_MEMORY_DMABUF;

      /* For DMABUF, prefer single-plane formats (YUYV works, UYVY doesn't)
       * to avoid multi-plane DMABUF complexity. The hantro encoder's NM12/YM12
       * are "non-contiguous" and expect separate DMABUFs per plane. */
      if (enc->out_num_planes > 1) {
        fprintf(stderr, "Re-negotiating to single-plane format for DMABUF...\n");
        if (set_output_format(enc, width, height, V4L2_PIX_FMT_YUYV) != 0) {
          fprintf(stderr, "Failed to set single-plane format\n");
        }
      }

      memset(&req, 0, sizeof(req));
      req.count = 1;
      req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
      req.memory = V4L2_MEMORY_DMABUF;
      if (xioctl(enc->fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 1) {
        perror("VIDIOC_REQBUFS output DMABUF");
        close(enc->fd);
        enc->fd = -1;
        return -1;
      }
      if (allocate_output_dmabuf(enc) != 0) {
        fprintf(stderr, "DMABUF allocation failed\n");
        v4l2_jpeg_destroy(enc);
        return -1;
      }
    } else {
      if (allocate_output_userptr(enc) != 0) {
        v4l2_jpeg_destroy(enc);
        return -1;
      }
    }
  } else {
    for (unsigned int i = 0; i < enc->out_num_planes; ++i) {
      enc->out_map_size[i] = out_buf.m.planes[i].length;
      enc->out_map[i] =
          mmap(NULL, enc->out_map_size[i], PROT_READ | PROT_WRITE, MAP_SHARED,
               enc->fd, out_buf.m.planes[i].m.mem_offset);
      if (enc->out_map[i] == MAP_FAILED) {
        perror("mmap output");
        v4l2_jpeg_destroy(enc);
        return -1;
      }
    }
  }

  memset(&req, 0, sizeof(req));
  req.count = 1;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(enc->fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 1) {
    perror("VIDIOC_REQBUFS capture");
    v4l2_jpeg_destroy(enc);
    return -1;
  }

  struct v4l2_buffer cap_buf;
  struct v4l2_plane cap_plane[3];
  memset(&cap_buf, 0, sizeof(cap_buf));
  memset(&cap_plane, 0, sizeof(cap_plane));
  cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  cap_buf.memory = V4L2_MEMORY_MMAP;
  cap_buf.index = 0;
  cap_buf.length = enc->cap_num_planes;
  cap_buf.m.planes = cap_plane;

  if (xioctl(enc->fd, VIDIOC_QUERYBUF, &cap_buf) != 0) {
    perror("VIDIOC_QUERYBUF capture");
    v4l2_jpeg_destroy(enc);
    return -1;
  }

  for (unsigned int i = 0; i < enc->cap_num_planes; ++i) {
    enc->cap_map_size[i] = cap_buf.m.planes[i].length;
    enc->cap_map[i] =
        mmap(NULL, enc->cap_map_size[i], PROT_READ | PROT_WRITE, MAP_SHARED,
             enc->fd, cap_buf.m.planes[i].m.mem_offset);
    if (enc->cap_map[i] == MAP_FAILED) {
      perror("mmap capture");
      v4l2_jpeg_destroy(enc);
      return -1;
    }
  }

  if (queue_capture(enc) != 0) {
    v4l2_jpeg_destroy(enc);
    return -1;
  }

  enum v4l2_buf_type out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  enum v4l2_buf_type cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (xioctl(enc->fd, VIDIOC_STREAMON, &out_type) != 0) {
    perror("VIDIOC_STREAMON output");
    v4l2_jpeg_destroy(enc);
    return -1;
  }
  if (xioctl(enc->fd, VIDIOC_STREAMON, &cap_type) != 0) {
    perror("VIDIOC_STREAMON capture");
    v4l2_jpeg_destroy(enc);
    return -1;
  }

  return 0;
}

int v4l2_jpeg_init_nv12(struct v4l2_jpeg_encoder *enc, int width, int height,
                        int quality) {
  memset(enc, 0, sizeof(*enc));
  enc->fd = find_jpeg_encoder();
  if (enc->fd < 0) {
    return -1;
  }

  enc->width = width;
  enc->height = height;
  enc->quality = quality;

  /* Try NV12M (multi-plane semi-planar) - this is what RGA outputs */
  if (set_output_format(enc, width, height, V4L2_PIX_FMT_NV12M) != 0) {
    /* NV12M failed, try single-plane NV12 */
    if (set_output_format(enc, width, height, V4L2_PIX_FMT_NV12) != 0) {
      fprintf(stderr, "JPEG encoder does not support NV12/NV12M\n");
      close(enc->fd);
      enc->fd = -1;
      return -1;
    }
  }

  fprintf(stderr, "JPEG encoder configured for %s input (%d planes)\n",
          fourcc_to_str(enc->out_format), enc->out_num_planes);

  /* Set JPEG output format */
  struct v4l2_format cap_fmt;
  memset(&cap_fmt, 0, sizeof(cap_fmt));
  cap_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  cap_fmt.fmt.pix_mp.width = (unsigned int)width;
  cap_fmt.fmt.pix_mp.height = (unsigned int)height;
  cap_fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_JPEG;
  cap_fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
  cap_fmt.fmt.pix_mp.num_planes = 1;
  cap_fmt.fmt.pix_mp.plane_fmt[0].sizeimage = (unsigned int)width * (unsigned int)height * 2u;

  if (xioctl(enc->fd, VIDIOC_S_FMT, &cap_fmt) != 0) {
    perror("VIDIOC_S_FMT capture");
    close(enc->fd);
    enc->fd = -1;
    return -1;
  }
  if (xioctl(enc->fd, VIDIOC_G_FMT, &cap_fmt) == 0) {
    dump_pix_mp("v4l2 capture (NV12 init)", &cap_fmt);
  }

  enc->cap_format = cap_fmt.fmt.pix_mp.pixelformat;
  enc->cap_num_planes = cap_fmt.fmt.pix_mp.num_planes;
  for (unsigned int i = 0; i < enc->cap_num_planes; ++i) {
    enc->cap_plane_size[i] = cap_fmt.fmt.pix_mp.plane_fmt[i].sizeimage;
  }

  /* Set quality */
  struct v4l2_control ctrl;
  memset(&ctrl, 0, sizeof(ctrl));
  ctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
  ctrl.value = quality;
  if (xioctl(enc->fd, VIDIOC_S_CTRL, &ctrl) != 0) {
    fprintf(stderr, "Warning: JPEG quality control not supported\n");
  }

  /* Try MMAP first, fall back to USERPTR */
  for (unsigned int i = 0; i < 3; ++i) {
    enc->out_dmabuf_fd[i] = -1;
  }

  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof(req));
  req.count = 1;
  req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  req.memory = V4L2_MEMORY_MMAP;

  int use_mmap = 1;
  if (xioctl(enc->fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 1) {
    use_mmap = 0;
  } else {
    struct v4l2_buffer out_buf;
    struct v4l2_plane out_plane[3];
    memset(&out_buf, 0, sizeof(out_buf));
    memset(&out_plane, 0, sizeof(out_plane));
    out_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    out_buf.memory = V4L2_MEMORY_MMAP;
    out_buf.index = 0;
    out_buf.length = enc->out_num_planes;
    out_buf.m.planes = out_plane;

    if (xioctl(enc->fd, VIDIOC_QUERYBUF, &out_buf) != 0) {
      use_mmap = 0;
      /* Release MMAP request */
      struct v4l2_requestbuffers zero;
      memset(&zero, 0, sizeof(zero));
      zero.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
      zero.memory = V4L2_MEMORY_MMAP;
      xioctl(enc->fd, VIDIOC_REQBUFS, &zero);
    } else {
      enc->out_memory = V4L2_MEMORY_MMAP;
      for (unsigned int i = 0; i < enc->out_num_planes; ++i) {
        enc->out_map_size[i] = out_buf.m.planes[i].length;
        enc->out_map[i] =
            mmap(NULL, enc->out_map_size[i], PROT_READ | PROT_WRITE, MAP_SHARED,
                 enc->fd, out_buf.m.planes[i].m.mem_offset);
        if (enc->out_map[i] == MAP_FAILED) {
          perror("mmap output (NV12)");
          v4l2_jpeg_destroy(enc);
          return -1;
        }
      }
    }
  }

  if (!use_mmap) {
    /* Try USERPTR first */
    enc->out_memory = V4L2_MEMORY_USERPTR;
    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_USERPTR;
    if (xioctl(enc->fd, VIDIOC_REQBUFS, &req) == 0 && req.count >= 1) {
      fprintf(stderr, "Using USERPTR for NV12M encoder input\n");
      if (allocate_output_userptr(enc) != 0) {
        v4l2_jpeg_destroy(enc);
        return -1;
      }
    } else {
      /* Fall back to DMABUF */
      fprintf(stderr, "Using DMABUF for NV12M encoder input\n");
      enc->out_memory = V4L2_MEMORY_DMABUF;
      memset(&req, 0, sizeof(req));
      req.count = 1;
      req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
      req.memory = V4L2_MEMORY_DMABUF;
      if (xioctl(enc->fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 1) {
        perror("VIDIOC_REQBUFS output DMABUF (NV12)");
        close(enc->fd);
        enc->fd = -1;
        return -1;
      }
      if (allocate_output_dmabuf(enc) != 0) {
        fprintf(stderr, "DMABUF allocation failed for NV12M\n");
        v4l2_jpeg_destroy(enc);
        return -1;
      }
    }
  }

  /* Capture buffer setup */
  memset(&req, 0, sizeof(req));
  req.count = 1;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(enc->fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 1) {
    perror("VIDIOC_REQBUFS capture (NV12)");
    v4l2_jpeg_destroy(enc);
    return -1;
  }

  struct v4l2_buffer cap_buf;
  struct v4l2_plane cap_plane[3];
  memset(&cap_buf, 0, sizeof(cap_buf));
  memset(&cap_plane, 0, sizeof(cap_plane));
  cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  cap_buf.memory = V4L2_MEMORY_MMAP;
  cap_buf.index = 0;
  cap_buf.length = enc->cap_num_planes;
  cap_buf.m.planes = cap_plane;

  if (xioctl(enc->fd, VIDIOC_QUERYBUF, &cap_buf) != 0) {
    perror("VIDIOC_QUERYBUF capture (NV12)");
    v4l2_jpeg_destroy(enc);
    return -1;
  }

  for (unsigned int i = 0; i < enc->cap_num_planes; ++i) {
    enc->cap_map_size[i] = cap_buf.m.planes[i].length;
    enc->cap_map[i] =
        mmap(NULL, enc->cap_map_size[i], PROT_READ | PROT_WRITE, MAP_SHARED,
             enc->fd, cap_buf.m.planes[i].m.mem_offset);
    if (enc->cap_map[i] == MAP_FAILED) {
      perror("mmap capture (NV12)");
      v4l2_jpeg_destroy(enc);
      return -1;
    }
  }

  /* Queue capture buffer and start streaming */
  if (queue_capture(enc) != 0) {
    v4l2_jpeg_destroy(enc);
    return -1;
  }

  enum v4l2_buf_type out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  enum v4l2_buf_type cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (xioctl(enc->fd, VIDIOC_STREAMON, &out_type) != 0) {
    perror("VIDIOC_STREAMON output (NV12)");
    v4l2_jpeg_destroy(enc);
    return -1;
  }
  if (xioctl(enc->fd, VIDIOC_STREAMON, &cap_type) != 0) {
    perror("VIDIOC_STREAMON capture (NV12)");
    v4l2_jpeg_destroy(enc);
    return -1;
  }

  return 0;
}

int v4l2_jpeg_encode_frame(struct v4l2_jpeg_encoder *enc,
                           const struct capture_frame *frame,
                           unsigned char **out_buf,
                           unsigned long *out_size) {
  if (!enc || enc->fd < 0) {
    return -1;
  }
  if (frame->width != (uint32_t)enc->width ||
      frame->height != (uint32_t)enc->height) {
    fprintf(stderr, "Frame size changed; reinit required\n");
    return -1;
  }

  void *out_plane0 = enc->out_memory == V4L2_MEMORY_USERPTR
                         ? enc->out_userptr[0]
                         : enc->out_map[0];
  void *out_plane1 = enc->out_memory == V4L2_MEMORY_USERPTR
                         ? enc->out_userptr[1]
                         : enc->out_map[1];
  void *out_plane2 = enc->out_memory == V4L2_MEMORY_USERPTR
                         ? enc->out_userptr[2]
                         : enc->out_map[2];

  if (enc->out_format == V4L2_PIX_FMT_YUYV) {
    unsigned int stride =
        max_u32(enc->out_bytesperline[0], (unsigned int)enc->width * 2u);
    /* Check if input is already YUYV (e.g., from OpenCL conversion) */
    if (frame->format == V4L2_PIX_FMT_YUYV) {
      /* Direct copy - input is already YUYV */
      for (uint32_t row = 0; row < frame->height; ++row) {
        const uint8_t *src_row = (const uint8_t *)frame->data + row * frame->stride;
        uint8_t *dst_row = (uint8_t *)out_plane0 + row * stride;
        memcpy(dst_row, src_row, frame->width * 2u);
      }
    } else if (convert_to_yuyv(frame, out_plane0, stride) != 0) {
      return -1;
    }
  } else if (enc->out_format == V4L2_PIX_FMT_UYVY) {
    unsigned int stride =
        max_u32(enc->out_bytesperline[0], (unsigned int)enc->width * 2u);
    if (convert_to_uyvy(frame, out_plane0, stride) != 0) {
      return -1;
    }
  } else if (enc->out_format == V4L2_PIX_FMT_NV12M ||
             enc->out_format == V4L2_PIX_FMT_NV12) {
    if (enc->out_num_planes < 2) {
      fprintf(stderr, "NV12 requires two planes\n");
      return -1;
    }
    if (convert_to_nv12(frame, out_plane0, enc->out_bytesperline[0],
                        out_plane1, enc->out_bytesperline[1]) != 0) {
      return -1;
    }
  } else if (enc->out_format == V4L2_PIX_FMT_YUV420M) {
    if (enc->out_num_planes < 3) {
      fprintf(stderr, "YUV420 requires three planes\n");
      return -1;
    }
    if (convert_to_yuv420p(frame, out_plane0, enc->out_bytesperline[0],
                           out_plane1, enc->out_bytesperline[1], out_plane2,
                           enc->out_bytesperline[2]) != 0) {
      return -1;
    }
  } else {
    fprintf(stderr, "Unsupported V4L2 output format: %s\n",
            fourcc_to_str(enc->out_format));
    return -1;
  }

  if (!enc->cap_queued) {
    if (queue_capture(enc) != 0) {
      return -1;
    }
  }

  struct v4l2_buffer out_buf_desc;
  struct v4l2_plane out_plane[3];
  memset(&out_buf_desc, 0, sizeof(out_buf_desc));
  memset(&out_plane, 0, sizeof(out_plane));
  out_buf_desc.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  out_buf_desc.memory = enc->out_memory;
  out_buf_desc.index = 0;
  out_buf_desc.length = enc->out_num_planes;
  out_buf_desc.m.planes = out_plane;
  for (unsigned int i = 0; i < enc->out_num_planes; ++i) {
    unsigned int used = bytes_used_for_plane(enc, i);
    if (enc->out_plane_size[i] > 0 && used > enc->out_plane_size[i]) {
      used = enc->out_plane_size[i];
    }
    out_plane[i].bytesused = used;
    if (enc->out_memory == V4L2_MEMORY_USERPTR) {
      out_plane[i].m.userptr = (unsigned long)enc->out_userptr[i];
      out_plane[i].length = enc->out_plane_size[i];
    } else if (enc->out_memory == V4L2_MEMORY_DMABUF) {
      out_plane[i].m.fd = enc->out_dmabuf_fd[i];
      out_plane[i].length = enc->out_plane_size[i] > 0
                                 ? enc->out_plane_size[i]
                                 : enc->out_map_size[i];
      out_plane[i].data_offset = enc->out_dmabuf_offset[i];
    }
  }

  dump_qbuf_planes(enc, out_plane);
  if (xioctl(enc->fd, VIDIOC_QBUF, &out_buf_desc) != 0) {
    perror("VIDIOC_QBUF output");
    return -1;
  }

  struct pollfd pfd;
  memset(&pfd, 0, sizeof(pfd));
  pfd.fd = enc->fd;
  pfd.events = POLLIN;

  int poll_rc = poll(&pfd, 1, 2000);
  if (poll_rc <= 0) {
    fprintf(stderr, "poll timeout or error\n");
    return -1;
  }

  struct v4l2_buffer cap_buf_desc;
  struct v4l2_plane cap_plane[3];
  memset(&cap_buf_desc, 0, sizeof(cap_buf_desc));
  memset(&cap_plane, 0, sizeof(cap_plane));
  cap_buf_desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  cap_buf_desc.memory = V4L2_MEMORY_MMAP;
  cap_buf_desc.index = 0;
  cap_buf_desc.length = enc->cap_num_planes;
  cap_buf_desc.m.planes = cap_plane;

  if (xioctl(enc->fd, VIDIOC_DQBUF, &cap_buf_desc) != 0) {
    perror("VIDIOC_DQBUF capture");
    return -1;
  }
  enc->cap_queued = 0;

  struct v4l2_buffer out_done;
  struct v4l2_plane out_done_plane[3];
  memset(&out_done, 0, sizeof(out_done));
  memset(&out_done_plane, 0, sizeof(out_done_plane));
  out_done.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  out_done.memory = enc->out_memory;
  out_done.index = 0;
  out_done.length = enc->out_num_planes;
  out_done.m.planes = out_done_plane;
  if (xioctl(enc->fd, VIDIOC_DQBUF, &out_done) != 0) {
    perror("VIDIOC_DQBUF output");
    return -1;
  }

  *out_buf = (unsigned char *)enc->cap_map[0];
  *out_size = cap_buf_desc.m.planes[0].bytesused;
  return 0;
}

int v4l2_jpeg_encode_nv12(struct v4l2_jpeg_encoder *enc,
                          const void *y_plane, unsigned int y_stride,
                          const void *uv_plane, unsigned int uv_stride,
                          unsigned char **out_buf, unsigned long *out_size) {
  if (!enc || enc->fd < 0) {
    return -1;
  }

  /* Verify encoder is configured for NV12/YUV420 */
  if (enc->out_format != V4L2_PIX_FMT_NV12M &&
      enc->out_format != V4L2_PIX_FMT_NV12 &&
      enc->out_format != V4L2_PIX_FMT_YUV420M) {
    fprintf(stderr, "Encoder not configured for NV12/YUV420 (format=%s)\n",
            fourcc_to_str(enc->out_format));
    return -1;
  }

  void *out_plane0 = enc->out_memory == V4L2_MEMORY_USERPTR
                         ? enc->out_userptr[0]
                         : enc->out_map[0];
  void *out_plane1 = enc->out_memory == V4L2_MEMORY_USERPTR
                         ? enc->out_userptr[1]
                         : enc->out_map[1];

  /* Copy Y plane */
  unsigned int enc_y_stride = enc->out_bytesperline[0];
  for (int row = 0; row < enc->height; ++row) {
    memcpy((uint8_t *)out_plane0 + row * enc_y_stride,
           (const uint8_t *)y_plane + row * y_stride,
           (unsigned int)enc->width);
  }

  /* Copy UV plane */
  unsigned int enc_uv_stride = enc->out_bytesperline[1];
  for (int row = 0; row < enc->height / 2; ++row) {
    memcpy((uint8_t *)out_plane1 + row * enc_uv_stride,
           (const uint8_t *)uv_plane + row * uv_stride,
           (unsigned int)enc->width);
  }

  if (!enc->cap_queued) {
    if (queue_capture(enc) != 0) {
      return -1;
    }
  }

  struct v4l2_buffer out_buf_desc;
  struct v4l2_plane out_plane[3];
  memset(&out_buf_desc, 0, sizeof(out_buf_desc));
  memset(&out_plane, 0, sizeof(out_plane));
  out_buf_desc.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  out_buf_desc.memory = enc->out_memory;
  out_buf_desc.index = 0;
  out_buf_desc.length = enc->out_num_planes;
  out_buf_desc.m.planes = out_plane;

  for (unsigned int i = 0; i < enc->out_num_planes; ++i) {
    unsigned int used = bytes_used_for_plane(enc, i);
    if (enc->out_plane_size[i] > 0 && used > enc->out_plane_size[i]) {
      used = enc->out_plane_size[i];
    }
    out_plane[i].bytesused = used;
    if (enc->out_memory == V4L2_MEMORY_USERPTR) {
      out_plane[i].m.userptr = (unsigned long)enc->out_userptr[i];
      out_plane[i].length = enc->out_plane_size[i];
    } else if (enc->out_memory == V4L2_MEMORY_DMABUF) {
      out_plane[i].m.fd = enc->out_dmabuf_fd[i];
      out_plane[i].length = enc->out_plane_size[i] > 0
                                 ? enc->out_plane_size[i]
                                 : enc->out_map_size[i];
      out_plane[i].data_offset = enc->out_dmabuf_offset[i];
    }
  }

  dump_qbuf_planes(enc, out_plane);

  if (xioctl(enc->fd, VIDIOC_QBUF, &out_buf_desc) != 0) {
    perror("VIDIOC_QBUF output (NV12)");
    return -1;
  }

  struct pollfd pfd;
  memset(&pfd, 0, sizeof(pfd));
  pfd.fd = enc->fd;
  pfd.events = POLLIN;

  int poll_rc = poll(&pfd, 1, 2000);
  if (poll_rc <= 0) {
    fprintf(stderr, "poll timeout or error (NV12)\n");
    return -1;
  }

  struct v4l2_buffer cap_buf_desc;
  struct v4l2_plane cap_plane[3];
  memset(&cap_buf_desc, 0, sizeof(cap_buf_desc));
  memset(&cap_plane, 0, sizeof(cap_plane));
  cap_buf_desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  cap_buf_desc.memory = V4L2_MEMORY_MMAP;
  cap_buf_desc.index = 0;
  cap_buf_desc.length = enc->cap_num_planes;
  cap_buf_desc.m.planes = cap_plane;

  if (xioctl(enc->fd, VIDIOC_DQBUF, &cap_buf_desc) != 0) {
    perror("VIDIOC_DQBUF capture (NV12)");
    return -1;
  }
  enc->cap_queued = 0;

  struct v4l2_buffer out_done;
  struct v4l2_plane out_done_plane[3];
  memset(&out_done, 0, sizeof(out_done));
  memset(&out_done_plane, 0, sizeof(out_done_plane));
  out_done.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  out_done.memory = enc->out_memory;
  out_done.index = 0;
  out_done.length = enc->out_num_planes;
  out_done.m.planes = out_done_plane;
  if (xioctl(enc->fd, VIDIOC_DQBUF, &out_done) != 0) {
    perror("VIDIOC_DQBUF output (NV12)");
    return -1;
  }

  *out_buf = (unsigned char *)enc->cap_map[0];
  *out_size = cap_buf_desc.m.planes[0].bytesused;
  return 0;
}

void v4l2_jpeg_destroy(struct v4l2_jpeg_encoder *enc) {
  if (!enc) {
    return;
  }

  if (enc->fd >= 0) {
    enum v4l2_buf_type out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    enum v4l2_buf_type cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(enc->fd, VIDIOC_STREAMOFF, &out_type);
    xioctl(enc->fd, VIDIOC_STREAMOFF, &cap_type);
  }

  release_output_buffers(enc);
  for (unsigned int i = 0; i < enc->cap_num_planes; ++i) {
    if (enc->cap_map[i] && enc->cap_map[i] != MAP_FAILED) {
      munmap(enc->cap_map[i], enc->cap_map_size[i]);
    }
  }
  if (enc->fd >= 0) {
    close(enc->fd);
  }

  memset(enc, 0, sizeof(*enc));
  enc->fd = -1;
}

int v4l2_jpeg_set_quality(struct v4l2_jpeg_encoder *enc, int quality) {
  if (!enc || enc->fd < 0) {
    return -1;
  }

  struct v4l2_control ctrl;
  memset(&ctrl, 0, sizeof(ctrl));
  ctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
  ctrl.value = quality;
  if (xioctl(enc->fd, VIDIOC_S_CTRL, &ctrl) < 0) {
    /* Quality control might not be supported, but that's okay */
    return -1;
  }

  enc->quality = quality;
  return 0;
}
