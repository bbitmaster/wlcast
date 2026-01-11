#ifndef WLCAST_UDP_H
#define WLCAST_UDP_H

#include <stddef.h>
#include <stdint.h>

#include <netinet/in.h>

/* Track sent frames for RTT calculation */
#define FRAME_HISTORY_SIZE 64

struct frame_record {
  uint32_t frame_id;
  uint64_t sent_time_ms;
  int acked;
};

/* Network quality metrics from ACKs */
struct network_stats {
  int viewer_connected;      /* 1 if receiving ACKs */
  uint64_t last_ack_time_ms; /* When we last received an ACK */
  double smoothed_rtt_ms;    /* Exponential moving average of RTT */
  double min_rtt_ms;         /* Minimum RTT observed (baseline) */
  uint32_t viewer_fps;       /* FPS reported by viewer */
  int frames_sent;           /* Frames sent in current window */
  int frames_acked;          /* Frames ACKed in current window */
  int frames_lost;           /* Frames presumed lost (timeout) */
};

struct udp_sender {
  int fd;
  uint32_t frame_id;
  struct sockaddr_in addr;
  /* Frame tracking for RTT/loss detection */
  struct frame_record history[FRAME_HISTORY_SIZE];
  int history_idx;
  struct network_stats stats;
};

int udp_sender_init(struct udp_sender *sender, const char *ip, uint16_t port);
int udp_sender_send_frame(struct udp_sender *sender, const uint8_t *data,
                          size_t size);
void udp_sender_close(struct udp_sender *sender);

/* Check for incoming ACKs (non-blocking) and update stats */
void udp_sender_poll_acks(struct udp_sender *sender);

/* Get current network stats (call after poll_acks) */
const struct network_stats *udp_sender_get_stats(struct udp_sender *sender);

/* Reset stats for next measurement window */
void udp_sender_reset_stats(struct udp_sender *sender);

#endif
