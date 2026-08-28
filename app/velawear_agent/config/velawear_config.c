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

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int velawear_config_init(velawear_config_t *config)
{
  memset(config, 0, sizeof(velawear_config_t));

  /* Set defaults */

  config->imu_sample_rate_hz = 50;
  config->sedentary_threshold_sec = 3600;
  config->fall_accel_threshold_g = 2.5f;
  config->event_queue_size = 32;
  config->llm_enabled = true;
  config->ble_enabled = true;

  syslog(LOG_INFO, "[Config] Initialized with defaults\n");
  return VELAWEAR_OK;
}

void velawear_config_cleanup(velawear_config_t *config)
{
  syslog(LOG_INFO, "[Config] Cleaned up\n");
}
