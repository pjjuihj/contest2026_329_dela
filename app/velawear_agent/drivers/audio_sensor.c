/*
 * VelaWear Agent - Audio Sensor Driver
 *
 * The Huangshan Pi has an onboard MEMS microphone wired to the SF32LB52
 * AUDCODEC ADC.  The board port has no /dev/audio/pcm_in0 node, so this
 * driver owns the direct AUDCODEC circular-DMA stream.
 */

#include <nuttx/config.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <time.h>

#include "velawear.h"
#include "../audio_hw_test.h"

#define AUDIO_LEVEL_START_THRESHOLD 500U
#define AUDIO_LEVEL_STOP_THRESHOLD  300U
#define AUDIO_LEVEL_DEBOUNCE_HALVES 3U
#define AUDIO_STREAM_START_GRACE_MS 500U

static uint32_t audio_sensor_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return (uint32_t)time(NULL) * 1000U;
    }

  return (uint32_t)(ts.tv_sec * 1000U + ts.tv_nsec / 1000000U);
}

static void audio_sensor_push_event(velawear_audio_t *audio,
                                    bool active,
                                    uint32_t avg_abs,
                                    uint32_t peak_abs)
{
  velawear_events_t *events;
  velawear_event_t event;

  if (audio == NULL || audio->events == NULL)
    {
      return;
    }

  events = (velawear_events_t *)audio->events;
  memset(&event, 0, sizeof(event));
  event.type = VELAWEAR_EVENT_AUDIO;
  event.priority = VELAWEAR_PRIORITY_NORMAL;
  event.timestamp = audio_sensor_now_ms();
  event.data.audio.active = active;
  event.data.audio.avg_abs = avg_abs;
  event.data.audio.peak_abs = peak_abs;
  if (active)
    {
      strncpy(event.data.audio.text, "sound_start",
              sizeof(event.data.audio.text) - 1);
    }
  else
    {
      strncpy(event.data.audio.text, "sound_end",
              sizeof(event.data.audio.text) - 1);
    }
  event.data.audio.length = (int)strlen(event.data.audio.text);

  if (event_manager_push(events, &event) < 0)
    {
      syslog(LOG_WARNING, "[AudioStream] audio event queue full\n");
    }
}

static void *audio_sensor_thread(void *context)
{
  velawear_audio_t *audio = (velawear_audio_t *)context;
  uint32_t start_ms = audio_sensor_now_ms();
  uint32_t last_report_ms = start_ms;
  uint32_t active_streak = 0;
  uint32_t silent_streak = 0;
  bool stream_has_samples = false;

  syslog(LOG_INFO, "[AudioStream] level worker started\n");
  while (audio->stream_running)
    {
      uint32_t avg_abs;
      uint32_t peak_abs;
      int ret = velawear_mic_stream_read_level(&avg_abs, &peak_abs);

      if (ret < 0)
        {
          syslog(LOG_ERR, "[AudioStream] level read failed\n");
          break;
        }

      if (ret > 0)
        {
          uint32_t now_ms = audio_sensor_now_ms();
          bool previous_active;
          bool current_active;

          /* DMA is restarted after playback.  Do not interpret its first
           * samples as microphone activity: DAC residue reaches the ADC
           * briefly even when the external PA is disabled. */
          if (!stream_has_samples)
            {
              stream_has_samples = true;
              start_ms = now_ms;
              active_streak = 0;
              silent_streak = 0;
              audio->level_active = false;
            }

          previous_active = audio->level_active;
          current_active = previous_active;

          if (!previous_active)
            {
              silent_streak = 0;
              if (avg_abs >= AUDIO_LEVEL_START_THRESHOLD)
                {
                  active_streak++;
                }
              else
                {
                  active_streak = 0;
                }

              if (now_ms - start_ms >= AUDIO_STREAM_START_GRACE_MS &&
                  active_streak >= AUDIO_LEVEL_DEBOUNCE_HALVES)
                {
                  current_active = true;
                  active_streak = 0;
                }
            }
          else
            {
              active_streak = 0;
              if (avg_abs <= AUDIO_LEVEL_STOP_THRESHOLD)
                {
                  silent_streak++;
                }
              else
                {
                  silent_streak = 0;
                }

              if (silent_streak >= AUDIO_LEVEL_DEBOUNCE_HALVES)
                {
                  current_active = false;
                  silent_streak = 0;
                }
            }

          audio->latest_avg_abs = avg_abs;
          audio->latest_peak_abs = peak_abs;
          audio->level_active = current_active;

          if (current_active != previous_active)
            {
              syslog(LOG_INFO,
                     "[AudioStream] sound %s avg_abs=%lu peak_abs=%lu\n",
                     current_active ? "start" : "end",
                     (unsigned long)avg_abs, (unsigned long)peak_abs);
              audio_sensor_push_event(audio, current_active, avg_abs,
                                      peak_abs);
              last_report_ms = now_ms;
            }
          else if (now_ms - last_report_ms >= 5000U)
            {
              syslog(LOG_INFO,
                     "[AudioStream] level avg_abs=%lu peak_abs=%lu active=%d\n",
                     (unsigned long)avg_abs, (unsigned long)peak_abs,
                     current_active ? 1 : 0);
              last_report_ms = now_ms;
            }
        }
      else
        {
          /* Playback owns the codec.  The next ready half starts a fresh
           * debounce window after microphone DMA is resumed. */
          stream_has_samples = false;
          active_streak = 0;
          silent_streak = 0;
        }

      usleep(10000);
    }

  audio->stream_running = false;
  syslog(LOG_INFO, "[AudioStream] level worker stopped\n");
  return NULL;
}

