/*
 * VelaWear Agent - Action Manager Implementation
 *
 * Manages all actions: display, vibrate, BLE, LLM, etc.
 *
 * Author: pjjuihj
 * Team: 329 - dela
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>
#include <pthread.h>
#include <unistd.h>

#include "velawear.h"
#include "action_manager.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void *action_manager_thread(void *arg)
{
  velawear_actions_t *actions = (velawear_actions_t *)arg;
  velawear_action_t action;
  int ret;

  syslog(LOG_INFO, "[ActionMgr] Thread started\n");

  while (actions->running)
    {
      /* Wait for action in queue */

      pthread_mutex_lock(&actions->lock);

      while (actions->queue_count == 0 && actions->running)
        {
          pthread_cond_wait(&actions->cond, &actions->lock);
        }

      if (!actions->running)
        {
          pthread_mutex_unlock(&actions->lock);
          break;
        }

      /* Dequeue action */

      memcpy(&action, &actions->queue[actions->queue_head],
             sizeof(velawear_action_t));
      actions->queue_head =
        (actions->queue_head + 1) % ACTION_QUEUE_SIZE;
      actions->queue_count--;

      pthread_mutex_unlock(&actions->lock);

      /* Dispatch action to handler */

      for (int i = 0; i < actions->handler_count; i++)
        {
          if (actions->handlers[i].action_type == action.type)
            {
              ret = actions->handlers[i].handler(&action,
                                                 actions->handlers[i].context);
              if (ret < 0)
                {
                  syslog(LOG_WARNING,
                         "[ActionMgr] Handler for type %d failed: %d\n",
                         action.type, ret);
                }
              break;
            }
        }
    }

  syslog(LOG_INFO, "[ActionMgr] Thread stopped\n");
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int action_manager_init(velawear_actions_t *actions)
{
  memset(actions, 0, sizeof(velawear_actions_t));

  pthread_mutex_init(&actions->lock, NULL);
  pthread_cond_init(&actions->cond, NULL);

  actions->running = false;
  actions->queue_head = 0;
  actions->queue_tail = 0;
  actions->queue_count = 0;

  syslog(LOG_INFO, "[ActionMgr] Initialized\n");
  return VELAWEAR_OK;
}

int action_manager_start(velawear_actions_t *actions)
{
  int ret;

  if (actions->running)
    {
      return VELAWEAR_ERR_BUSY;
    }

  actions->running = true;

  ret = pthread_create(&actions->thread, NULL, action_manager_thread, actions);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[ActionMgr] Failed to create thread: %d\n", ret);
      actions->running = false;
      return VELAWEAR_ERR_IO;
    }

  syslog(LOG_INFO, "[ActionMgr] Started\n");
  return VELAWEAR_OK;
}

void action_manager_cleanup(velawear_actions_t *actions)
{
  if (actions->running)
    {
      actions->running = false;
      pthread_cond_signal(&actions->cond);
      pthread_join(actions->thread, NULL);
    }

  pthread_mutex_destroy(&actions->lock);
  pthread_cond_destroy(&actions->cond);

  syslog(LOG_INFO, "[ActionMgr] Cleaned up\n");
}

int action_manager_execute(velawear_actions_t *actions,
                           velawear_action_t *action)
{
  pthread_mutex_lock(&actions->lock);

  if (actions->queue_count >= ACTION_QUEUE_SIZE)
    {
      pthread_mutex_unlock(&actions->lock);
      syslog(LOG_WARNING, "[ActionMgr] Queue full, dropping action\n");
      return VELAWEAR_ERR_BUSY;
    }

  /* Enqueue action */

  memcpy(&actions->queue[actions->queue_tail], action,
         sizeof(velawear_action_t));
  actions->queue_tail =
    (actions->queue_tail + 1) % ACTION_QUEUE_SIZE;
  actions->queue_count++;

  pthread_cond_signal(&actions->cond);
  pthread_mutex_unlock(&actions->lock);

  return VELAWEAR_OK;
}

int action_manager_process(velawear_actions_t *actions)
{
  if (actions == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  /* The worker owns action execution.  Wake it after decision evaluation so
   * actions queued by a rule are dispatched without an extra consumer. */
  pthread_mutex_lock(&actions->lock);
  if (!actions->running)
    {
      pthread_mutex_unlock(&actions->lock);
      return VELAWEAR_ERR_BUSY;
    }

  pthread_cond_signal(&actions->cond);
  pthread_mutex_unlock(&actions->lock);
  return VELAWEAR_OK;
}

int action_manager_register_handler(velawear_actions_t *actions,
                                    int action_type,
                                    action_handler_t handler,
                                    void *context)
{
  pthread_mutex_lock(&actions->lock);

  if (actions->handler_count >= ACTION_HANDLERS_MAX)
    {
      pthread_mutex_unlock(&actions->lock);
      return VELAWEAR_ERR_NOMEM;
    }

  actions->handlers[actions->handler_count].action_type = action_type;
  actions->handlers[actions->handler_count].handler = handler;
  actions->handlers[actions->handler_count].context = context;
  actions->handler_count++;

  pthread_mutex_unlock(&actions->lock);

  syslog(LOG_INFO, "[ActionMgr] Registered handler for type %d\n",
         action_type);
  return VELAWEAR_OK;
}
