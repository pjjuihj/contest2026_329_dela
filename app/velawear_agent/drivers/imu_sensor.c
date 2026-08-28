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

#define FALL_FREEFALL_THRESHOLD 0.5f
#define FALL_IMPACT_THRESHOLD   8.0f
#define FALL_REST_MIN           0.7f
#define FALL_REST_MAX           1.3f
#define FALL_FREEFALL_WINDOW_MS 500
#define FALL_FREEFALL_MIN_MS    100
#define FALL_IMPACT_WINDOW_MS   2000
#define FALL_CONFIRM_HOLD_MS    2000

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static float calculate_magnitude(float x, float y, float z)
{
  return sqrtf(x * x + y * y + z * z);
}

static uint32_t imu_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return (uint32_t)time(NULL) * 1000;
    }

  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void reset_fall_detector(imu_sensor_t *imu)
{
  imu->fall_detector.stage = FALL_STAGE_NONE;
  imu->fall_detector.stage_start_ms = 0;
  imu->fall_detector.confirmed = false;
}

static void update_fall_detector(imu_sensor_t *imu, float accel_mag)
{
  uint32_t now = imu_now_ms();
  uint32_t elapsed = now - imu->fall_detector.stage_start_ms;

  switch (imu->fall_detector.stage)
    {
      case FALL_STAGE_NONE:
        if (accel_mag < FALL_FREEFALL_THRESHOLD)
          {
            imu->fall_detector.stage = FALL_STAGE_FREEFALL;
            imu->fall_detector.stage_start_ms = now;
            imu->fall_detector.confirmed = false;
          }
        break;

      case FALL_STAGE_FREEFALL:
        if (accel_mag > FALL_IMPACT_THRESHOLD &&
            elapsed >= FALL_FREEFALL_MIN_MS &&
            elapsed <= FALL_FREEFALL_WINDOW_MS)
          {
            imu->fall_detector.stage = FALL_STAGE_IMPACT;
            imu->fall_detector.stage_start_ms = now;
          }
        else if (accel_mag >= FALL_FREEFALL_THRESHOLD ||
                 elapsed > FALL_FREEFALL_WINDOW_MS)
          {
            /* Freefall must remain below 0.5g until the impact sample. */
            reset_fall_detector(imu);
          }
        break;

      case FALL_STAGE_IMPACT:
        if (accel_mag >= FALL_REST_MIN && accel_mag <= FALL_REST_MAX &&
            elapsed <= FALL_IMPACT_WINDOW_MS)
          {
            imu->fall_detector.stage = FALL_STAGE_STILL;
            imu->fall_detector.stage_start_ms = now;
            imu->fall_detector.confirmed = true;
            syslog(LOG_WARNING,
                   "[IMU] Fall confirmed after freefall and impact\n");
          }
        else if (elapsed > FALL_IMPACT_WINDOW_MS)
          {
            reset_fall_detector(imu);
          }
        break;

      case FALL_STAGE_STILL:
        if (elapsed > FALL_CONFIRM_HOLD_MS)
          {
            reset_fall_detector(imu);
          }
        break;

      default:
        reset_fall_detector(imu);
        break;
    }

  imu->motion.fall_detected = imu->fall_detector.confirmed;
}

static void update_motion_state(imu_sensor_t *imu, imu_data_t *data)
{
  float accel_mag = calculate_magnitude(data->accel_x,
                                        data->accel_y,
                                        data->accel_z);
  float gyro_mag = calculate_magnitude(data->gyro_x,
                                       data->gyro_y,
                                       data->gyro_z);

  /* Confirm a fall only after freefall, impact, and post-impact stillness. */

  update_fall_detector(imu, accel_mag);

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
