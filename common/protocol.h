#ifndef WLCAST_PROTOCOL_H
#define WLCAST_PROTOCOL_H

#include <stdint.h>

#define WLCAST_UDP_MAGIC 0x574c4350u /* "WLCP" */
#define WLCAST_UDP_CHUNK_SIZE 1200u
#define WLCAST_MAX_FRAME_SIZE (8u * 1024u * 1024u)
#define WLCAST_UDP_HEADER_SIZE 20u

struct wlcast_udp_header {
  uint32_t magic;
  uint32_t frame_id;
  uint32_t total_size;
  uint16_t chunk_index;
  uint16_t chunk_count;
  uint16_t payload_size;
  uint16_t reserved;
};

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(struct wlcast_udp_header) == WLCAST_UDP_HEADER_SIZE,
               "wlcast_udp_header size mismatch");
#endif

#endif
