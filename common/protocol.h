#ifndef WLCAST_PROTOCOL_H
#define WLCAST_PROTOCOL_H

#include <stdint.h>

#define WLCAST_UDP_MAGIC 0x574c4350u /* "WLCP" - video frame packet */
#define WLCAST_ACK_MAGIC 0x574c4341u /* "WLCA" - ACK packet */
#define WLCAST_AUDIO_MAGIC 0x574c4155u /* "WLAU" - audio packet */
#define WLCAST_UDP_CHUNK_SIZE 8000u  /* Large chunks - kernel handles IP fragmentation */
#define WLCAST_MAX_FRAME_SIZE (8u * 1024u * 1024u)
#define WLCAST_UDP_HEADER_SIZE 20u
#define WLCAST_ACK_SIZE 12u
#define WLCAST_AUDIO_HEADER_SIZE 16u

/* Audio constants */
#define WLCAST_AUDIO_SAMPLE_RATE 48000u
#define WLCAST_AUDIO_CHANNELS 2u
#define WLCAST_AUDIO_FRAME_MS 20u  /* 20ms Opus frames for low latency */
#define WLCAST_AUDIO_BITRATE 64000u  /* 64kbps Opus - good quality, low bandwidth */

/* Frame data packet header */
struct wlcast_udp_header {
  uint32_t magic;
  uint32_t frame_id;
  uint32_t total_size;
  uint16_t chunk_index;
  uint16_t chunk_count;
  uint16_t payload_size;
  uint16_t reserved;
};

/* ACK packet sent from viewer to streamer */
struct wlcast_ack_packet {
  uint32_t magic;      /* WLCAST_ACK_MAGIC */
  uint32_t frame_id;   /* Frame being acknowledged */
  uint32_t viewer_fps; /* Viewer's current display FPS (for info) */
};

/* Audio packet header - Opus encoded audio data follows */
struct wlcast_audio_header {
  uint32_t magic;        /* WLCAST_AUDIO_MAGIC */
  uint32_t sequence;     /* Sequence number for ordering/loss detection */
  uint32_t timestamp;    /* Sample timestamp (for sync) */
  uint16_t payload_size; /* Size of Opus data following header */
  uint16_t reserved;
};

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(struct wlcast_udp_header) == WLCAST_UDP_HEADER_SIZE,
               "wlcast_udp_header size mismatch");
_Static_assert(sizeof(struct wlcast_ack_packet) == WLCAST_ACK_SIZE,
               "wlcast_ack_packet size mismatch");
_Static_assert(sizeof(struct wlcast_audio_header) == WLCAST_AUDIO_HEADER_SIZE,
               "wlcast_audio_header size mismatch");
#endif

#endif
