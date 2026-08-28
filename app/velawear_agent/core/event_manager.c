/*
 * VelaWear Agent - Event Manager Implementation
 *
 * Manages all events from sensors, BLE, touch, and system.
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
#include <nuttx/mqueue.h>
#include <fcntl.h>
#include <unistd.h>

#include "velawear.h"
#include "event_manager.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void *event_manager_thread(void *arg)
{
  velawear_events_t *events = (velawear_events_t *)arg;
  velawear_event_t event;
  int ret;

  syslog(LOG_INFO, "[EventMgr] Thread started\n");

  while (events->running)
    {
      /* Pop event from queue with timeout */

      ret = event_manager_pop(events, &event, 1000);
      if (ret < 0)
        {
          /* Timeout or error, continue */

          continue;
        }

      /* Dispatch event to registered handlers. */
      ret = event_manager_dispatch(events, &event);
      if (ret < 0)
        {
          syslog(LOG_WARNING, "[EventMgr] Dispatch failed: %d\n", ret);
        }
    }

  syslog(LOG_INFO, "[EventMgr] Thread stopped\n");
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int event_manager_dispatch(velawear_events_t *events,
                           velawear_event_t *event)
{
  event_handler_entry_t handlers[EVENT_HANDLER_MAX];
  int handler_count;

  if (events == NULL || event == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  /* Snapshot registrations before invoking callbacks.  Handlers are
   * external code and may register another handler; never hold the manager
   * lock across that call. */
  pthread_mutex_lock(&events->lock);
  handler_count = events->handler_count;
  memcpy(handlers, events->handlers,
         handler_count * sizeof(event_handler_entry_t));
  pthread_mutex_unlock(&events->lock);

  for (int i = 0; i < handler_count; i++)
    {
      int ret;

      if (handlers[i].event_type == event->type ||
          handlers[i].event_type == VELAWEAR_EVENT_NONE)
        {
          ret = handlers[i].handler(event, handlers[i].context);
          if (ret < 0)
            {
              syslog(LOG_WARNING,
                     "[EventMgr] Handler %d returned error: %d\n",
                     i, ret);
            }
        }
    }

  return VELAWEAR_OK;
}


int event_manager_init(velawear_events_t *events, int queue_size)
{
  struct mq_attr attr;
  int ret;

  if (events == NULL || queue_size < 1 || queue_size > EVENT_QUEUE_SIZE_MAX)
    {
      return VELAWEAR_ERR_INVAL;
    }

  memset(events, 0, sizeof(velawear_events_t));
  events->queue_open = false;
  events->queue_size = queue_size;

  /* Initialize mutex */

  ret = pthread_mutex_init(&events->lock, NULL);
  if (ret != 0)
    {
      return VELAWEAR_ERR_IO;
    }

  ret = pthread_mutex_init(&events->pool_lock, NULL);
  if (ret != 0)
    {
      pthread_mutex_destroy(&events->lock);
      return VELAWEAR_ERR_IO;
    }

  /* Create message queue */

  attr.mq_flags = 0;
  attr.mq_maxmsg = queue_size;
  /* Keep the mqueue payload within CONFIG_MQ_MAXMSGSIZE.  Events are
   * copied through heap-backed pointers so the full event union can
   * remain available to handlers. */
  attr.mq_msgsize = sizeof(uintptr_t);
  attr.mq_curmsgs = 0;

  /* A crashed instance may leave pointer messages behind.  Remove the
   * old queue before creating a fresh one for this process. */
  mq_unlink(EVENT_QUEUE_NAME);

  ret = file_mq_open(&events->queue, EVENT_QUEUE_NAME,
                     O_CREAT | O_RDWR, 0644, &attr);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[EventMgr] Failed to create message queue: %d\n",
             -ret);
      pthread_mutex_destroy(&events->pool_lock);
      pthread_mutex_destroy(&events->lock);
      return VELAWEAR_ERR_IO;
    }

  events->queue_open = true;
  events->running = false;
  events->handler_count = 0;

  syslog(LOG_INFO, "[EventMgr] Initialized queue=%d\n", queue_size);
  return VELAWEAR_OK;
}

int event_manager_start(velawear_events_t *events)
{
  int ret;

  if (events->running)
    {
      return VELAWEAR_ERR_BUSY;
    }

  events->running = true;

  ret = pthread_create(&events->thread, NULL, event_manager_thread, events);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[EventMgr] Failed to create thread: %d\n", ret);
      events->running = false;
      return VELAWEAR_ERR_IO;
    }

  syslog(LOG_INFO, "[EventMgr] Started\n");
  return VELAWEAR_OK;
}

