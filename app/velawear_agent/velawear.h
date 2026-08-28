/*
 * VelaWear Agent - Main Header
 *
 * This header defines the main agent structure and common types.
 *
 * Author: pjjuihj
 * Team: 329 - dela
 */

#ifndef __VELAWEAR_H
#define __VELAWEAR_H

/****************************************************************************
 * Includes
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Version information */

#define VELAWEAR_VERSION_MAJOR  1
#define VELAWEAR_VERSION_MINOR  0
#define VELAWEAR_VERSION_PATCH  0

/* Error codes */

#define VELAWEAR_OK             0
#define VELAWEAR_ERR_NOMEM      (-1)
#define VELAWEAR_ERR_INVAL      (-2)
#define VELAWEAR_ERR_TIMEOUT    (-3)
#define VELAWEAR_ERR_IO         (-4)
#define VELAWEAR_ERR_NOTFOUND   (-5)
#define VELAWEAR_ERR_BUSY       (-6)
#define VELAWEAR_ERR_NOSUPPORT  (-7)

/* Power modes */

#define VELAWEAR_POWER_ACTIVE       0
#define VELAWEAR_POWER_IDLE         1
#define VELAWEAR_POWER_LIGHT_SLEEP  2
#define VELAWEAR_POWER_DEEP_SLEEP   3

/* Runtime degradation modes */
#define VELAWEAR_MODE_ONLINE       0
#define VELAWEAR_MODE_INDEPENDENT  1
#define VELAWEAR_MODE_SURVIVAL     2


/* Event types */

#define VELAWEAR_EVENT_NONE         0
#define VELAWEAR_EVENT_IMU_DATA     1
#define VELAWEAR_EVENT_MOTION       2
#define VELAWEAR_EVENT_FALL         3
#define VELAWEAR_EVENT_AUDIO        4
#define VELAWEAR_EVENT_VOICE_CMD    5
#define VELAWEAR_EVENT_BLE_MSG      6
#define VELAWEAR_EVENT_BLE_STATE    7
#define VELAWEAR_EVENT_TOUCH        8
#define VELAWEAR_EVENT_TIMER        9
#define VELAWEAR_EVENT_USER_ACTION  10
#define VELAWEAR_EVENT_SYSTEM       11

/* Action types */

#define VELAWEAR_ACTION_NONE        0
#define VELAWEAR_ACTION_SHOW_UI     1
#define VELAWEAR_ACTION_VIBRATE     2
#define VELAWEAR_ACTION_PLAY_SOUND  3
#define VELAWEAR_ACTION_SEND_BLE    4
#define VELAWEAR_ACTION_CALL_LLM    5
#define VELAWEAR_ACTION_SET_TIMER   6
#define VELAWEAR_ACTION_LOG         7

/* Priority levels */

#define VELAWEAR_PRIORITY_LOW       0
#define VELAWEAR_PRIORITY_NORMAL    1
#define VELAWEAR_PRIORITY_HIGH      2
#define VELAWEAR_PRIORITY_CRITICAL  3

/****************************************************************************
 * Common Type Definitions
 ****************************************************************************/

/* Event structure */

typedef struct velawear_event
{
  uint32_t type;
  uint32_t priority;
  uint32_t timestamp;
  union
    {
      struct
        {
          float x;
          float y;
          float z;
        } imu;
      struct
        {
          int motion_type;
          float intensity;
        } motion;
      struct
        {
          char text[256];
          int length;
        } audio;
      struct
        {
          char data[512];
          int length;
        } ble;
      struct
        {
          int x;
          int y;
          int gesture;
        } touch;
      struct
        {
          int id;
        } timer;
    } data;
} velawear_event_t;

/* State structure */

typedef struct velawear_state
{
  /* Device state */

  uint32_t battery_level;
  bool is_charging;
  int power_mode;
  uint32_t free_memory;
  uint32_t uptime_seconds;

  /* User state */

  bool is_moving;
  bool is_running;
  bool is_sleeping;
  float heart_rate;
  float temperature;

  /* Environment state */

  bool ble_connected;
  char ble_device_name[64];
  bool has_network;
  int signal_strength;

  /* History */

  uint32_t last_move_time;
  uint32_t last_interaction_time;
  uint32_t fall_count_today;
  uint32_t move_count_today;
} velawear_state_t;

/* Action structure */

typedef struct velawear_action
{
  int type;
  uint32_t priority;
  union
    {
      struct
        {
          char text[256];
        } display;
      struct
        {
          int duration_ms;
          int pattern;
        } vibrate;
      struct
        {
          char data[512];
          int length;
        } ble;
      struct
        {
          char prompt[1024];
        } llm;
    } params;
} velawear_action_t;

/* Configuration structure */

typedef struct velawear_config
{
  int imu_sample_rate_hz;
  int sedentary_threshold_sec;
  float fall_accel_threshold_g;
  int event_queue_size;
  bool llm_enabled;
  bool ble_enabled;
} velawear_config_t;

/* LLM client structure */

typedef struct velawear_llm
{
  bool initialized;
  bool connected;
  char model_name[64];
  int request_count;
} velawear_llm_t;

/* Audio sensor structure */

typedef struct velawear_audio
{
  int fd;
  bool initialized;
  int sample_rate;
} velawear_audio_t;

/****************************************************************************
 * Submodule Headers (must come after common types are defined)
 ****************************************************************************/

#include "core/event_manager.h"
#include "core/action_manager.h"
#include "core/decision_engine.h"
#include "core/state_manager.h"
#include "display_manager.h"
#include "drivers/imu_sensor.h"

/****************************************************************************
 * Main Agent Structure
 ****************************************************************************/

typedef struct velawear_agent
{
  volatile sig_atomic_t running;
  int power_mode;
  int operation_mode;
  bool imu_available;
  bool audio_available;
  bool ble_available;
  velawear_config_t config;
  velawear_state_t state;
  velawear_state_mgr_t state_mgr;
  velawear_display_t display;
  velawear_events_t events;
  velawear_engine_t engine;
  velawear_actions_t actions;
  velawear_llm_t llm;
  imu_sensor_t imu;
  velawear_audio_t audio;
  pthread_t imu_thread;
  bool imu_thread_started;
  int watchdog_fd;
  bool watchdog_active;
  pthread_t watchdog_thread;
  bool watchdog_thread_started;
  volatile uint32_t watchdog_last_main_ms;
} velawear_agent_t;

/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

int velawear_config_init(velawear_config_t *config);
void velawear_config_cleanup(velawear_config_t *config);

int velawear_llm_init(velawear_llm_t *llm);
void velawear_llm_cleanup(velawear_llm_t *llm);

int audio_sensor_init(velawear_audio_t *audio);
void audio_sensor_cleanup(velawear_audio_t *audio);

#endif /* __VELAWEAR_H */
