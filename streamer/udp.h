#ifndef WLCAST_UDP_H
#define WLCAST_UDP_H

#include <stddef.h>
#include <stdint.h>

#include <netinet/in.h>

struct udp_sender {
  int fd;
  uint32_t frame_id;
  struct sockaddr_in addr;
};

int udp_sender_init(struct udp_sender *sender, const char *ip, uint16_t port);
int udp_sender_send_frame(struct udp_sender *sender, const uint8_t *data,
                          size_t size);
void udp_sender_close(struct udp_sender *sender);

#endif