void event_manager_cleanup(velawear_events_t *events)
{
  if (events == NULL)
    {
      return;
    }

  if (events->running)
    {
      events->running = false;
      pthread_join(events->thread, NULL);
    }

  if (events->queue_open)
    {
      file_mq_close(&events->queue);
      mq_unlink(EVENT_QUEUE_NAME);
      events->queue_open = false;
    }

  pthread_mutex_destroy(&events->pool_lock);
  pthread_mutex_destroy(&events->lock);

  syslog(LOG_INFO, "[EventMgr] Cleaned up\n");
}

int event_manager_push(velawear_events_t *events, velawear_event_t *event)
{
  uintptr_t message;
  int slot = -1;
  int ret;

  if (events == NULL || !events->queue_open || event == NULL)
    {
      return VELAWEAR_ERR_IO;
    }

  pthread_mutex_lock(&events->pool_lock);
  /* Keep one pool slot for an Agent command.  Sensor producers must not
   * prevent a connected BLE client from acknowledging or changing state. */
  for (int i = 0; i < EVENT_POOL_SIZE - 1; i++)
    {
      if (!events->pool_used[i])
        {
          slot = i;
          events->pool_used[i] = true;
          memcpy(&events->pool[i], event, sizeof(*event));
          break;
        }
    }

  if (slot < 0 && event->type == VELAWEAR_EVENT_BLE_MSG &&
      !events->pool_used[EVENT_POOL_SIZE - 1])
    {
      slot = EVENT_POOL_SIZE - 1;
      events->pool_used[slot] = true;
      memcpy(&events->pool[slot], event, sizeof(*event));
    }
  pthread_mutex_unlock(&events->pool_lock);

  if (slot < 0)
    {
      return VELAWEAR_ERR_NOMEM;
    }

  /* The queue carries a stable pool slot, not a heap pointer. */
  message = (uintptr_t)(slot + 1);
  ret = file_mq_send(&events->queue, (const char *)&message,
                     sizeof(message), event->priority);
  if (ret < 0)
    {
      pthread_mutex_lock(&events->pool_lock);
      events->pool_used[slot] = false;
      pthread_mutex_unlock(&events->pool_lock);
      syslog(LOG_WARNING, "[EventMgr] Failed to push event: %d\n", -ret);
      return VELAWEAR_ERR_IO;
    }

  return VELAWEAR_OK;
}

int event_manager_pop(velawear_events_t *events, velawear_event_t *event,
                      int timeout_ms)
{
  struct timespec timeout;
  uintptr_t message;
  ssize_t nbytes;
  int slot;

  if (events == NULL || !events->queue_open || event == NULL)
    {
      return VELAWEAR_ERR_IO;
    }

  /* Calculate timeout */
  clock_gettime(CLOCK_REALTIME, &timeout);
  timeout.tv_sec += timeout_ms / 1000;
  timeout.tv_nsec += (timeout_ms % 1000) * 1000000;
  if (timeout.tv_nsec >= 1000000000)
    {
      timeout.tv_sec++;
      timeout.tv_nsec -= 1000000000;
    }

  nbytes = file_mq_timedreceive(&events->queue, (char *)&message,
                                sizeof(message), NULL, &timeout);
  if (nbytes < 0)
    {
      if (nbytes == -ETIMEDOUT)
        {
          return VELAWEAR_ERR_TIMEOUT;
        }

      return VELAWEAR_ERR_IO;
    }

  if (nbytes != sizeof(message) || message == 0 ||
      message > EVENT_POOL_SIZE)
    {
      return VELAWEAR_ERR_IO;
    }

  slot = (int)message - 1;
  pthread_mutex_lock(&events->pool_lock);
  if (!events->pool_used[slot])
    {
      pthread_mutex_unlock(&events->pool_lock);
      return VELAWEAR_ERR_IO;
    }

  memcpy(event, &events->pool[slot], sizeof(*event));
  events->pool_used[slot] = false;
  pthread_mutex_unlock(&events->pool_lock);

  return VELAWEAR_OK;
}

int event_manager_register_handler(velawear_events_t *events,
                                   uint32_t event_type,
                                   event_handler_t handler,
                                   void *context,
                                   int priority)
{
  pthread_mutex_lock(&events->lock);

  if (events->handler_count >= EVENT_HANDLER_MAX)
    {
      pthread_mutex_unlock(&events->lock);
      return VELAWEAR_ERR_NOMEM;
    }

  events->handlers[events->handler_count].event_type = event_type;
  events->handlers[events->handler_count].handler = handler;
  events->handlers[events->handler_count].context = context;
  events->handlers[events->handler_count].priority = priority;
  events->handler_count++;

  pthread_mutex_unlock(&events->lock);

  syslog(LOG_INFO, "[EventMgr] Registered handler for type %lu\n",
         (unsigned long)event_type);
  return VELAWEAR_OK;
}
