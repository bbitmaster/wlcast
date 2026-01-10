#ifndef WLCAST_VIEWER_NETWORK_H
#define WLCAST_VIEWER_NETWORK_H

#include <stddef.h>
#include <stdint.h>

struct udp_receiver;

struct frame_buffer {
  uint8_t *data;
  size_t size;
};

int udp_receiver_init(struct udp_receiver **out, uint16_t port);
int udp_receiver_poll(struct udp_receiver *rx, struct frame_buffer *out);
void udp_receiver_destroy(struct udp_receiver *rx);

#endif
