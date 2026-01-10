#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "capture.h"
#include "capture_dmabuf.h"
#include "compress.h"
#include "v4l2_common.h"
#include "v4l2_jpeg.h"
#include "v4l2_rga.h"
#include "udp.h"

#ifdef HAVE_OPENCL
#include "opencl_convert.h"
#endif

static volatile sig_atomic_t g_running = 1;

static void handle_sigint(int sig) {
  (void)sig;
  g_running = 0;
}

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sleep_ms(uint64_t ms) {
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000u);
  ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
  }
}

static void print_usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s --dest <ip> [--port <port>] [--quality <1-100>] "
          "[--fps <limit>] [--region x y w h] [--hw-jpeg] [--dmabuf] [--rga] [--opencl] [--no-cursor]\n"
          "  --dmabuf    Use wlr-export-dmabuf (zero-copy capture, reduces compositor load)\n"
          "  --rga       Use RGA for hardware color conversion (requires --dmabuf --hw-jpeg)\n"
#ifdef HAVE_OPENCL
          "  --opencl    Use OpenCL for GPU color conversion (requires --dmabuf --hw-jpeg, libmali)\n"
#endif
          ,
          prog);
}

int main(int argc, char **argv) {
  const char *dest_ip = NULL;
  uint16_t port = 7723;
  int quality = 80;
  int fps_limit = 0;
  int overlay_cursor = 1;
  int region_x = 0;
  int region_y = 0;
  int region_w = 0;
  int region_h = 0;
  int use_hw_jpeg = 0;
  int use_dmabuf = 0;
  int use_rga = 0;
  int use_opencl = 0;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--dest") == 0 && i + 1 < argc) {
      dest_ip = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--quality") == 0 && i + 1 < argc) {
      quality = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
      fps_limit = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--region") == 0 && i + 4 < argc) {
      region_x = atoi(argv[++i]);
      region_y = atoi(argv[++i]);
      region_w = atoi(argv[++i]);
      region_h = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--hw-jpeg") == 0) {
      use_hw_jpeg = 1;
    } else if (strcmp(argv[i], "--dmabuf") == 0) {
      use_dmabuf = 1;
    } else if (strcmp(argv[i], "--rga") == 0) {
      use_rga = 1;
    } else if (strcmp(argv[i], "--opencl") == 0) {
#ifdef HAVE_OPENCL
      use_opencl = 1;
#else
      fprintf(stderr, "OpenCL support not compiled in (rebuild with OPENCL=1)\n");
      return 1;
#endif
    } else if (strcmp(argv[i], "--no-cursor") == 0) {
      overlay_cursor = 0;
    } else if (strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      return 0;
    } else {
      print_usage(argv[0]);
      return 1;
    }
  }

  if (!dest_ip) {
    print_usage(argv[0]);
    return 1;
  }

  if (quality < 1) {
    quality = 1;
  } else if (quality > 100) {
    quality = 100;
  }

  /* RGA requires both dmabuf and hw-jpeg */
  if (use_rga) {
    if (!use_dmabuf) {
      fprintf(stderr, "--rga requires --dmabuf\n");
      use_rga = 0;
    }
    if (!use_hw_jpeg) {
      fprintf(stderr, "--rga requires --hw-jpeg, enabling it\n");
      use_hw_jpeg = 1;
    }
  }

  /* OpenCL requires dmabuf and hw-jpeg */
  if (use_opencl) {
    if (!use_dmabuf) {
      fprintf(stderr, "--opencl requires --dmabuf, enabling it\n");
      use_dmabuf = 1;
    }
    if (!use_hw_jpeg) {
      fprintf(stderr, "--opencl requires --hw-jpeg, enabling it\n");
      use_hw_jpeg = 1;
    }
    if (use_rga) {
      fprintf(stderr, "--opencl and --rga are mutually exclusive, using --opencl\n");
      use_rga = 0;
    }
  }

  signal(SIGINT, handle_sigint);
  signal(SIGTERM, handle_sigint);

  struct capture_context *capture = NULL;
  struct dmabuf_capture_context *dmabuf_capture = NULL;

  if (use_dmabuf) {
    if (dmabuf_capture_init(&dmabuf_capture, overlay_cursor) != 0) {
      fprintf(stderr, "Failed to initialize dmabuf capture, falling back to screencopy\n");
      use_dmabuf = 0;
    } else {
      fprintf(stderr, "Using wlr-export-dmabuf for capture\n");
    }
  }

  if (!use_dmabuf) {
    if (capture_init(&capture, overlay_cursor) != 0) {
      fprintf(stderr, "Failed to initialize capture\n");
      return 1;
    }
    if (region_w > 0 && region_h > 0) {
      capture_set_region(capture, region_x, region_y, region_w, region_h);
    }
  }

  struct udp_sender sender;
  if (udp_sender_init(&sender, dest_ip, port) != 0) {
    fprintf(stderr, "Failed to initialize UDP sender\n");
    if (use_dmabuf) {
      dmabuf_capture_shutdown(dmabuf_capture);
    } else {
      capture_shutdown(capture);
    }
    return 1;
  }

  struct jpeg_encoder encoder;
  int sw_encoder_ready = 0;
  struct v4l2_jpeg_encoder hw_encoder;
  int hw_encoder_ready = 0;
  struct v4l2_rga_converter rga_converter;
  int rga_ready = 0;
