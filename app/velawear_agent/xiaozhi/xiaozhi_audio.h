/* XiaoZhi Opus/audio adapter used by the NuttX transport. */

#ifndef VELAWEAR_XIAOZHI_AUDIO_H
#define VELAWEAR_XIAOZHI_AUDIO_H

#include <stddef.h>
#include <stdint.h>

struct xiaozhi_client;

int xiaozhi_audio_init(struct xiaozhi_client *client);
void xiaozhi_audio_deinit(struct xiaozhi_client *client);
int xiaozhi_audio_configure(struct xiaozhi_client *client,
                            unsigned int sample_rate,
                            unsigned int frame_duration_ms);
int xiaozhi_audio_listen_start(struct xiaozhi_client *client);
int xiaozhi_audio_listen_stop(struct xiaozhi_client *client);
int xiaozhi_audio_tts_start(struct xiaozhi_client *client);
int xiaozhi_audio_tts_stop(struct xiaozhi_client *client);
int xiaozhi_audio_downlink(struct xiaozhi_client *client,
                           const uint8_t *data, size_t length);

#endif /* VELAWEAR_XIAOZHI_AUDIO_H */
