#include "udp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../common/protocol.h"

int udp_sender_init(struct udp_sender *sender, const char *ip, uint16_t port) {
  memset(sender, 0, sizeof(*sender));
  sender->fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sender->fd < 0) {
    perror("socket");
    return -1;
  }

  int enable = 1;
  if (setsockopt(sender->fd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable)) <
      0) {
    perror("setsockopt SO_BROADCAST");
  }

  sender->addr.sin_family = AF_INET;
  sender->addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip, &sender->addr.sin_addr) != 1) {
    fprintf(stderr, "Invalid destination IP: %s\n", ip);
    close(sender->fd);
    sender->fd = -1;
    return -1;
  }

  sender->frame_id = 1;
  return 0;
}

int udp_sender_send_frame(struct udp_sender *sender, const uint8_t *data,
                          size_t size) {
  if (size == 0 || size > WLCAST_MAX_FRAME_SIZE) {
    fprintf(stderr, "Invalid frame size: %zu\n", size);
    return -1;
  }

  uint32_t frame_id = sender->frame_id++;
  uint16_t chunk_count = (uint16_t)((size + WLCAST_UDP_CHUNK_SIZE - 1) /
                                    WLCAST_UDP_CHUNK_SIZE);

  for (uint16_t i = 0; i < chunk_count; ++i) {
    size_t offset = (size_t)i * WLCAST_UDP_CHUNK_SIZE;
    size_t payload = size - offset;
    if (payload > WLCAST_UDP_CHUNK_SIZE) {
      payload = WLCAST_UDP_CHUNK_SIZE;
    }

    struct wlcast_udp_header header;
    header.magic = htonl(WLCAST_UDP_MAGIC);
    header.frame_id = htonl(frame_id);
    header.total_size = htonl((uint32_t)size);
    header.chunk_index = htons(i);
    header.chunk_count = htons(chunk_count);
    header.payload_size = htons((uint16_t)payload);
    header.reserved = 0;

    uint8_t packet[sizeof(header) + WLCAST_UDP_CHUNK_SIZE];
    memcpy(packet, &header, sizeof(header));
    memcpy(packet + sizeof(header), data + offset, payload);

    ssize_t sent = sendto(sender->fd, packet, sizeof(header) + payload, 0,
                          (struct sockaddr *)&sender->addr,
                          sizeof(sender->addr));
    if (sent < 0) {
      perror("sendto");
      return -1;
    }
  }

  return 0;
}

void udp_sender_close(struct udp_sender *sender) {
  if (sender->fd >= 0) {
    close(sender->fd);
  }
  memset(sender, 0, sizeof(*sender));
}