int audio_sensor_init(velawear_audio_t *audio)
{
  int ret;

  if (audio == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  memset(audio, 0, sizeof(velawear_audio_t));
  audio->fd = -1;
  audio->sample_rate = 16000;

  syslog(LOG_INFO,
         "[Audio] Probing onboard MEMS microphone via direct AUDCODEC ADC\n");
  ret = velawear_mic_hw_test();
  if (ret != 0)
    {
      syslog(LOG_WARNING,
             "[Audio] Onboard MEMS microphone ADC probe failed: %d\n", ret);
      return VELAWEAR_ERR_IO;
    }

  audio->initialized = true;
  syslog(LOG_INFO,
         "[Audio] Onboard MEMS microphone ready via direct AUDCODEC ADC at %d Hz\n",
         audio->sample_rate);
  return VELAWEAR_OK;
}

int audio_sensor_start(velawear_audio_t *audio, void *events)
{
  int ret;

  if (audio == NULL || !audio->initialized)
    {
      return VELAWEAR_ERR_IO;
    }
  if (audio->thread_started || audio->stream_running)
    {
      return VELAWEAR_ERR_BUSY;
    }

  ret = velawear_mic_stream_start();
  if (ret < 0)
    {
      return VELAWEAR_ERR_IO;
    }

  audio->events = events;
  audio->level_active = false;
  audio->latest_avg_abs = 0;
  audio->latest_peak_abs = 0;
  audio->stream_running = true;
  ret = pthread_create(&audio->thread, NULL, audio_sensor_thread, audio);
  if (ret != 0)
    {
      audio->stream_running = false;
      velawear_mic_stream_stop();
      syslog(LOG_ERR, "[AudioStream] level worker start failed: %d\n", ret);
      return VELAWEAR_ERR_IO;
    }

  audio->thread_started = true;
  return VELAWEAR_OK;
}

void audio_sensor_cleanup(velawear_audio_t *audio)
{
  if (audio == NULL)
    {
      return;
    }

  audio->stream_running = false;
  if (audio->thread_started)
    {
      pthread_join(audio->thread, NULL);
      audio->thread_started = false;
    }

  velawear_mic_stream_stop();
  if (audio->fd >= 0)
    {
      close(audio->fd);
      audio->fd = -1;
    }

  audio->events = NULL;
  audio->initialized = false;
  syslog(LOG_INFO, "[Audio] Cleaned up\n");
}
