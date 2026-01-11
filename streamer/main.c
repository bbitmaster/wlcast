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

#ifdef HAVE_AUDIO
#include "audio.h"
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
          "[--fps <limit>] [--target-fps <fps>] [--region x y w h] [--hw-jpeg] [--dmabuf] [--rga] [--opencl] [--audio] [--no-cursor]\n"
          "  --target-fps  Adaptive quality: auto-adjust quality to hit target FPS (default: 0=off)\n"
          "  --dmabuf      Use wlr-export-dmabuf (zero-copy capture, reduces compositor load)\n"
          "  --rga         Use RGA for hardware color conversion (requires --dmabuf --hw-jpeg)\n"
#ifdef HAVE_OPENCL
          "  --opencl      Use OpenCL for GPU color conversion (requires --dmabuf --hw-jpeg, libmali)\n"
#endif
#ifdef HAVE_AUDIO
          "  --audio       Enable audio streaming (PulseAudio capture + Opus encoding)\n"
#endif
          ,
          prog);
}

int main(int argc, char **argv) {
  const char *dest_ip = NULL;
  uint16_t port = 7723;
  int quality = 80;
  int fps_limit = 0;
  int target_fps = 0;  /* 0 = adaptive quality disabled */
  int overlay_cursor = 1;
  int region_x = 0;
  int region_y = 0;
  int region_w = 0;
  int region_h = 0;
  int use_hw_jpeg = 0;
  int use_dmabuf = 0;
  int use_rga = 0;
  int use_opencl = 0;
#ifdef HAVE_AUDIO
  int use_audio = 0;
#endif

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--dest") == 0 && i + 1 < argc) {
      dest_ip = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--quality") == 0 && i + 1 < argc) {
      quality = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
      fps_limit = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--target-fps") == 0 && i + 1 < argc) {
      target_fps = atoi(argv[++i]);
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
    } else if (strcmp(argv[i], "--audio") == 0) {
#ifdef HAVE_AUDIO
      use_audio = 1;
#else
      fprintf(stderr, "Audio support not compiled in (rebuild with AUDIO=1)\n");
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
#ifdef HAVE_AUDIO
  struct audio_streamer *audio = NULL;
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

  /* Pipelining state for OpenCL path */
  struct dmabuf_pending_frame *pending_capture = NULL;
  int pipeline_active = 0;

  /* Timing debug (enable with SM_TIMING_DEBUG=1) */
  int timing_debug = (getenv("SM_TIMING_DEBUG") != NULL);
  uint64_t t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0;
  (void)t0; (void)t1; (void)t2; (void)t3; (void)t4; /* Suppress unused warnings */

#ifdef HAVE_AUDIO
  /* Initialize and start audio streaming */
  if (use_audio) {
    if (audio_streamer_init(&audio, dest_ip, port) != 0) {
      fprintf(stderr, "Warning: Failed to initialize audio, continuing without\n");
      use_audio = 0;
    } else if (audio_streamer_start(audio) != 0) {
      fprintf(stderr, "Warning: Failed to start audio, continuing without\n");
      audio_streamer_destroy(audio);
      audio = NULL;
      use_audio = 0;
    }
  }
#endif

  while (g_running) {
    uint64_t frame_start = now_ms();
    struct capture_frame frame;
    struct dmabuf_frame dma_frame;
    int capture_ok = 0;

    if (timing_debug) t0 = now_ms();

    if (use_dmabuf) {
#ifdef HAVE_OPENCL
      if (use_opencl && pipeline_active && pending_capture) {
        /* Pipelined path: wait for previously requested frame */
        if (dmabuf_capture_finish(dmabuf_capture, pending_capture, &dma_frame) == 0) {
          capture_ok = 1;
          if (timing_debug) t1 = now_ms();
          /* Immediately request next frame to maintain pipeline */
          pending_capture = dmabuf_capture_request(dmabuf_capture);
          if (timing_debug) fprintf(stderr, "[PIPE] wait=%lums ", (unsigned long)(t1 - t0));
        } else {
          fprintf(stderr, "dmabuf pipelined capture failed\n");
          pending_capture = NULL;
        }
      } else
#endif
      if (dmabuf_capture_next_frame(dmabuf_capture, &dma_frame) == 0) {
        if (timing_debug) t1 = now_ms();
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
          /* Request next frame NOW so capture overlaps with convert+encode */
          if (pipeline_active) {
            pending_capture = dmabuf_capture_request(dmabuf_capture);
          }
          pipeline_active = 1; /* Enable pipelining after first frame */
          if (timing_debug) fprintf(stderr, "[SYNC] cap=%lums ", (unsigned long)(t1 - t0));
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
      if (timing_debug) t2 = now_ms();
      int output_fd;
      size_t output_size;
      size_t input_size = (size_t)w * h * 4;
      if (opencl_convert(opencl_conv, dma_frame.objects[0].fd, input_size,
                         &output_fd, &output_size) != 0) {
        fprintf(stderr, "OpenCL conversion failed\n");
        dmabuf_frame_release(&dma_frame);
        continue;
      }
      if (timing_debug) t3 = now_ms();

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
      if (timing_debug) {
        t4 = now_ms();
        fprintf(stderr, "ocl=%lums enc=%lums ", (unsigned long)(t3 - t2), (unsigned long)(t4 - t3));
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
    uint64_t t5 = 0, t6 = 0;
    if (timing_debug) t5 = now_ms();
    if (use_dmabuf) {
      dmabuf_frame_release(&dma_frame);
    }
    if (timing_debug) t6 = now_ms();

    if (udp_sender_send_frame(&sender, jpeg_data, jpeg_size) != 0) {
      fprintf(stderr, "UDP send failed\n");
      break;
    }

    /* Poll for ACKs from viewer */
    udp_sender_poll_acks(&sender);

    if (timing_debug) {
      uint64_t t7 = now_ms();
      fprintf(stderr, "rel=%lums udp=%lums ", (unsigned long)(t6 - t5), (unsigned long)(t7 - t6));
    }

    frame_counter++;
    uint64_t now = now_ms();
    static unsigned long total_jpeg_bytes = 0;
    total_jpeg_bytes += jpeg_size;
    if (now - last_fps_ts >= 1000u) {
      unsigned long avg_kb = frame_counter > 0 ? (total_jpeg_bytes / 1024) / frame_counter : 0;
      const struct network_stats *net = udp_sender_get_stats(&sender);
      int old_quality = quality;

      /* Adaptive target FPS: lower target when quality stuck at floor */
      static int effective_target_fps = 0;
      static int quality_floor_seconds = 0;
      static int quality_recovered_seconds = 0;
      if (effective_target_fps == 0) {
        effective_target_fps = target_fps;  /* Initialize on first run */
      }

      /* Adaptive quality: use network feedback if viewer connected, else local FPS */
      if (target_fps > 0) {
        if (net->viewer_connected) {
          /* Network-based adaptation: use packet loss and RTT */
          int loss_pct = 0;
          if (net->frames_sent > 0) {
            loss_pct = (net->frames_lost * 100) / net->frames_sent;
          }
          double rtt = net->smoothed_rtt_ms;
          double base_rtt = net->min_rtt_ms > 0 ? net->min_rtt_ms : rtt;

          /* RTT thresholds relative to baseline */
          double rtt_reduce = base_rtt * 3.0;   /* 3x baseline: reduce quality */
          double rtt_hold = base_rtt * 2.0;     /* 2x baseline: hold steady */

          if (loss_pct > 10) {
            /* High packet loss: reduce quality aggressively */
            quality -= 10;
            if (quality < 30) quality = 30;
          } else if (loss_pct > 3) {
            /* Moderate loss: reduce quality */
            quality -= 5;
            if (quality < 50) quality = 50;
          } else if (rtt > rtt_reduce) {
            /* RTT too high: proactively reduce quality before loss occurs */
            quality -= 3;
            if (quality < 50) quality = 50;
          } else if (rtt > rtt_hold) {
            /* RTT elevated: hold quality steady, don't increase */
            /* (do nothing) */
          } else if (loss_pct == 0 && net->frames_acked > 5 && quality < 95) {
            /* Low RTT, no loss, getting ACKs: can increase quality */
            quality += 2;
            if (quality > 95) quality = 95;
          }

          /* Print with network stats */
          if (quality != old_quality) {
            fprintf(stderr, "fps=%u avg_kb=%lu total_kb=%lu q=%d->%d [net: rtt=%.0f/%.0fms loss=%d%% acked=%d/%d]",
                    frame_counter, avg_kb, total_jpeg_bytes / 1024, old_quality, quality,
                    rtt, base_rtt, loss_pct, net->frames_acked, net->frames_sent);
          } else {
            fprintf(stderr, "fps=%u avg_kb=%lu total_kb=%lu q=%d [net: rtt=%.0f/%.0fms loss=%d%% acked=%d/%d]",
                    frame_counter, avg_kb, total_jpeg_bytes / 1024, quality,
                    rtt, base_rtt, loss_pct, net->frames_acked, net->frames_sent);
          }
          if (effective_target_fps != target_fps) {
            fprintf(stderr, " target=%d", effective_target_fps);
          }
          fprintf(stderr, "\n");
        } else {
          /* No viewer: use local FPS-based adaptation */
          int fps_diff = (int)frame_counter - effective_target_fps;

          if (fps_diff < -5) {
            quality -= 5;
            if (quality < 50) quality = 50;
          } else if (fps_diff >= 0 && quality < 95) {
            quality += 2;
            if (quality > 95) quality = 95;
          }

          if (effective_target_fps != target_fps) {
            fprintf(stderr, "fps=%u avg_kb=%lu total_kb=%lu q=%d target=%d\n",
                    frame_counter, avg_kb, total_jpeg_bytes / 1024, quality, effective_target_fps);
          } else if (quality != old_quality) {
            fprintf(stderr, "fps=%u avg_kb=%lu total_kb=%lu q=%d->%d\n",
                    frame_counter, avg_kb, total_jpeg_bytes / 1024, old_quality, quality);
          } else {
            fprintf(stderr, "fps=%u avg_kb=%lu total_kb=%lu q=%d\n",
                    frame_counter, avg_kb, total_jpeg_bytes / 1024, quality);
          }
        }

        /* Update encoder quality if changed */
        if (quality != old_quality) {
          if (hw_encoder_ready) {
            v4l2_jpeg_set_quality(&hw_encoder, quality);
          }
          if (sw_encoder_ready) {
            jpeg_encoder_set_quality(&encoder, quality);
          }
        }

        /* Adaptive target FPS: adjust when quality stuck at floor or recovered */
        if (quality <= 35) {
          /* Quality at or near floor */
          quality_floor_seconds++;
          quality_recovered_seconds = 0;
          if (quality_floor_seconds >= 5 && effective_target_fps > 15) {
            /* Stuck at floor for 5+ seconds: reduce target FPS */
            effective_target_fps -= 10;
            if (effective_target_fps < 15) effective_target_fps = 15;
            quality_floor_seconds = 0;
            /* Also throttle actual frame rate */
            frame_interval_ms = 1000u / (uint64_t)effective_target_fps;
            fprintf(stderr, "  -> target fps reduced to %d, throttling to %lums/frame\n",
                    effective_target_fps, (unsigned long)frame_interval_ms);
          }
        } else if (quality >= 60 && effective_target_fps < target_fps) {
          /* Quality recovered above 60 */
          quality_floor_seconds = 0;
          quality_recovered_seconds++;
          if (quality_recovered_seconds >= 10) {
            /* Stable for 10+ seconds: try increasing target FPS */
            effective_target_fps += 5;
            if (effective_target_fps > target_fps) effective_target_fps = target_fps;
            quality_recovered_seconds = 0;
            /* Update frame rate throttle */
            if (effective_target_fps >= target_fps) {
              frame_interval_ms = fps_limit > 0 ? 1000u / (uint64_t)fps_limit : 0;
            } else {
              frame_interval_ms = 1000u / (uint64_t)effective_target_fps;
            }
            fprintf(stderr, "  -> target fps increased to %d\n", effective_target_fps);
          }
        } else {
          /* Quality in middle range - reset counters */
          quality_floor_seconds = 0;
          quality_recovered_seconds = 0;
        }
      } else {
        /* No target FPS: just print stats */
        if (net->viewer_connected) {
          int loss_pct = net->frames_sent > 0 ? (net->frames_lost * 100) / net->frames_sent : 0;
          double base_rtt = net->min_rtt_ms > 0 ? net->min_rtt_ms : net->smoothed_rtt_ms;
          fprintf(stderr, "fps=%u avg_kb=%lu total_kb=%lu q=%d [net: rtt=%.0f/%.0fms loss=%d%% acked=%d/%d]\n",
                  frame_counter, avg_kb, total_jpeg_bytes / 1024, quality,
                  net->smoothed_rtt_ms, base_rtt, loss_pct, net->frames_acked, net->frames_sent);
        } else {
          fprintf(stderr, "fps=%u avg_kb=%lu total_kb=%lu q=%d\n",
                  frame_counter, avg_kb, total_jpeg_bytes / 1024, quality);
        }
      }

      /* Reset stats for next window */
      udp_sender_reset_stats(&sender);
      frame_counter = 0;
      total_jpeg_bytes = 0;
      last_fps_ts = now;
    }

    if (timing_debug) {
      uint64_t loop_end = now_ms();
      fprintf(stderr, "total=%lums\n", (unsigned long)(loop_end - frame_start));
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
  if (pending_capture) {
    dmabuf_capture_cancel(pending_capture);
    pending_capture = NULL;
  }
  if (opencl_conv) {
    opencl_convert_destroy(opencl_conv);
  }
#endif
#ifdef HAVE_AUDIO
  if (audio) {
    audio_streamer_destroy(audio);
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
