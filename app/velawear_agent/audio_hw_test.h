/*
 * VelaWear - SF32LB52 on-chip audio hardware test.
 */

#ifndef VELAWEAR_AUDIO_HW_TEST_H
#define VELAWEAR_AUDIO_HW_TEST_H

#include <stdint.h>

int velawear_audio_hw_test(void);
int velawear_speaker_alert(int duration_ms, int pattern);
int velawear_music_hw_test(void);
int velawear_mic_hw_test(void);
int velawear_mic_stream_start(void);
void velawear_mic_stream_stop(void);
int velawear_mic_stream_read_level(uint32_t *avg_abs, uint32_t *peak_abs);
int velawear_audio_voice_start(uint16_t stream_id,
                               uint32_t sample_count,
                               uint32_t total_bytes,
                               uint32_t crc32);
int velawear_audio_voice_write(uint16_t stream_id,
                               uint32_t offset,
                               const uint8_t *data,
                               uint16_t length);
int velawear_audio_voice_end(uint16_t stream_id,
                             uint32_t total_bytes,
                             uint32_t crc32,
                             uint32_t *sample_count);
int velawear_audio_voice_play(uint16_t stream_id,
                              uint32_t sample_count);

#endif /* VELAWEAR_AUDIO_HW_TEST_H */
