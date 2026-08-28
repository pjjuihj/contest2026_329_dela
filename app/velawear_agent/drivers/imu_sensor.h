/*
 * VelaWear Agent - IMU Sensor Driver
 *
 * Reads LSM6DSL IMU data for motion detection.
 *
 * Author: pjjuihj
 * Team: 329 - dela
 */

#ifndef __VELAWEAR_IMU_SENSOR_H
#define __VELAWEAR_IMU_SENSOR_H

/****************************************************************************
 * Includes
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/sensors/lsm6dsl.h>
#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define IMU_SAMPLE_RATE_HZ      50
#define IMU_BUFFER_SIZE         32

/****************************************************************************
 * Type Definitions
 ****************************************************************************/

/* IMU data structure */

typedef struct imu_data
{
  float accel_x;
  float accel_y;
  float accel_z;
  float gyro_x;
  float gyro_y;
  float gyro_z;
  uint32_t timestamp;
} imu_data_t;

/* Three-stage fall detector state. */

typedef enum fall_stage
{
  FALL_STAGE_NONE = 0,
  FALL_STAGE_FREEFALL,
  FALL_STAGE_IMPACT,
  FALL_STAGE_STILL
} fall_stage_t;

typedef struct fall_detector
{
  fall_stage_t stage;
  uint32_t stage_start_ms;
  bool confirmed;
} fall_detector_t;

/* Motion detection state */

typedef struct motion_state
{
  bool is_moving;
  bool is_running;
  bool fall_detected;
  float motion_intensity;
  uint32_t last_motion_time;
  uint32_t motion_count;
} motion_state_t;

/* IMU sensor structure */

typedef struct imu_sensor
{
  int fd;
  bool initialized;
  imu_data_t buffer[IMU_BUFFER_SIZE];
  int buffer_index;
  motion_state_t motion;
  fall_detector_t fall_detector;
  float gravity_x;
  float gravity_y;
  float gravity_z;
  bool gravity_valid;
  float threshold_move;
  float threshold_run;
  float threshold_fall;
} imu_sensor_t;

/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

int imu_sensor_init(imu_sensor_t *imu);
void imu_sensor_cleanup(imu_sensor_t *imu);

int imu_sensor_read(imu_sensor_t *imu, imu_data_t *data);
int imu_sensor_read_buffer(imu_sensor_t *imu, imu_data_t *buffer,
                           int max_count);

motion_state_t imu_sensor_get_motion_state(imu_sensor_t *imu);
void imu_sensor_set_thresholds(imu_sensor_t *imu,
                               float move, float run, float fall);

#endif /* __VELAWEAR_IMU_SENSOR_H */
