/*
 * VelaWear Agent - Configuration
 *
 * Manages agent configuration and runtime parameters.
 *
 * Author: pjjuihj
 * Team: 329 - dela
 */

#include <nuttx/config.h>
#include <string.h>
#include <syslog.h>

#include "velawear.h"

#ifndef CONFIG_VELAWEAR_IMU_SAMPLE_RATE_HZ
#  define CONFIG_VELAWEAR_IMU_SAMPLE_RATE_HZ 50
#endif

#ifndef CONFIG_VELAWEAR_SEDENTARY_THRESHOLD_SEC
#  define CONFIG_VELAWEAR_SEDENTARY_THRESHOLD_SEC 3600
#endif

#ifndef CONFIG_VELAWEAR_FALL_ACCEL_THRESHOLD_G
#  define CONFIG_VELAWEAR_FALL_ACCEL_THRESHOLD_G 25
#endif

#ifndef CONFIG_VELAWEAR_EVENT_QUEUE_SIZE
#  define CONFIG_VELAWEAR_EVENT_QUEUE_SIZE 32
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int velawear_config_init(velawear_config_t *config)
{
  memset(config, 0, sizeof(velawear_config_t));

  /* Load the generated Kconfig values instead of silently shadowing
   * them with demo-only constants.  Keep defensive fallbacks for builds
   * that compile this source outside the full NuttX configuration step. */
  config->imu_sample_rate_hz = CONFIG_VELAWEAR_IMU_SAMPLE_RATE_HZ > 0 ?
                               CONFIG_VELAWEAR_IMU_SAMPLE_RATE_HZ : 50;
  config->sedentary_threshold_sec =
    CONFIG_VELAWEAR_SEDENTARY_THRESHOLD_SEC >= 10 ?
    CONFIG_VELAWEAR_SEDENTARY_THRESHOLD_SEC : 3600;
  config->fall_accel_threshold_g =
    CONFIG_VELAWEAR_FALL_ACCEL_THRESHOLD_G > 0 ?
    (float)CONFIG_VELAWEAR_FALL_ACCEL_THRESHOLD_G / 10.0f : 2.5f;
  config->event_queue_size = CONFIG_VELAWEAR_EVENT_QUEUE_SIZE;
  if (config->event_queue_size < 1)
    {
      config->event_queue_size = 1;
    }
  else if (config->event_queue_size > EVENT_QUEUE_SIZE_MAX)
    {
      config->event_queue_size = EVENT_QUEUE_SIZE_MAX;
    }
  config->llm_enabled = true;
  config->ble_enabled = true;

  syslog(LOG_INFO,
         "[Config] Initialized imu=%dHz sedentary=%ds fall=%.1fg queue=%d\n",
         config->imu_sample_rate_hz, config->sedentary_threshold_sec,
         config->fall_accel_threshold_g, config->event_queue_size);
  return VELAWEAR_OK;
}

void velawear_config_cleanup(velawear_config_t *config)
{
  syslog(LOG_INFO, "[Config] Cleaned up\n");
}
