/*
 * VelaWear XiaoZhi audio path.
 *
 * The upstream SF32 application uses SiFli audio_server callbacks and
 * RT-Thread ring buffers.  The current openvela image has the AUDCODEC HAL
 * path already exercised by audio_hw_test.c, so this adapter uses bounded
 * 60 ms PCM blocks and the same board-native DMA stream helpers.
 */

#include "xiaozhi_internal.h"
#include "xiaozhi_audio.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <opus.h>

#include "../drivers/audio_hw_test.h"

#define XZ_AUDIO_TAG "XiaoZhiAudio"
#define XZ_AUDIO_RATE 16000
#define XZ_AUDIO_FRAME_MS 60
#define XZ_AUDIO_MAX_SAMPLES 2048
#define XZ_AUDIO_MAX_PACKET 4096

static unsigned int xz_audio_frame_samples(const struct xiaozhi_client *client)
{
  unsigned int rate = client->sample_rate == 0 ? XZ_AUDIO_RATE :
                                                 client->sample_rate;
  unsigned int duration = client->frame_duration_ms == 0 ? XZ_AUDIO_FRAME_MS :
                                                            client->frame_duration_ms;
  unsigned int samples = (rate * duration + 999) / 1000;

  if (samples == 0 || samples > XZ_AUDIO_MAX_SAMPLES)
    {
      return XZ_AUDIO_RATE * XZ_AUDIO_FRAME_MS / 1000;
    }
  return samples;
}

static int xz_audio_create_codec(struct xiaozhi_client *client)
{
  int error;
  OpusEncoder *encoder;
  OpusDecoder *decoder;

  if (client->audio_encoder != NULL && client->audio_decoder != NULL)
    {
      return 0;
    }

  encoder = opus_encoder_create(XZ_AUDIO_RATE, 1, OPUS_APPLICATION_VOIP,
                                &error);
  if (encoder == NULL || error != OPUS_OK)
    {
      syslog(LOG_ERR, "[%s] Opus encoder create failed: %d\n",
             XZ_AUDIO_TAG, error);
      if (encoder != NULL)
        {
          opus_encoder_destroy(encoder);
        }
      return -EIO;
    }

  (void)opus_encoder_ctl(encoder, OPUS_SET_BITRATE(16000));
  (void)opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
  (void)opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(1));
  (void)opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

  decoder = opus_decoder_create(XZ_AUDIO_RATE, 1, &error);
  if (decoder == NULL || error != OPUS_OK)
    {
      syslog(LOG_ERR, "[%s] Opus decoder create failed: %d\n",
             XZ_AUDIO_TAG, error);
      opus_encoder_destroy(encoder);
      if (decoder != NULL)
        {
          opus_decoder_destroy(decoder);
        }
      return -EIO;
    }

  client->audio_encoder = encoder;
  client->audio_decoder = decoder;
  return 0;
}

int xiaozhi_audio_init(struct xiaozhi_client *client)
{
  if (client == NULL)
    {
      return -EINVAL;
    }

  if (client->sample_rate != XZ_AUDIO_RATE)
    {
      syslog(LOG_WARNING, "[%s] board audio is fixed at %uHz; server "
             "requested %uHz\n", XZ_AUDIO_TAG, XZ_AUDIO_RATE,
             client->sample_rate);
      client->sample_rate = XZ_AUDIO_RATE;
    }
  return xz_audio_create_codec(client);
}

int xiaozhi_audio_configure(struct xiaozhi_client *client,
                            unsigned int sample_rate,
                            unsigned int frame_duration_ms)
{
  if (client == NULL)
    {
      return -EINVAL;
    }

  if (sample_rate != XZ_AUDIO_RATE)
    {
      syslog(LOG_WARNING, "[%s] unsupported server sample rate %uHz; using "
             "%uHz\n", XZ_AUDIO_TAG, sample_rate, XZ_AUDIO_RATE);
      sample_rate = XZ_AUDIO_RATE;
    }
  client->sample_rate = sample_rate;
  if (frame_duration_ms == 0 || frame_duration_ms > 120 ||
      (frame_duration_ms % 20) != 0)
    {
      frame_duration_ms = XZ_AUDIO_FRAME_MS;
    }
  client->frame_duration_ms = frame_duration_ms;
  return xz_audio_create_codec(client);
}

