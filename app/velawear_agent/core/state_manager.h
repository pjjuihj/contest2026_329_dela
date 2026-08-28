/*
 * VelaWear Agent - State Manager
 *
 * Manages device, user, and environment state.
 *
 * Author: pjjuihj
 * Team: 329 - dela
 */

#ifndef __VELAWEAR_STATE_MANAGER_H
#define __VELAWEAR_STATE_MANAGER_H

/****************************************************************************
 * Includes
 ****************************************************************************/

#include <nuttx/config.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Type Definitions
 ****************************************************************************/

/* State change callback */

typedef void (*state_change_cb_t)(velawear_state_t *state, void *context);

/* State manager structure */

typedef struct velawear_state_mgr
{
  velawear_state_t state;
  pthread_mutex_t lock;
  state_change_cb_t callbacks[8];
  void *contexts[8];
  int callback_count;
} velawear_state_mgr_t;

/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

int state_manager_init(velawear_state_mgr_t *mgr);
void state_manager_cleanup(velawear_state_mgr_t *mgr);

velawear_state_t state_manager_get_state(velawear_state_mgr_t *mgr);

void state_manager_update_battery(velawear_state_mgr_t *mgr,
                                  uint32_t level, bool charging);
void state_manager_update_power_mode(velawear_state_mgr_t *mgr, int mode);
void state_manager_update_motion(velawear_state_mgr_t *mgr,
                                 bool moving, bool running);
int state_manager_update_from_event(velawear_state_mgr_t *mgr,
                                     velawear_event_t *event);
void state_manager_update_heart_rate(velawear_state_mgr_t *mgr,
                                     float heart_rate);
void state_manager_update_ble(velawear_state_mgr_t *mgr,
                              bool connected, const char *device_name);
void state_manager_update_runtime(velawear_state_mgr_t *mgr,
                                  uint32_t uptime_seconds,
                                  uint32_t free_memory);
void state_manager_register_callback(velawear_state_mgr_t *mgr,
                                     state_change_cb_t callback,
                                     void *context);

#endif /* __VELAWEAR_STATE_MANAGER_H */
