#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "decode.h"
#include "network.h"

#ifdef HAVE_AUDIO
#include "audio.h"
#endif

static void print_usage(const char *prog) {
  fprintf(stderr, "Usage: %s [--port <port>]\n", prog);
}

int main(int argc, char **argv) {
  uint16_t port = 7723;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      return 0;
    } else {
      print_usage(argv[0]);
      return 1;
    }
  }

  struct udp_receiver *receiver = NULL;
  if (udp_receiver_init(&receiver, port) != 0) {
    fprintf(stderr, "Failed to bind UDP receiver\n");
    return 1;
  }

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    udp_receiver_destroy(receiver);
    return 1;
  }

  SDL_Window *window = SDL_CreateWindow(
      "wlcast", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if (!window) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    udp_receiver_destroy(receiver);
    return 1;
  }
  SDL_RaiseWindow(window);
  fprintf(stderr, "Window created, waiting for frames on port %u...\n", port);

  SDL_Renderer *renderer = SDL_CreateRenderer(
      window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) {
    fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    udp_receiver_destroy(receiver);
    return 1;
  }

  struct jpeg_decoder decoder;
  if (jpeg_decoder_init(&decoder) != 0) {
    fprintf(stderr, "Failed to init JPEG decoder\n");
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    udp_receiver_destroy(receiver);
    return 1;
  }

#ifdef HAVE_AUDIO
  struct audio_player *audio_player = NULL;
  if (audio_player_init(&audio_player) != 0) {
    fprintf(stderr, "Warning: Failed to init audio player, continuing without audio\n");
  }
#endif

  SDL_Texture *texture = NULL;
  int tex_w = 0;
  int tex_h = 0;

  uint32_t last_fps_tick = SDL_GetTicks();
  unsigned int fps_counter = 0;

  int running = 1;
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
        running = 0;
      }
    }

    struct frame_buffer frame;
    int got = udp_receiver_poll(receiver, &frame);
    if (got < 0) {
      fprintf(stderr, "UDP receive error\n");
      running = 0;
      break;
    }

    if (got > 0) {
      struct decoded_frame decoded;
      if (jpeg_decode_frame(&decoder, frame.data, frame.size, &decoded) == 0) {
        if (!texture || decoded.width != tex_w || decoded.height != tex_h) {
          if (texture) {
            SDL_DestroyTexture(texture);
          }
          /* TJPF_BGRX = B,G,R,X in memory (bytes 0,1,2,3)
           * SDL_PIXELFORMAT_XRGB8888 on little-endian = B,G,R,X in memory
           * These match! (SDL names are bit-position, not byte order) */
          texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      decoded.width, decoded.height);
          tex_w = decoded.width;
          tex_h = decoded.height;
          SDL_RenderSetLogicalSize(renderer, tex_w, tex_h);
        }

        if (texture) {
          SDL_UpdateTexture(texture, NULL, decoded.pixels, decoded.pitch);
          SDL_RenderClear(renderer);
          SDL_RenderCopy(renderer, texture, NULL, NULL);
          SDL_RenderPresent(renderer);
        }
        fps_counter++;

        /* Send ACK back to streamer for network-based quality adaptation */
        udp_receiver_send_ack(receiver, frame.frame_id, fps_counter);
      }
    }

#ifdef HAVE_AUDIO
    /* Process any pending audio packets */
    if (audio_player) {
      struct audio_packet audio;
      while (udp_receiver_poll_audio(receiver, &audio)) {
        audio_player_process_packet(audio_player, audio.data, audio.size);
      }
    }
#endif

    uint32_t now = SDL_GetTicks();
    if (now - last_fps_tick >= 1000u) {
      char title[128];
      if (tex_w > 0 && tex_h > 0) {
        snprintf(title, sizeof(title), "wlcast - %dx%d @ %u fps", tex_w,
                 tex_h, fps_counter);
      } else {
        snprintf(title, sizeof(title), "wlcast - %u fps", fps_counter);
      }
      SDL_SetWindowTitle(window, title);
      fps_counter = 0;
      last_fps_tick = now;
    }

    if (got == 0) {
      SDL_Delay(1);  /* Only sleep when no frame received */
    }
  }

  if (texture) {
    SDL_DestroyTexture(texture);
  }
#ifdef HAVE_AUDIO
  if (audio_player) {
    audio_player_destroy(audio_player);
  }
#endif
  jpeg_decoder_destroy(&decoder);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  udp_receiver_destroy(receiver);

  return 0;
}