static void *xz_audio_capture_thread(void *argument)
{
  struct xiaozhi_client *client = argument;
  int16_t pcm[XZ_AUDIO_MAX_SAMPLES];
  uint8_t packet[XZ_AUDIO_MAX_PACKET];

  while (!client->stop_requested && client->audio_capture_started)
    {
      unsigned int samples = xz_audio_frame_samples(client);
      int captured;
      int encoded;

      captured = 0;
      while ((unsigned int)captured < samples &&
             !client->stop_requested && client->audio_capture_started)
        {
          int chunk = velawear_mic_stream_read_pcm(
              pcm + captured, samples - (unsigned int)captured >= 320 ?
              320 : samples - (unsigned int)captured);
          if (chunk < 0)
            {
              captured = chunk;
              break;
            }
          if (chunk == 0)
            {
              usleep(2000);
              continue;
            }
          captured += chunk;
        }
      if (captured < 0)
        {
          if (!client->stop_requested && client->audio_capture_started)
            {
              syslog(LOG_WARNING, "[%s] capture read failed: %d\n",
                     XZ_AUDIO_TAG, captured);
            }
          break;
        }
      if (captured == 0)
        {
          continue;
        }

      encoded = opus_encode((OpusEncoder *)client->audio_encoder, pcm,
                            captured, packet, sizeof(packet));
      if (encoded < 0)
        {
          syslog(LOG_WARNING, "[%s] Opus encode failed: %s\n", XZ_AUDIO_TAG,
                 opus_strerror(encoded));
          continue;
        }

      if (xiaozhi_transport_send_binary(client, packet, (size_t)encoded) < 0)
        {
          if (!client->stop_requested)
            {
              syslog(LOG_WARNING, "[%s] uplink send failed\n", XZ_AUDIO_TAG);
            }
          break;
        }
    }

  return NULL;
}

int xiaozhi_audio_listen_start(struct xiaozhi_client *client)
{
  int ret;

  if (client == NULL || !client->session_ready)
    {
      return -ENOTCONN;
    }
  if (client->audio_thread_started)
    {
      return 0;
    }

  ret = xz_audio_create_codec(client);
  if (ret < 0)
    {
      return ret;
    }
  if (client->sample_rate != XZ_AUDIO_RATE)
    {
      return -ENOTSUP;
    }
  ret = velawear_mic_stream_start();
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[%s] capture open failed: %d\n", XZ_AUDIO_TAG,
             ret);
      return ret;
    }

  client->audio_capture_started = true;
  ret = pthread_create(&client->audio_thread, NULL, xz_audio_capture_thread,
                       client);
  if (ret != 0)
    {
      client->audio_capture_started = false;
      velawear_mic_stream_stop();
      return -ret;
    }
  client->audio_thread_started = true;
  syslog(LOG_INFO, "[%s] microphone streaming started (%uHz, %ums)\n",
         XZ_AUDIO_TAG, client->sample_rate, client->frame_duration_ms);
  return 0;
}

int xiaozhi_audio_listen_stop(struct xiaozhi_client *client)
{
  if (client == NULL)
    {
      return -EINVAL;
    }

  client->audio_capture_started = false;
  if (client->audio_thread_started)
    {
      (void)pthread_join(client->audio_thread, NULL);
      client->audio_thread_started = false;
    }
  velawear_mic_stream_stop();
  return 0;
}

int xiaozhi_audio_tts_start(struct xiaozhi_client *client)
{
  if (client == NULL)
    {
      return -EINVAL;
    }
  if (client->audio_playback_started)
    {
      return 0;
    }

  if (client->sample_rate != XZ_AUDIO_RATE)
    {
      return -ENOTSUP;
    }
  client->audio_playback_started = true;
  syslog(LOG_INFO, "[%s] speaker streaming started (%uHz)\n", XZ_AUDIO_TAG,
         client->sample_rate);
  return 0;
}

int xiaozhi_audio_tts_stop(struct xiaozhi_client *client)
{
  if (client == NULL)
    {
      return -EINVAL;
    }

  if (client->audio_playback_started)
    {
      /* Playback is block-oriented in the current SF32LB52 HAL adapter; no
       * persistent player handle needs to be closed here. */
      client->audio_playback_started = false;
    }
  return 0;
}

int xiaozhi_audio_downlink(struct xiaozhi_client *client,
                           const uint8_t *data, size_t length)
{
  int16_t pcm[XZ_AUDIO_MAX_SAMPLES];
  int decoded;
  int ret;

  if (client == NULL || data == NULL || length == 0)
    {
      return -EINVAL;
    }
  if (client->audio_decoder == NULL)
    {
      ret = xz_audio_create_codec(client);
      if (ret < 0)
        {
          return ret;
        }
    }
  if (!client->audio_playback_started)
    {
      ret = xiaozhi_audio_tts_start(client);
      if (ret < 0)
        {
          return ret;
        }
    }

  decoded = opus_decode((OpusDecoder *)client->audio_decoder, data, length,
                        pcm, xz_audio_frame_samples(client), 0);
  if (decoded < 0)
    {
      syslog(LOG_WARNING, "[%s] Opus decode failed: %s\n", XZ_AUDIO_TAG,
             opus_strerror(decoded));
      return decoded;
    }
  if (decoded == 0)
    {
      return 0;
    }
  ret = velawear_audio_pcm_play(pcm, (size_t)decoded);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[%s] playback write failed: %d\n", XZ_AUDIO_TAG,
             ret);
      return ret;
    }
  return 0;
}

void xiaozhi_audio_deinit(struct xiaozhi_client *client)
{
  if (client == NULL)
    {
      return;
    }

  (void)xiaozhi_audio_listen_stop(client);
  (void)xiaozhi_audio_tts_stop(client);
  if (client->audio_encoder != NULL)
    {
      opus_encoder_destroy((OpusEncoder *)client->audio_encoder);
      client->audio_encoder = NULL;
    }
  if (client->audio_decoder != NULL)
    {
      opus_decoder_destroy((OpusDecoder *)client->audio_decoder);
      client->audio_decoder = NULL;
    }
}
