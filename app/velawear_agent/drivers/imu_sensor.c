/*
 * VelaWear Agent - IMU Sensor Driver Implementation
 *
 * Reads LSM6DSL IMU data for motion detection.
 *
 * Author: pjjuihj
 * Team: 329 - dela
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <math.h>
#include <time.h>

#include "velawear.h"
#include "imu_sensor.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The Huangshan Pi board registers the LSM6DSL as /dev/lsm6dsl0. */
#define IMU_DEVICE_PATH         "/dev/lsm6dsl0"

/* Default thresholds (in m/s^2) */

#define DEFAULT_MOVE_THRESHOLD  0.5f
#define DEFAULT_RUN_THRESHOLD   2.0f
#define DEFAULT_FALL_THRESHOLD  15.0f

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static float calculate_magnitude(float x, float y, float z)
{
  return sqrtf(x * x + y * y + z * z);
}

static void update_motion_state(imu_sensor_t *imu, imu_data_t *data)
{
  float accel_mag = calculate_magnitude(data->accel_x,
                                        data->accel_y,
                                        data->accel_z);
  float gyro_mag = calculate_magnitude(data->gyro_x,
                                       data->gyro_y,
                                       data->gyro_z);

  /* Check for fall detection (sudden high acceleration) */

  if (accel_mag > imu->threshold_fall)
    {
      imu->motion.fall_detected = true;
      syslog(LOG_WARNING, "[IMU] Fall detected! Accel: %.2f\n", accel_mag);
    }

  /* Check for running (high acceleration + gyro) */

  if (accel_mag > imu->threshold_run && gyro_mag > 1.0f)
    {
      imu->motion.is_running = true;
      imu->motion.is_moving = true;
      imu->motion.last_motion_time = time(NULL);
      imu->motion.motion_count++;
    }
  /* Check for moving (moderate acceleration) */

  else if (accel_mag > imu->threshold_move)
    {
      imu->motion.is_moving = true;
      imu->motion.is_running = false;
      imu->motion.last_motion_time = time(NULL);
      imu->motion.motion_count++;
    }
  else
    {
      /* No significant motion */

      if (time(NULL) - imu->motion.last_motion_time > 5)
        {
          imu->motion.is_moving = false;
          imu->motion.is_running = false;
        }
    }

  imu->motion.motion_intensity = accel_mag;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int imu_sensor_init(imu_sensor_t *imu)
{
  memset(imu, 0, sizeof(imu_sensor_t));

  /* Open IMU device */

  imu->fd = open(IMU_DEVICE_PATH, O_RDONLY);
  if (imu->fd < 0)
    {
      syslog(LOG_ERR, "[IMU] Failed to open %s: %d\n",
             IMU_DEVICE_PATH, errno);
      return VELAWEAR_ERR_IO;
    }

  /* Start the sensor before requesting samples.  The LSM6DSL driver exposes
   * data through SNIOC_LSM6DSLSENSORREAD rather than read(). */
  if (ioctl(imu->fd, SNIOC_START, 0) < 0)
    {
      syslog(LOG_ERR, "[IMU] Failed to start %s: %d\n",
             IMU_DEVICE_PATH, errno);
      close(imu->fd);
      imu->fd = -1;
      return VELAWEAR_ERR_IO;
    }

  /* Set default thresholds */

  imu->threshold_move = DEFAULT_MOVE_THRESHOLD;
  imu->threshold_run = DEFAULT_RUN_THRESHOLD;
  imu->threshold_fall = DEFAULT_FALL_THRESHOLD;

  imu->initialized = true;
  imu->buffer_index = 0;

  syslog(LOG_INFO, "[IMU] Initialized\n");
  return VELAWEAR_OK;
}

void imu_sensor_cleanup(imu_sensor_t *imu)
{
  if (imu->fd >= 0)
    {
      close(imu->fd);
    }

  imu->initialized = false;
  syslog(LOG_INFO, "[IMU] Cleaned up\n");
}

int imu_sensor_read(imu_sensor_t *imu, imu_data_t *data)
{
  struct lsm6dsl_sensor_data_s raw;
  int ret;

  if (!imu->initialized)
    {
      return VELAWEAR_ERR_IO;
    }

  ret = ioctl(imu->fd, SNIOC_LSM6DSLSENSORREAD,
              (unsigned long)&raw);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[IMU] Read failed: %d\n", errno);
      return VELAWEAR_ERR_IO;
    }

  /* Convert raw data to float */

  data->accel_x = raw.x_data / 1000.0f;
  data->accel_y = raw.y_data / 1000.0f;
  data->accel_z = raw.z_data / 1000.0f;
  data->gyro_x = raw.g_x_data / 1000.0f;
  data->gyro_y = raw.g_y_data / 1000.0f;
  data->gyro_z = raw.g_z_data / 1000.0f;
  data->timestamp = time(NULL);

  /* Store in buffer */

  imu->buffer[imu->buffer_index] = *data;
  imu->buffer_index = (imu->buffer_index + 1) % IMU_BUFFER_SIZE;

  /* Update motion state */

  update_motion_state(imu, data);

  return VELAWEAR_OK;
}

int imu_sensor_read_buffer(imu_sensor_t *imu, imu_data_t *buffer,
                           int max_count)
{
  int count = 0;

  for (int i = 0; i < max_count; i++)
    {
      if (imu_sensor_read(imu, &buffer[count]) < 0)
        {
          break;
        }

      count++;
    }

  return count;
}

motion_state_t imu_sensor_get_motion_state(imu_sensor_t *imu)
{
  return imu->motion;
}

void imu_sensor_set_thresholds(imu_sensor_t *imu,
                               float move, float run, float fall)
{
  imu->threshold_move = move;
  imu->threshold_run = run;
  imu->threshold_fall = fall;

  syslog(LOG_INFO, "[IMU] Thresholds set: move=%.2f, run=%.2f, fall=%.2f\n",
         move, run, fall);
}
