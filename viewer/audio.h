#ifndef WLCAST_VIEWER_AUDIO_H
#define WLCAST_VIEWER_AUDIO_H

#include <stdint.h>
#include <stddef.h>

struct audio_player;

/* Initialize audio player (SDL audio + Opus decoder) */
int audio_player_init(struct audio_player **out);

/* Process an incoming audio packet (Opus data)
 * packet: raw UDP packet data (includes header)
 * size: packet size
 */
void audio_player_process_packet(struct audio_player *ap, const uint8_t *packet,
                                 size_t size);

/* Clean up */
void audio_player_destroy(struct audio_player *ap);

#endif
