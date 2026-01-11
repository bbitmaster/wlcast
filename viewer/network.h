#ifndef WLCAST_VIEWER_NETWORK_H
#define WLCAST_VIEWER_NETWORK_H

#include <stddef.h>
#include <stdint.h>

struct udp_receiver;

struct frame_buffer {
  uint8_t *data;
  size_t size;
  uint32_t frame_id;  /* Frame ID for ACK */
};

/* Audio packet buffer */
struct audio_packet {
  uint8_t *data;
  size_t size;
};

int udp_receiver_init(struct udp_receiver **out, uint16_t port);

/* Poll for video frames. Returns 1 if frame ready, 0 if not, -1 on error */
int udp_receiver_poll(struct udp_receiver *rx, struct frame_buffer *out);

/* Poll for audio packets. Returns 1 if packet ready, 0 if not.
 * Audio packets are returned directly without assembly (single packet per frame).
 * Call this after udp_receiver_poll to process any pending audio packets. */
int udp_receiver_poll_audio(struct udp_receiver *rx, struct audio_packet *out);

void udp_receiver_destroy(struct udp_receiver *rx);

/* Send ACK for a received frame (call after displaying) */
void udp_receiver_send_ack(struct udp_receiver *rx, uint32_t frame_id,
                           uint32_t viewer_fps);

#endif
