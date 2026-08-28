/*
 * VelaWear Agent - Event Manager
 *
 * Manages all events from sensors, BLE, touch, and system.
 *
 * Author: pjjuihj
 * Team: 329 - dela
 */

#ifndef __VELAWEAR_EVENT_MANAGER_H
#define __VELAWEAR_EVENT_MANAGER_H

/****************************************************************************
 * Includes
 ****************************************************************************/

#include <nuttx/config.h>
#include <pthread.h>
#include <nuttx/mqueue.h>
#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define EVENT_QUEUE_NAME        "/velawear_events"
#define EVENT_QUEUE_SIZE_MAX    64
#define EVENT_POOL_SIZE         16
#define EVENT_HANDLER_MAX       16

/****************************************************************************
 * Type Definitions
 ****************************************************************************/

/* Event handler callback */

typedef int (*event_handler_t)(velawear_event_t *event, void *context);

/* Event handler registration */

typedef struct event_handler_entry
{
  uint32_t event_type;
  event_handler_t handler;
  void *context;
  int priority;
} event_handler_entry_t;

/* Event manager structure */

typedef struct velawear_events
{
  pthread_t thread;
  bool running;
  /* Keep the queue as a file object so producers running outside the
   * VelaWear task group (for example the Bluetooth system workqueue) can
   * submit events with the internal file_mq_* APIs. */
  struct file queue;
  bool queue_open;
  int queue_size;
  event_handler_entry_t handlers[EVENT_HANDLER_MAX];
  int handler_count;
  pthread_mutex_t lock;
  pthread_mutex_t pool_lock;
  velawear_event_t pool[EVENT_POOL_SIZE];
  bool pool_used[EVENT_POOL_SIZE];
} velawear_events_t;

/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

int event_manager_init(velawear_events_t *events, int queue_size);
int event_manager_start(velawear_events_t *events);
void event_manager_cleanup(velawear_events_t *events);

int event_manager_push(velawear_events_t *events, velawear_event_t *event);
int event_manager_pop(velawear_events_t *events, velawear_event_t *event,
                      int timeout_ms);
int event_manager_dispatch(velawear_events_t *events,
                           velawear_event_t *event);
int event_manager_register_handler(velawear_events_t *events,
                                   uint32_t event_type,
                                   event_handler_t handler,
                                   void *context,
                                   int priority);

#endif /* __VELAWEAR_EVENT_MANAGER_H */
