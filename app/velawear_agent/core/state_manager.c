/*
 * VelaWear Agent - State Manager Implementation
 *
 * Manages device, user, and environment state.
 *
 * Author: pjjuihj
 * Team: 329 - dela
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <pthread.h>
#include <time.h>
#include <math.h>

#include "velawear.h"
#include "state_manager.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void notify_callbacks(velawear_state_mgr_t *mgr)
{
  for (int i = 0; i < mgr->callback_count; i++)
    {
      if (mgr->callbacks[i])
        {
          mgr->callbacks[i](&mgr->state, mgr->contexts[i]);
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int state_manager_init(velawear_state_mgr_t *mgr)
{
  memset(mgr, 0, sizeof(velawear_state_mgr_t));

  pthread_mutex_init(&mgr->lock, NULL);

  /* Initialize default state */

  mgr->state.battery_level = 100;
  mgr->state.is_charging = false;
  mgr->state.power_mode = VELAWEAR_POWER_IDLE;
  mgr->state.free_memory = 0;
  mgr->state.uptime_seconds = 0;
  mgr->state.is_moving = false;
  mgr->state.is_running = false;
  mgr->state.is_sleeping = false;
  mgr->state.heart_rate = 0.0f;
  mgr->state.temperature = 36.5f;
  mgr->state.ble_connected = false;
  mgr->state.has_network = false;
  mgr->state.signal_strength = 0;
  mgr->state.last_move_time = (uint32_t)time(NULL);
  mgr->state.last_interaction_time = mgr->state.last_move_time;
  mgr->state.fall_count_today = 0;
  mgr->state.move_count_today = 0;

  syslog(LOG_INFO, "[StateMgr] Initialized\n");
  return VELAWEAR_OK;
}

void state_manager_cleanup(velawear_state_mgr_t *mgr)
{
  pthread_mutex_destroy(&mgr->lock);
  syslog(LOG_INFO, "[StateMgr] Cleaned up\n");
}

velawear_state_t state_manager_get_state(velawear_state_mgr_t *mgr)
{
  velawear_state_t state;

  pthread_mutex_lock(&mgr->lock);
  memcpy(&state, &mgr->state, sizeof(velawear_state_t));
  pthread_mutex_unlock(&mgr->lock);

  return state;
}

void state_manager_update_battery(velawear_state_mgr_t *mgr,
                                  uint32_t level, bool charging)
{
  pthread_mutex_lock(&mgr->lock);

  mgr->state.battery_level = level;
  mgr->state.is_charging = charging;

  pthread_mutex_unlock(&mgr->lock);

  notify_callbacks(mgr);
}

void state_manager_update_power_mode(velawear_state_mgr_t *mgr, int mode)
{
  pthread_mutex_lock(&mgr->lock);

  mgr->state.power_mode = mode;

  pthread_mutex_unlock(&mgr->lock);

  notify_callbacks(mgr);
}

void state_manager_update_motion(velawear_state_mgr_t *mgr,
                                 bool moving, bool running)
{
  pthread_mutex_lock(&mgr->lock);

  mgr->state.is_moving = moving;
  mgr->state.is_running = running;

  if (moving)
    {
      mgr->state.last_move_time = time(NULL);
      mgr->state.move_count_today++;
    }

  pthread_mutex_unlock(&mgr->lock);

  notify_callbacks(mgr);
}

int state_manager_update_from_event(velawear_state_mgr_t *mgr,
                                     velawear_event_t *event)
{
  float accel_magnitude;

  if (mgr == NULL || event == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  switch (event->type)
    {
      case VELAWEAR_EVENT_IMU_DATA:
        /* IMU events carry the accelerometer sample.  Keep the state update
         * in the state manager so every event follows one state boundary. */
        accel_magnitude = sqrtf(event->data.imu.x * event->data.imu.x +
                                event->data.imu.y * event->data.imu.y +
                                event->data.imu.z * event->data.imu.z);
        state_manager_update_motion(mgr, accel_magnitude > 0.5f,
                                    accel_magnitude > 2.0f);
        break;

      case VELAWEAR_EVENT_MOTION:
        state_manager_update_motion(mgr,
                                    event->data.motion.motion_type != 0,
                                    event->data.motion.motion_type == 2);
        break;

      case VELAWEAR_EVENT_FALL:
        pthread_mutex_lock(&mgr->lock);
        mgr->state.fall_count_today++;
        pthread_mutex_unlock(&mgr->lock);
        notify_callbacks(mgr);
        break;

      case VELAWEAR_EVENT_BLE_STATE:
        state_manager_update_ble(mgr,
                                 event->data.ble_state.connected,
                                 event->data.ble_state.device_name);
        break;

      default:
        /* Events without state payload still pass through this API. */
        break;
    }

  return VELAWEAR_OK;
}

void state_manager_update_heart_rate(velawear_state_mgr_t *mgr,
                                     float heart_rate)
{
  pthread_mutex_lock(&mgr->lock);

  mgr->state.heart_rate = heart_rate;

  pthread_mutex_unlock(&mgr->lock);

  notify_callbacks(mgr);
}

void state_manager_update_ble(velawear_state_mgr_t *mgr,
                              bool connected, const char *device_name)
{
  pthread_mutex_lock(&mgr->lock);

  mgr->state.ble_connected = connected;
  if (connected && device_name != NULL)
    {
      strncpy(mgr->state.ble_device_name, device_name,
              sizeof(mgr->state.ble_device_name) - 1);
      mgr->state.ble_device_name[sizeof(mgr->state.ble_device_name) - 1] =
        '\0';
    }
  else if (!connected)
    {
      mgr->state.ble_device_name[0] = '\0';
    }

  pthread_mutex_unlock(&mgr->lock);

  notify_callbacks(mgr);
}

void state_manager_update_runtime(velawear_state_mgr_t *mgr,
                                  uint32_t uptime_seconds,
                                  uint32_t free_memory)
{
  if (mgr == NULL)
    {
      return;
    }

  pthread_mutex_lock(&mgr->lock);
  mgr->state.uptime_seconds = uptime_seconds;
  mgr->state.free_memory = free_memory;
  pthread_mutex_unlock(&mgr->lock);
}

void state_manager_register_callback(velawear_state_mgr_t *mgr,
                                     state_change_cb_t callback,
                                     void *context)
{
  pthread_mutex_lock(&mgr->lock);

  if (mgr->callback_count < 8)
    {
      mgr->callbacks[mgr->callback_count] = callback;
      mgr->contexts[mgr->callback_count] = context;
      mgr->callback_count++;
    }

  pthread_mutex_unlock(&mgr->lock);
}
