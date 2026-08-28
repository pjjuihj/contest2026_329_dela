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
#include <mqueue.h>
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


int event_manager_init(velawear_events_t *events)
{
  struct mq_attr attr;
  int ret;

  if (events == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  memset(events, 0, sizeof(velawear_events_t));
  events->queue = (mqd_t)-1;

  /* Initialize mutex */

  ret = pthread_mutex_init(&events->lock, NULL);
  if (ret != 0)
    {
      return VELAWEAR_ERR_IO;
    }

  /* Create message queue */

  attr.mq_flags = 0;
  attr.mq_maxmsg = EVENT_QUEUE_SIZE;
  /* Keep the mqueue payload within CONFIG_MQ_MAXMSGSIZE.  Events are
   * copied through heap-backed pointers so the full event union can
   * remain available to handlers. */
  attr.mq_msgsize = sizeof(uintptr_t);
  attr.mq_curmsgs = 0;

  /* A crashed instance may leave pointer messages behind.  Remove the
   * old queue before creating a fresh one for this process. */
  mq_unlink("/velawear_events");

  events->queue = mq_open("/velawear_events", O_CREAT | O_RDWR, 0644,
                          &attr);
  if (events->queue == (mqd_t)-1)
    {
      syslog(LOG_ERR, "[EventMgr] Failed to create message queue: %d\n",
             errno);
      return VELAWEAR_ERR_IO;
    }

  events->running = false;
  events->handler_count = 0;

  syslog(LOG_INFO, "[EventMgr] Initialized\n");
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

  if (events->queue != (mqd_t)-1)
    {
      mq_close(events->queue);
      mq_unlink("/velawear_events");
    }

  pthread_mutex_destroy(&events->lock);

  syslog(LOG_INFO, "[EventMgr] Cleaned up\n");
}

int event_manager_push(velawear_events_t *events, velawear_event_t *event)
{
  velawear_event_t *copy;
  uintptr_t message;
  int ret;

  if (events->queue == (mqd_t)-1 || event == NULL)
    {
      return VELAWEAR_ERR_IO;
    }

  copy = (velawear_event_t *)malloc(sizeof(*copy));
  if (copy == NULL)
    {
      return VELAWEAR_ERR_NOMEM;
    }

  memcpy(copy, event, sizeof(*copy));
  message = (uintptr_t)copy;
  ret = mq_send(events->queue, (const char *)&message,
                sizeof(message), event->priority);
  if (ret < 0)
    {
      free(copy);
      syslog(LOG_WARNING, "[EventMgr] Failed to push event: %d\n", errno);
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

  if (events->queue == (mqd_t)-1 || event == NULL)
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

  nbytes = mq_timedreceive(events->queue, (char *)&message,
                           sizeof(message), NULL, &timeout);
  if (nbytes < 0)
    {
      if (errno == ETIMEDOUT)
        {
          return VELAWEAR_ERR_TIMEOUT;
        }

      return VELAWEAR_ERR_IO;
    }

  if (nbytes != sizeof(message) || message == 0)
    {
      return VELAWEAR_ERR_IO;
    }

  memcpy(event, (const void *)message, sizeof(*event));
  free((void *)message);
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
