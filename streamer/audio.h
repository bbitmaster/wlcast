#ifndef WLCAST_AUDIO_H
#define WLCAST_AUDIO_H

#include <stdint.h>

struct audio_streamer;

/* Initialize audio capture and encoding
 * Returns 0 on success, -1 on failure
 * dest_ip/port: UDP destination for audio packets
 */
int audio_streamer_init(struct audio_streamer **out, const char *dest_ip,
                        uint16_t port);

/* Start audio capture thread */
int audio_streamer_start(struct audio_streamer *as);

/* Stop audio capture thread */
void audio_streamer_stop(struct audio_streamer *as);

/* Clean up resources */
void audio_streamer_destroy(struct audio_streamer *as);

/* Get statistics */
void audio_streamer_get_stats(struct audio_streamer *as, uint32_t *packets_sent,
                              uint32_t *bytes_sent);

#endif
