#include "audio.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <opus/opus.h>

#include "../common/protocol.h"

/* Ring buffer size (in samples, MUST be power of 2 for mask to work) */
#define RING_BUFFER_SAMPLES 65536  /* ~1.36 seconds at 48kHz (2^16) */
#define RING_BUFFER_MASK (RING_BUFFER_SAMPLES - 1)

/* Samples per Opus frame (20ms at 48kHz) */
#define FRAME_SAMPLES (WLCAST_AUDIO_SAMPLE_RATE * WLCAST_AUDIO_FRAME_MS / 1000)

struct audio_player {
  OpusDecoder *decoder;
  SDL_AudioDeviceID dev;

  /* Ring buffer for decoded audio */
  int16_t ring_buffer[RING_BUFFER_SAMPLES * WLCAST_AUDIO_CHANNELS];
  volatile uint32_t write_pos;
  volatile uint32_t read_pos;

  /* Stats */
  uint32_t packets_received;
  uint32_t underruns;
};

static uint32_t g_audio_callbacks = 0;
static uint32_t g_audio_played = 0;

static void audio_callback(void *userdata, Uint8 *stream, int len) {
  struct audio_player *ap = userdata;
  int16_t *out = (int16_t *)stream;
  int samples_needed = len / (int)(sizeof(int16_t) * WLCAST_AUDIO_CHANNELS);

  g_audio_callbacks++;

  uint32_t read_pos = ap->read_pos;
  uint32_t write_pos = ap->write_pos;

  /* Calculate available samples */
  uint32_t available = (write_pos - read_pos) & RING_BUFFER_MASK;

  if (available < (uint32_t)samples_needed) {
    /* Underrun - fill with silence */
    memset(stream, 0, (size_t)len);
    ap->underruns++;
    return;
  }

  g_audio_played++;

  /* Copy samples from ring buffer */
  for (int i = 0; i < samples_needed; i++) {
    uint32_t idx = (read_pos + (uint32_t)i) & RING_BUFFER_MASK;
    out[i * 2] = ap->ring_buffer[idx * 2];
    out[i * 2 + 1] = ap->ring_buffer[idx * 2 + 1];
  }

  ap->read_pos = (read_pos + (uint32_t)samples_needed) & RING_BUFFER_MASK;
}

int audio_player_init(struct audio_player **out) {
  struct audio_player *ap = calloc(1, sizeof(*ap));
  if (!ap) {
    return -1;
  }

  /* Initialize Opus decoder */
  int error;
  ap->decoder = opus_decoder_create(WLCAST_AUDIO_SAMPLE_RATE,
                                    WLCAST_AUDIO_CHANNELS, &error);
  if (!ap->decoder) {
    fprintf(stderr, "opus_decoder_create failed: %s\n", opus_strerror(error));
    free(ap);
    return -1;
  }

  /* Initialize SDL audio */
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    fprintf(stderr, "SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
    opus_decoder_destroy(ap->decoder);
    free(ap);
    return -1;
  }

  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = WLCAST_AUDIO_SAMPLE_RATE;
  want.format = AUDIO_S16LSB;
  want.channels = WLCAST_AUDIO_CHANNELS;
  want.samples = 512;  /* ~10ms buffer - small to reduce latency, needs only 1 packet */
  want.callback = audio_callback;
  want.userdata = ap;

  ap->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (ap->dev == 0) {
    fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    opus_decoder_destroy(ap->decoder);
    free(ap);
    return -1;
  }

  /* Start playback immediately */
  SDL_PauseAudioDevice(ap->dev, 0);

  ap->write_pos = 0;
  ap->read_pos = 0;
  ap->packets_received = 0;
  ap->underruns = 0;

  fprintf(stderr, "Audio player initialized (%dHz, %dch, buffer %d samples)\n",
          have.freq, have.channels, have.samples);

  *out = ap;
  return 0;
}

void audio_player_process_packet(struct audio_player *ap, const uint8_t *packet,
                                 size_t size) {
  if (!ap || size < sizeof(struct wlcast_audio_header)) {
    return;
  }

  /* Parse header */
  struct wlcast_audio_header header;
  memcpy(&header, packet, sizeof(header));

  uint32_t magic = ntohl(header.magic);
  if (magic != WLCAST_AUDIO_MAGIC) {
    return;
  }

  uint16_t payload_size = ntohs(header.payload_size);
  if (sizeof(header) + payload_size > size) {
    return;
  }

  const uint8_t *opus_data = packet + sizeof(header);

  /* Decode Opus to PCM */
  int16_t pcm_buffer[FRAME_SAMPLES * WLCAST_AUDIO_CHANNELS];
  int samples = opus_decode(ap->decoder, opus_data, payload_size,
                            pcm_buffer, FRAME_SAMPLES, 0);
  if (samples < 0) {
    fprintf(stderr, "opus_decode failed: %s\n", opus_strerror(samples));
    return;
  }

  /* Write to ring buffer (lock to prevent race with callback) */
  SDL_LockAudioDevice(ap->dev);
  uint32_t write_pos = ap->write_pos;
  for (int i = 0; i < samples; i++) {
    uint32_t idx = (write_pos + (uint32_t)i) & RING_BUFFER_MASK;
    ap->ring_buffer[idx * 2] = pcm_buffer[i * 2];
    ap->ring_buffer[idx * 2 + 1] = pcm_buffer[i * 2 + 1];
  }
  ap->write_pos = (write_pos + (uint32_t)samples) & RING_BUFFER_MASK;
  SDL_UnlockAudioDevice(ap->dev);

  ap->packets_received++;
}

void audio_player_destroy(struct audio_player *ap) {
  if (!ap) {
    return;
  }

  if (ap->dev != 0) {
    SDL_CloseAudioDevice(ap->dev);
  }
  if (ap->decoder) {
    opus_decoder_destroy(ap->decoder);
  }

  fprintf(stderr, "Audio: %u packets received, %u callbacks, %u played, %u underruns\n",
          ap->packets_received, g_audio_callbacks, g_audio_played, ap->underruns);
  free(ap);
}
