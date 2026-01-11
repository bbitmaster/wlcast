#include "udp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../common/protocol.h"

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

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

  /* Increase send buffer to reduce blocking (512KB) */
  int sndbuf = 512 * 1024;
  if (setsockopt(sender->fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
    perror("setsockopt SO_SNDBUF");
  }

  /* Make socket non-blocking to poll for ACKs */
  int flags = fcntl(sender->fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(sender->fd, F_SETFL, flags | O_NONBLOCK);
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
  sender->history_idx = 0;
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

  /* Record this frame for RTT tracking */
  int idx = sender->history_idx;
  sender->history[idx].frame_id = frame_id;
  sender->history[idx].sent_time_ms = now_ms();
  sender->history[idx].acked = 0;
  sender->history_idx = (idx + 1) % FRAME_HISTORY_SIZE;
  sender->stats.frames_sent++;

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
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* Non-blocking socket would block - skip this chunk */
        continue;
      }
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

void udp_sender_poll_acks(struct udp_sender *sender) {
  uint64_t now = now_ms();

  /* Check if viewer disconnected (no ACKs for 2 seconds) */
  if (sender->stats.viewer_connected &&
      now - sender->stats.last_ack_time_ms > 2000) {
    sender->stats.viewer_connected = 0;
  }

  /* Read all pending ACK packets */
  while (1) {
    struct wlcast_ack_packet ack;
    ssize_t n = recvfrom(sender->fd, &ack, sizeof(ack), 0, NULL, NULL);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break; /* No more packets */
      }
      /* Ignore other errors */
      break;
    }

    if (n != sizeof(ack)) {
      continue; /* Wrong size */
    }

    uint32_t magic = ntohl(ack.magic);
    if (magic != WLCAST_ACK_MAGIC) {
      continue; /* Not an ACK */
    }

    uint32_t frame_id = ntohl(ack.frame_id);
    uint32_t viewer_fps = ntohl(ack.viewer_fps);

    /* Detect viewer (re)connection */
    if (!sender->stats.viewer_connected) {
      /* Viewer just connected - reset baseline to re-learn network conditions */
      sender->stats.min_rtt_ms = 0;
      sender->stats.smoothed_rtt_ms = 0;
    }
    sender->stats.viewer_connected = 1;
    sender->stats.last_ack_time_ms = now;
    sender->stats.viewer_fps = viewer_fps;

    /* Find this frame in history and calculate RTT */
    for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
      if (sender->history[i].frame_id == frame_id && !sender->history[i].acked) {
        sender->history[i].acked = 1;
        sender->stats.frames_acked++;

        uint64_t rtt = now - sender->history[i].sent_time_ms;

        /* Exponential moving average for RTT (alpha = 0.2) */
        if (sender->stats.smoothed_rtt_ms == 0) {
          sender->stats.smoothed_rtt_ms = (double)rtt;
        } else {
          sender->stats.smoothed_rtt_ms =
              0.8 * sender->stats.smoothed_rtt_ms + 0.2 * (double)rtt;
        }

        /* Track minimum RTT as baseline (with floor of 5ms) */
        double rtt_d = (double)rtt;
        if (rtt_d < 5.0) rtt_d = 5.0;  /* Floor to avoid overly sensitive thresholds */
        if (sender->stats.min_rtt_ms == 0 || rtt_d < sender->stats.min_rtt_ms) {
          sender->stats.min_rtt_ms = rtt_d;
        }
        break;
      }
    }
  }

  /* Count frames that are presumed lost (sent > 500ms ago, not acked) */
  int lost = 0;
  for (int i = 0; i < FRAME_HISTORY_SIZE; i++) {
    if (sender->history[i].frame_id != 0 && !sender->history[i].acked &&
        now - sender->history[i].sent_time_ms > 500) {
      lost++;
      /* Mark as "acked" so we don't count it again */
      sender->history[i].acked = 1;
    }
  }
  sender->stats.frames_lost += lost;
}

const struct network_stats *udp_sender_get_stats(struct udp_sender *sender) {
  return &sender->stats;
}

void udp_sender_reset_stats(struct udp_sender *sender) {
  /* Slowly drift min_rtt toward smoothed_rtt (1% per second)
   * This prevents a lucky early measurement from locking in an
   * unrealistic baseline forever. After ~1 minute, baseline reflects
   * "recent best" rather than "all-time best". */
  if (sender->stats.min_rtt_ms > 0 && sender->stats.smoothed_rtt_ms > 0) {
    sender->stats.min_rtt_ms =
        sender->stats.min_rtt_ms * 0.99 + sender->stats.smoothed_rtt_ms * 0.01;
  }

  /* Keep viewer_connected, last_ack_time, smoothed_rtt, min_rtt, viewer_fps */
  sender->stats.frames_sent = 0;
  sender->stats.frames_acked = 0;
  sender->stats.frames_lost = 0;
}
