/*
 * VelaWear Agent - Action Manager
 *
 * Manages all actions: display, vibrate, BLE, LLM, etc.
 *
 * Author: pjjuihj
 * Team: 329 - dela
 */

#ifndef __VELAWEAR_ACTION_MANAGER_H
#define __VELAWEAR_ACTION_MANAGER_H

/****************************************************************************
 * Includes
 ****************************************************************************/

#include <nuttx/config.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ACTION_QUEUE_SIZE       32
#define ACTION_HANDLERS_MAX     8

/****************************************************************************
 * Type Definitions
 ****************************************************************************/

/* Action handler callback */

typedef int (*action_handler_t)(velawear_action_t *action, void *context);

/* Action handler registration */

typedef struct action_handler_entry
{
  int action_type;
  action_handler_t handler;
  void *context;
} action_handler_entry_t;

/* Action manager structure */

typedef struct velawear_actions
{
  pthread_t thread;
  bool running;
  velawear_action_t queue[ACTION_QUEUE_SIZE];
  int queue_head;
  int queue_tail;
  int queue_count;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  action_handler_entry_t handlers[ACTION_HANDLERS_MAX];
  int handler_count;
} velawear_actions_t;

/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

int action_manager_init(velawear_actions_t *actions);
int action_manager_start(velawear_actions_t *actions);
void action_manager_cleanup(velawear_actions_t *actions);

int action_manager_execute(velawear_actions_t *actions,
                           velawear_action_t *action);
int action_manager_process(velawear_actions_t *actions);
int action_manager_register_handler(velawear_actions_t *actions,
                                    int action_type,
                                    action_handler_t handler,
                                    void *context);

#endif /* __VELAWEAR_ACTION_MANAGER_H */
