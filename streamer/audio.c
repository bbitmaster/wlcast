#include "audio.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <opus/opus.h>
#include <pulse/simple.h>
#include <pulse/error.h>

#include "../common/protocol.h"

/* Samples per Opus frame (20ms at 48kHz) */
#define FRAME_SAMPLES (WLCAST_AUDIO_SAMPLE_RATE * WLCAST_AUDIO_FRAME_MS / 1000)
/* PCM buffer size for one frame (stereo int16) */
#define PCM_FRAME_SIZE (FRAME_SAMPLES * WLCAST_AUDIO_CHANNELS * sizeof(int16_t))
/* Maximum Opus packet size */
#define MAX_OPUS_PACKET 1500

struct audio_streamer {
  /* PulseAudio */
  pa_simple *pa;

  /* Opus encoder */
  OpusEncoder *encoder;

  /* UDP socket */
  int fd;
  struct sockaddr_in dest_addr;

  /* Thread control */
  pthread_t thread;
  volatile int running;

  /* Stats */
  uint32_t packets_sent;
  uint32_t bytes_sent;
  uint32_t sequence;
  uint32_t timestamp;
};

static void *audio_thread(void *arg) {
  struct audio_streamer *as = arg;
  int16_t pcm_buffer[FRAME_SAMPLES * WLCAST_AUDIO_CHANNELS];
  uint8_t opus_buffer[MAX_OPUS_PACKET];
  uint8_t packet[sizeof(struct wlcast_audio_header) + MAX_OPUS_PACKET];
  int error;

  while (as->running) {
    /* Read PCM from PulseAudio (blocking) */
    if (pa_simple_read(as->pa, pcm_buffer, PCM_FRAME_SIZE, &error) < 0) {
      fprintf(stderr, "pa_simple_read failed: %s\n", pa_strerror(error));
      break;
    }

    /* Encode to Opus */
    int opus_len = opus_encode(as->encoder, pcm_buffer, FRAME_SAMPLES,
                               opus_buffer, MAX_OPUS_PACKET);
    if (opus_len < 0) {
      fprintf(stderr, "opus_encode failed: %s\n", opus_strerror(opus_len));
      continue;
    }

    /* Build packet */
    struct wlcast_audio_header header;
    header.magic = htonl(WLCAST_AUDIO_MAGIC);
    header.sequence = htonl(as->sequence++);
    header.timestamp = htonl(as->timestamp);
    header.payload_size = htons((uint16_t)opus_len);
    header.reserved = 0;

    memcpy(packet, &header, sizeof(header));
    memcpy(packet + sizeof(header), opus_buffer, opus_len);

    /* Send UDP packet */
    ssize_t sent = sendto(as->fd, packet, sizeof(header) + opus_len, 0,
                          (struct sockaddr *)&as->dest_addr,
                          sizeof(as->dest_addr));
    if (sent > 0) {
      as->packets_sent++;
      as->bytes_sent += (uint32_t)sent;
    }

    /* Advance timestamp by samples per frame */
    as->timestamp += FRAME_SAMPLES;
  }

  return NULL;
}

int audio_streamer_init(struct audio_streamer **out, const char *dest_ip,
                        uint16_t port) {
  struct audio_streamer *as = calloc(1, sizeof(*as));
  if (!as) {
    return -1;
  }

  /* Initialize PulseAudio for recording from monitor source */
  pa_sample_spec ss = {
      .format = PA_SAMPLE_S16LE,
      .rate = WLCAST_AUDIO_SAMPLE_RATE,
      .channels = WLCAST_AUDIO_CHANNELS,
  };

  pa_buffer_attr ba = {
      .maxlength = (uint32_t)-1,
      .tlength = (uint32_t)-1,
      .prebuf = (uint32_t)-1,
      .minreq = (uint32_t)-1,
      .fragsize = PCM_FRAME_SIZE,  /* Request 20ms fragments */
  };

  int error;
  /* Record from default monitor source (speaker output loopback) */
  as->pa = pa_simple_new(NULL, "wlcast", PA_STREAM_RECORD, NULL,
                         "screen capture audio", &ss, NULL, &ba, &error);
  if (!as->pa) {
    fprintf(stderr, "pa_simple_new failed: %s\n", pa_strerror(error));
    free(as);
    return -1;
  }

  /* Initialize Opus encoder */
  as->encoder = opus_encoder_create(WLCAST_AUDIO_SAMPLE_RATE,
                                    WLCAST_AUDIO_CHANNELS,
                                    OPUS_APPLICATION_AUDIO, &error);
  if (!as->encoder) {
    fprintf(stderr, "opus_encoder_create failed: %s\n", opus_strerror(error));
    pa_simple_free(as->pa);
    free(as);
    return -1;
  }

  /* Configure encoder for low latency */
  opus_encoder_ctl(as->encoder, OPUS_SET_BITRATE(WLCAST_AUDIO_BITRATE));
  opus_encoder_ctl(as->encoder, OPUS_SET_COMPLEXITY(5));  /* Balance quality/CPU */
  opus_encoder_ctl(as->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

  /* Create UDP socket */
  as->fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (as->fd < 0) {
    perror("socket");
    opus_encoder_destroy(as->encoder);
    pa_simple_free(as->pa);
    free(as);
    return -1;
  }

  /* Set destination address */
  as->dest_addr.sin_family = AF_INET;
  as->dest_addr.sin_port = htons(port);
  if (inet_pton(AF_INET, dest_ip, &as->dest_addr.sin_addr) != 1) {
    fprintf(stderr, "Invalid audio destination IP: %s\n", dest_ip);
    close(as->fd);
    opus_encoder_destroy(as->encoder);
    pa_simple_free(as->pa);
    free(as);
    return -1;
  }

  as->running = 0;
  as->packets_sent = 0;
  as->bytes_sent = 0;
  as->sequence = 0;
  as->timestamp = 0;

  *out = as;
  return 0;
}

int audio_streamer_start(struct audio_streamer *as) {
  if (as->running) {
    return 0;
  }

  as->running = 1;
  if (pthread_create(&as->thread, NULL, audio_thread, as) != 0) {
    perror("pthread_create");
    as->running = 0;
    return -1;
  }

  fprintf(stderr, "Audio streaming started (Opus %dkbps, %dms frames)\n",
          WLCAST_AUDIO_BITRATE / 1000, WLCAST_AUDIO_FRAME_MS);
  return 0;
}

void audio_streamer_stop(struct audio_streamer *as) {
  if (!as->running) {
    return;
  }

  as->running = 0;
  pthread_join(as->thread, NULL);
  fprintf(stderr, "Audio streaming stopped\n");
}

void audio_streamer_destroy(struct audio_streamer *as) {
  if (!as) {
    return;
  }

  audio_streamer_stop(as);

  if (as->fd >= 0) {
    close(as->fd);
  }
  if (as->encoder) {
    opus_encoder_destroy(as->encoder);
  }
  if (as->pa) {
    pa_simple_free(as->pa);
  }
  free(as);
}

void audio_streamer_get_stats(struct audio_streamer *as, uint32_t *packets_sent,
                              uint32_t *bytes_sent) {
  if (packets_sent) {
    *packets_sent = as->packets_sent;
  }
  if (bytes_sent) {
    *bytes_sent = as->bytes_sent;
  }
}