#ifdef HAVE_OPENCL
  struct opencl_converter *opencl_conv = NULL;
#endif

  if (!use_hw_jpeg) {
    if (jpeg_encoder_init(&encoder, quality) != 0) {
      fprintf(stderr, "Failed to initialize JPEG encoder\n");
      udp_sender_close(&sender);
      capture_shutdown(capture);
      return 1;
    }
    sw_encoder_ready = 1;
  }

  uint64_t frame_interval_ms = 0;
  if (fps_limit > 0) {
    frame_interval_ms = 1000u / (uint64_t)fps_limit;
  }

  uint64_t last_fps_ts = now_ms();
  unsigned int frame_counter = 0;

  while (g_running) {
    uint64_t frame_start = now_ms();
    struct capture_frame frame;
    struct dmabuf_frame dma_frame;
    int capture_ok = 0;

    if (use_dmabuf) {
      if (dmabuf_capture_next_frame(dmabuf_capture, &dma_frame) == 0) {
        if (use_rga) {
          /* RGA path: map the dmabuf for potential USERPTR fallback */
          if (dmabuf_frame_map(&dma_frame) == 0) {
            capture_ok = 1;
          } else {
            fprintf(stderr, "dmabuf map failed for RGA\n");
            dmabuf_frame_release(&dma_frame);
          }
#ifdef HAVE_OPENCL
        } else if (use_opencl) {
          /* OpenCL path: only need the dmabuf FD, skip CPU mapping */
          capture_ok = 1;
#endif
        } else if (dmabuf_frame_map(&dma_frame) == 0) {
          frame.format = dma_frame.format;
          frame.width = dma_frame.width;
          frame.height = dma_frame.height;
          frame.stride = dma_frame.objects[0].stride;
          frame.data = (char *)dma_frame.mapped_data + dma_frame.objects[0].offset;
          frame.y_invert = 0;
          capture_ok = 1;
        } else {
          fprintf(stderr, "dmabuf map failed\n");
          dmabuf_frame_release(&dma_frame);
        }
      } else {
        fprintf(stderr, "dmabuf capture failed\n");
      }
    } else {
      if (capture_next_frame(capture, &frame) == 0) {
        capture_ok = 1;
      } else {
        fprintf(stderr, "Capture failed\n");
        break;
      }
    }

    if (!capture_ok) {
      continue;
    }

    unsigned char *jpeg_data = NULL;
    unsigned long jpeg_size = 0;

#ifdef HAVE_OPENCL
    if (use_opencl) {
      /* OpenCL zero-copy path: dmabuf → GPU kernel → YUYV dmabuf → JPEG encoder */
      int w = (int)dma_frame.width;
      int h = (int)dma_frame.height;

      /* Initialize OpenCL converter on first frame */
      if (!opencl_conv) {
        opencl_conv = opencl_convert_init(w, h);
        if (!opencl_conv) {
          fprintf(stderr, "Failed to initialize OpenCL converter\n");
          dmabuf_frame_release(&dma_frame);
          break;
        }
      }

      /* Initialize HW JPEG encoder on first frame (YUYV format) */
      if (!hw_encoder_ready) {
        if (v4l2_jpeg_init(&hw_encoder, w, h, quality) != 0) {
          fprintf(stderr, "Failed to initialize HW JPEG encoder for OpenCL\n");
          dmabuf_frame_release(&dma_frame);
          break;
        }
        hw_encoder_ready = 1;
      }

      /* Convert XRGB → YUYV using OpenCL (zero-copy dmabuf import) */
      int output_fd;
      size_t output_size;
      size_t input_size = (size_t)w * h * 4;
      if (opencl_convert(opencl_conv, dma_frame.objects[0].fd, input_size,
                         &output_fd, &output_size) != 0) {
        fprintf(stderr, "OpenCL conversion failed\n");
        dmabuf_frame_release(&dma_frame);
        continue;
      }

      /* Get mapped YUYV data for JPEG encoder */
      void *yuyv_data;
      opencl_convert_get_output(opencl_conv, NULL, &yuyv_data, NULL);

      /* Create a frame struct for the JPEG encoder */
      struct capture_frame yuyv_frame;
      yuyv_frame.format = FOURCC_YUYV;
      yuyv_frame.width = (uint32_t)w;
      yuyv_frame.height = (uint32_t)h;
      yuyv_frame.stride = (uint32_t)(w * 2);
      yuyv_frame.data = yuyv_data;
      yuyv_frame.y_invert = 0;

      /* Encode YUYV to JPEG */
      if (v4l2_jpeg_encode_frame(&hw_encoder, &yuyv_frame, &jpeg_data,
                                  &jpeg_size) != 0) {
        fprintf(stderr, "HW JPEG encode (OpenCL) failed\n");
        dmabuf_frame_release(&dma_frame);
        continue;
      }
    } else
#endif
    if (use_rga) {
      /* RGA + HW JPEG path */
      int w = (int)dma_frame.width;
      int h = (int)dma_frame.height;

      /* Initialize RGA and JPEG encoder on first frame */
      if (!rga_ready) {
        if (v4l2_rga_init(&rga_converter, w, h) != 0) {
          fprintf(stderr, "Failed to initialize RGA\n");
          dmabuf_frame_release(&dma_frame);
          break;
        }
        rga_ready = 1;
        fprintf(stderr, "RGA initialized for %dx%d\n", w, h);
      }
      if (!hw_encoder_ready) {
        /* Use NV12-specific init for RGA path */
        if (v4l2_jpeg_init_nv12(&hw_encoder, w, h, quality) != 0) {
          fprintf(stderr, "Failed to initialize HW JPEG encoder for NV12\n");
          dmabuf_frame_release(&dma_frame);
          break;
        }
        hw_encoder_ready = 1;
      }

      /* Convert XRGB8888 -> NV12 using RGA */
      void *y_plane = NULL, *uv_plane = NULL;
      unsigned int y_stride = 0, uv_stride = 0;
      void *mapped_ptr = (char *)dma_frame.mapped_data + dma_frame.objects[0].offset;
      if (v4l2_rga_convert_dmabuf(&rga_converter, dma_frame.objects[0].fd,
                                   mapped_ptr,
                                   &y_plane, &y_stride,
                                   &uv_plane, &uv_stride) != 0) {
        fprintf(stderr, "RGA conversion failed\n");
        dmabuf_frame_release(&dma_frame);
        continue;
      }

      /* Encode NV12 to JPEG */
      if (v4l2_jpeg_encode_nv12(&hw_encoder, y_plane, y_stride,
                                 uv_plane, uv_stride,
                                 &jpeg_data, &jpeg_size) != 0) {
        fprintf(stderr, "HW JPEG encode (NV12) failed\n");
        dmabuf_frame_release(&dma_frame);
        continue;
      }
    } else if (use_hw_jpeg) {
      if (!hw_encoder_ready) {
        if (v4l2_jpeg_init(&hw_encoder, (int)frame.width, (int)frame.height,
                           quality) != 0) {
          fprintf(stderr, "Failed to initialize HW JPEG encoder\n");
          if (use_dmabuf) {
            dmabuf_frame_release(&dma_frame);
          }
          break;
        }
        hw_encoder_ready = 1;
      }
      if (v4l2_jpeg_encode_frame(&hw_encoder, &frame, &jpeg_data,
                                &jpeg_size) != 0) {
        fprintf(stderr, "HW JPEG encode failed\n");
        if (use_dmabuf) {
          dmabuf_frame_release(&dma_frame);
        }
        continue;
      }
    } else {
      if (jpeg_encode_frame(&encoder, &frame, &jpeg_data, &jpeg_size) != 0) {
        fprintf(stderr, "JPEG encode failed\n");
        if (use_dmabuf) {
          dmabuf_frame_release(&dma_frame);
        }
        continue;
      }
    }

    /* Release dmabuf after encoding (all error paths above handle their own release) */
    if (use_dmabuf) {
      dmabuf_frame_release(&dma_frame);
    }

    if (udp_sender_send_frame(&sender, jpeg_data, jpeg_size) != 0) {
      fprintf(stderr, "UDP send failed\n");
      break;
    }

    frame_counter++;
    uint64_t now = now_ms();
    if (now - last_fps_ts >= 1000u) {
      fprintf(stderr, "fps=%u\n", frame_counter);
      frame_counter = 0;
      last_fps_ts = now;
    }

    if (frame_interval_ms > 0) {
      uint64_t elapsed = now_ms() - frame_start;
      if (elapsed < frame_interval_ms) {
        sleep_ms(frame_interval_ms - elapsed);
      }
    }
  }

  if (sw_encoder_ready) {
    jpeg_encoder_destroy(&encoder);
  }
  if (hw_encoder_ready) {
    v4l2_jpeg_destroy(&hw_encoder);
  }
  if (rga_ready) {
    v4l2_rga_destroy(&rga_converter);
  }
#ifdef HAVE_OPENCL
  if (opencl_conv) {
    opencl_convert_destroy(opencl_conv);
  }
#endif
  udp_sender_close(&sender);
  if (use_dmabuf) {
    dmabuf_capture_shutdown(dmabuf_capture);
  } else {
    capture_shutdown(capture);
  }

  return 0;
}
