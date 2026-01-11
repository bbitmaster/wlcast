#include "network.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../common/protocol.h"

struct udp_receiver {
  int fd;
  uint32_t frame_id;
  uint32_t total_size;
  uint16_t chunk_count;
  uint16_t received_count;
  uint8_t *data;
  size_t data_capacity;
  uint8_t *chunk_received;
  size_t chunk_capacity;
  int assembling;
  int frame_ready;
  uint64_t last_update_ms;
  /* Streamer address for sending ACKs */
  struct sockaddr_in streamer_addr;
  int streamer_known;
};

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void reset_assembly(struct udp_receiver *rx) {
  rx->frame_id = 0;
  rx->total_size = 0;
  rx->chunk_count = 0;
  rx->received_count = 0;
  rx->assembling = 0;
  rx->frame_ready = 0;
  rx->last_update_ms = 0;
  if (rx->chunk_received) {
    memset(rx->chunk_received, 0, rx->chunk_capacity);
  }
}

static int ensure_capacity(struct udp_receiver *rx, uint32_t total_size,
                           uint16_t chunk_count) {
  if (total_size > rx->data_capacity) {
    uint8_t *new_data = realloc(rx->data, total_size);
    if (!new_data) {
      fprintf(stderr, "realloc frame buffer failed\n");
      return -1;
    }
    rx->data = new_data;
    rx->data_capacity = total_size;
  }

  if (chunk_count > rx->chunk_capacity) {
    uint8_t *new_map = realloc(rx->chunk_received, chunk_count);
    if (!new_map) {
      fprintf(stderr, "realloc chunk map failed\n");
      return -1;
    }
    rx->chunk_received = new_map;
    rx->chunk_capacity = chunk_count;
  }
  memset(rx->chunk_received, 0, rx->chunk_capacity);
  return 0;
}

int udp_receiver_init(struct udp_receiver **out, uint16_t port) {
  struct udp_receiver *rx = calloc(1, sizeof(*rx));
  if (!rx) {
    return -1;
  }

  rx->fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (rx->fd < 0) {
    perror("socket");
    free(rx);
    return -1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (bind(rx->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(rx->fd);
    free(rx);
    return -1;
  }

  int flags = fcntl(rx->fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(rx->fd, F_SETFL, flags | O_NONBLOCK);
  }

  reset_assembly(rx);
  *out = rx;
  return 0;
}

int udp_receiver_poll(struct udp_receiver *rx, struct frame_buffer *out) {
  if (rx->frame_ready) {
    out->data = rx->data;
    out->size = rx->total_size;
    rx->frame_ready = 0;
    return 1;
  }

  uint64_t now = now_ms();
  if (rx->assembling && now - rx->last_update_ms > 200u) {
    reset_assembly(rx);
  }

  uint8_t packet[sizeof(struct wlcast_udp_header) + WLCAST_UDP_CHUNK_SIZE];
  while (1) {
    struct sockaddr_in sender_addr;
    socklen_t sender_len = sizeof(sender_addr);
    ssize_t n = recvfrom(rx->fd, packet, sizeof(packet), 0,
                         (struct sockaddr *)&sender_addr, &sender_len);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      perror("recvfrom");
      return -1;
    }

    /* Always update streamer address for ACKs (handles streamer restart) */
    rx->streamer_addr = sender_addr;
    rx->streamer_known = 1;

    if (n < (ssize_t)sizeof(struct wlcast_udp_header)) {
      continue;
    }

    struct wlcast_udp_header header;
    memcpy(&header, packet, sizeof(header));

    uint32_t magic = ntohl(header.magic);
    if (magic != WLCAST_UDP_MAGIC) {
      continue;
    }

    uint32_t frame_id = ntohl(header.frame_id);
    uint32_t total_size = ntohl(header.total_size);
    uint16_t chunk_index = ntohs(header.chunk_index);
    uint16_t chunk_count = ntohs(header.chunk_count);
    uint16_t payload_size = ntohs(header.payload_size);

    if (total_size == 0 || total_size > WLCAST_MAX_FRAME_SIZE) {
      continue;
    }
    if (chunk_count == 0 || chunk_index >= chunk_count) {
      continue;
    }
    if (payload_size == 0 || payload_size > WLCAST_UDP_CHUNK_SIZE) {
      continue;
    }

    if (sizeof(header) + payload_size > (size_t)n) {
      continue;
    }

    if (!rx->assembling || frame_id != rx->frame_id ||
        total_size != rx->total_size || chunk_count != rx->chunk_count) {
      reset_assembly(rx);
      if (ensure_capacity(rx, total_size, chunk_count) != 0) {
        reset_assembly(rx);
        continue;
      }
      rx->frame_id = frame_id;
      rx->total_size = total_size;
      rx->chunk_count = chunk_count;
      rx->assembling = 1;
    }

    size_t offset = (size_t)chunk_index * WLCAST_UDP_CHUNK_SIZE;
    if (offset + payload_size > total_size) {
      continue;
    }

    if (!rx->chunk_received[chunk_index]) {
      memcpy(rx->data + offset, packet + sizeof(header), payload_size);
      rx->chunk_received[chunk_index] = 1;
      rx->received_count++;
      rx->last_update_ms = now;
    }

    if (rx->received_count == rx->chunk_count) {
      rx->frame_ready = 1;
      rx->assembling = 0;
      out->data = rx->data;
      out->size = rx->total_size;
      out->frame_id = rx->frame_id;
      rx->frame_ready = 0;
      return 1;
    }
  }

  return 0;
}

void udp_receiver_send_ack(struct udp_receiver *rx, uint32_t frame_id,
                           uint32_t viewer_fps) {
  if (!rx || !rx->streamer_known) {
    return;
  }

  struct wlcast_ack_packet ack;
  ack.magic = htonl(WLCAST_ACK_MAGIC);
  ack.frame_id = htonl(frame_id);
  ack.viewer_fps = htonl(viewer_fps);

  sendto(rx->fd, &ack, sizeof(ack), 0, (struct sockaddr *)&rx->streamer_addr,
         sizeof(rx->streamer_addr));
}

void udp_receiver_destroy(struct udp_receiver *rx) {
  if (!rx) {
    return;
  }
  if (rx->fd >= 0) {
    close(rx->fd);
  }
  free(rx->data);
  free(rx->chunk_received);
  free(rx);
}
