/*
 * VelaWear Agent - Audio Sensor Driver
 *
 * Manages audio input for voice commands.
 *
 * Author: pjjuihj
 * Team: 329 - dela
 */

#include <nuttx/config.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>

#include "velawear.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define AUDIO_DEVICE_PATH       "/dev/audio/pcm_in0"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int audio_sensor_init(velawear_audio_t *audio)
{
  memset(audio, 0, sizeof(velawear_audio_t));

  audio->fd = -1;
  audio->sample_rate = 16000;
  audio->initialized = false;

  /* Try to open audio device (non-fatal if not available) */

  audio->fd = open(AUDIO_DEVICE_PATH, O_RDONLY);
  if (audio->fd < 0)
    {
      syslog(LOG_WARNING, "[Audio] Cannot open %s: %d (audio disabled)\n",
             AUDIO_DEVICE_PATH, errno);
      return VELAWEAR_ERR_IO;
    }

  audio->initialized = true;

  syslog(LOG_INFO, "[Audio] Initialized at %d Hz\n", audio->sample_rate);
  return VELAWEAR_OK;
}

void audio_sensor_cleanup(velawear_audio_t *audio)
{
  if (audio->fd >= 0)
    {
      close(audio->fd);
      audio->fd = -1;
    }

  audio->initialized = false;
  syslog(LOG_INFO, "[Audio] Cleaned up\n");
}
