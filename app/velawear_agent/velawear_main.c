/*
 * VelaWear Agent - Main Entry Point
 *
 * This is the main entry point for the VelaWear Agent application.
 * It initializes all subsystems and starts the main event loop.
 *
 * Author: pjjuihj
 * Team: 329 - dela
 */

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <time.h>

#include <nuttx/timers/watchdog.h>

#include "velawear.h"
#include "audio_hw_test.h"

/* Global agent instance */

static velawear_agent_t g_agent;

/* Forward declarations */

static uint32_t velawear_now_ms(void);
static int velawear_init(velawear_agent_t *agent);
static int velawear_start(velawear_agent_t *agent);
static void velawear_cleanup(velawear_agent_t *agent);
static void velawear_signal_handler(int signo);
static void *velawear_imu_thread(void *context);
static void *velawear_watchdog_thread(void *context);
static int velawear_watchdog_start_software(velawear_agent_t *agent);
static int velawear_watchdog_start(velawear_agent_t *agent);
static void velawear_watchdog_stop(velawear_agent_t *agent);
static void velawear_watchdog_ping(velawear_agent_t *agent);
static int velawear_event_handler(velawear_event_t *event, void *context);
static int velawear_log_action_handler(velawear_action_t *action,
                                       void *context);
static int velawear_vibrate_action_handler(velawear_action_t *action,
                                            void *context);
static int velawear_show_ui_action_handler(velawear_action_t *action,
                                            void *context);
static int velawear_send_ble_action_handler(velawear_action_t *action,
                                             void *context);

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  int ret;
  bool falltest = argc > 1 && strcmp(argv[1], "falltest") == 0;

  if (argc > 1 && strcmp(argv[1], "audio") == 0)
    {
      return velawear_audio_hw_test();
    }

  syslog(LOG_INFO, "[VelaWear] Agent starting...\n");
  if (argc > 1 && strcmp(argv[1], "mic") == 0)
    {
      return velawear_mic_hw_test();
    }

  /* Initialize agent structure */

  memset(&g_agent, 0, sizeof(velawear_agent_t));
  g_agent.running = false;
  g_agent.watchdog_fd = -1;
  g_agent.power_mode = VELAWEAR_POWER_IDLE;

  /* Set up signal handler */

  signal(SIGTERM, velawear_signal_handler);
  signal(SIGINT, velawear_signal_handler);

  /* Initialize subsystems */

  ret = velawear_init(&g_agent);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] Initialization failed: %d\n", ret);
      return EXIT_FAILURE;
    }

  /* Start the agent */

  ret = velawear_start(&g_agent);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] Start failed: %d\n", ret);
      velawear_cleanup(&g_agent);
      return EXIT_FAILURE;
    }

  syslog(LOG_INFO, "[VelaWear] Agent started successfully\n");

  /* Main event loop: block waiting for events.  IMU sampling runs in its
   * producer thread so this task remains the single queue consumer. */
  {
    bool falltest_sent = false;

    while (g_agent.running)
      {
        velawear_event_t event;
        int event_ret;

        if (falltest && !falltest_sent)
          {
            memset(&event, 0, sizeof(event));
            event.type = VELAWEAR_EVENT_FALL;
            event.priority = VELAWEAR_PRIORITY_CRITICAL;
            if (event_manager_push(&g_agent.events, &event) < 0)
              {
                syslog(LOG_ERR,
                       "[VelaWear] Fall self-test event enqueue failed\n");
              }
            else
              {
                syslog(LOG_INFO,
                       "[VelaWear] Fall self-test event queued\n");
              }
            falltest_sent = true;
          }

        event_ret = event_manager_pop(&g_agent.events, &event, 1000);
        if (event_ret == VELAWEAR_OK)
          {
            event_ret = event_manager_dispatch(&g_agent.events, &event);
            if (event_ret < 0)
              {
                syslog(LOG_WARNING,
                       "[VelaWear] Event dispatch failed: %d\n", event_ret);
              }
            else if (falltest && event.type == VELAWEAR_EVENT_FALL)
              {
                /* Give action worker and main-thread display handoff time to run. */
                for (int i = 0; i < 10; i++)
                  {
                    usleep(100000);
                    display_manager_tick(&g_agent.display);
                  }
                g_agent.running = false;
              }
          }
        else if (event_ret != VELAWEAR_ERR_TIMEOUT)
          {
            syslog(LOG_WARNING,
                   "[VelaWear] Event wait failed: %d\n", event_ret);
          }

        /* LVGL initialization and timer handling stay on this task. */
        display_manager_tick(&g_agent.display);
        velawear_watchdog_ping(&g_agent);
      }
  }

  /* Cleanup */

  velawear_cleanup(&g_agent);
  syslog(LOG_INFO, "[VelaWear] Agent stopped\n");

  return EXIT_SUCCESS;
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t velawear_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return (uint32_t)time(NULL) * 1000;
    }

  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void velawear_watchdog_ping(velawear_agent_t *agent)
{
  if (agent == NULL)
    {
      return;
    }

  agent->watchdog_last_main_ms = velawear_now_ms();
  if (agent->watchdog_active && agent->watchdog_fd >= 0)
    {
      if (ioctl(agent->watchdog_fd, WDIOC_KEEPALIVE, 0) < 0)
        {
          syslog(LOG_WARNING, "[Watchdog] Keepalive failed: %d\n", errno);
        }
    }
}

static void *velawear_watchdog_thread(void *context)
{
  velawear_agent_t *agent = (velawear_agent_t *)context;
  bool reported_stall = false;

  syslog(LOG_INFO, "[Watchdog] Software monitor started\n");
  while (agent->running)
    {
      uint32_t age = velawear_now_ms() - agent->watchdog_last_main_ms;

      if (age > 5000 && !reported_stall)
        {
          syslog(LOG_ERR,
                 "[Watchdog] Main loop heartbeat stalled (%lu ms)\n",
                 (unsigned long)age);
          reported_stall = true;
        }
      else if (age <= 5000)
        {
          reported_stall = false;
        }

      usleep(1000000);
    }

  syslog(LOG_INFO, "[Watchdog] Software monitor stopped\n");
  return NULL;
}

static int velawear_watchdog_start_software(velawear_agent_t *agent)
{
  int ret;

  agent->watchdog_last_main_ms = velawear_now_ms();
  ret = pthread_create(&agent->watchdog_thread, NULL,
                       velawear_watchdog_thread, agent);
  if (ret != 0)
    {
      syslog(LOG_WARNING, "[Watchdog] Software monitor start failed: %d\n",
             ret);
      return ret;
    }

  agent->watchdog_thread_started = true;
  return VELAWEAR_OK;
}

static int velawear_watchdog_start(velawear_agent_t *agent)
{
  int timeout_ms = 5000;
  int ret;

  if (agent == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  agent->watchdog_fd = open("/dev/watchdog0", O_RDWR);
  if (agent->watchdog_fd < 0)
    {
      syslog(LOG_WARNING, "[Watchdog] /dev/watchdog0 unavailable: %d\n",
             errno);
      ret = velawear_watchdog_start_software(agent);
      if (ret == VELAWEAR_OK)
        {
          syslog(LOG_INFO, "[Watchdog] Software-only monitor enabled\n");
        }
      return VELAWEAR_OK;
    }

  if (ioctl(agent->watchdog_fd, WDIOC_SETTIMEOUT, timeout_ms) < 0 ||
      ioctl(agent->watchdog_fd, WDIOC_START, 0) < 0)
    {
      syslog(LOG_WARNING, "[Watchdog] Hardware watchdog start failed: %d\n",
             errno);
      close(agent->watchdog_fd);
      agent->watchdog_fd = -1;
      ret = velawear_watchdog_start_software(agent);
      if (ret == VELAWEAR_OK)
        {
          syslog(LOG_INFO, "[Watchdog] Software-only monitor enabled\n");
        }
      return VELAWEAR_OK;
    }

  agent->watchdog_active = true;
  ret = velawear_watchdog_start_software(agent);
  if (ret != VELAWEAR_OK)
    {
      ioctl(agent->watchdog_fd, WDIOC_STOP, 0);
      close(agent->watchdog_fd);
      agent->watchdog_fd = -1;
      agent->watchdog_active = false;
      return VELAWEAR_OK;
    }

  syslog(LOG_INFO, "[Watchdog] Hardware watchdog started (%d ms)\n",
         timeout_ms);
  return VELAWEAR_OK;
}

static void velawear_watchdog_stop(velawear_agent_t *agent)
{
  if (agent == NULL)
    {
      return;
    }

  if (agent->watchdog_thread_started)
    {
      pthread_join(agent->watchdog_thread, NULL);
      agent->watchdog_thread_started = false;
    }

  if (agent->watchdog_active && agent->watchdog_fd >= 0)
    {
      ioctl(agent->watchdog_fd, WDIOC_STOP, 0);
      close(agent->watchdog_fd);
    }

  agent->watchdog_fd = -1;
  agent->watchdog_active = false;
}

static void *velawear_imu_thread(void *context)
{
  velawear_agent_t *agent = (velawear_agent_t *)context;
  bool fall_reported = false;

  while (agent->running)
    {
      imu_data_t sample;
      motion_state_t motion;
      velawear_event_t event;
      int period_us = 20000;

      if (imu_sensor_read(&agent->imu, &sample) == VELAWEAR_OK)
        {
          memset(&event, 0, sizeof(event));
          event.type = VELAWEAR_EVENT_IMU_DATA;
          event.priority = VELAWEAR_PRIORITY_NORMAL;
          event.timestamp = sample.timestamp;
          event.data.imu.x = sample.accel_x;
          event.data.imu.y = sample.accel_y;
          event.data.imu.z = sample.accel_z;
          if (event_manager_push(&agent->events, &event) < 0)
            {
              syslog(LOG_WARNING,
                     "[VelaWear] Failed to queue IMU event\n");
            }

          motion = imu_sensor_get_motion_state(&agent->imu);
          if (motion.fall_detected && !fall_reported)
            {
              event.type = VELAWEAR_EVENT_FALL;
              event.priority = VELAWEAR_PRIORITY_CRITICAL;
              if (event_manager_push(&agent->events, &event) < 0)
                {
                  syslog(LOG_WARNING,
                         "[VelaWear] Failed to queue fall event\n");
                }
              fall_reported = true;
            }
          else if (!motion.fall_detected)
            {
              fall_reported = false;
            }
        }

      if (agent->config.imu_sample_rate_hz > 0)
        {
          period_us = 1000000 / agent->config.imu_sample_rate_hz;
          if (period_us < 1000)
            {
              period_us = 1000;
            }
        }

      usleep(period_us);
    }

  return NULL;
}

static int velawear_init(velawear_agent_t *agent)
{
  int ret;

  /* Initialize configuration */

  ret = velawear_config_init(&agent->config);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] Config init failed: %d\n", ret);
      return ret;
    }

  /* Initialize event manager */

  ret = event_manager_init(&agent->events);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] Event manager init failed: %d\n", ret);
      return ret;
    }

  /* Initialize state manager */

  ret = state_manager_init(&agent->state_mgr);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] State manager init failed: %d\n", ret);
      return ret;
    }
  agent->state = state_manager_get_state(&agent->state_mgr);

  /* Initialize the LCD/LVGL status page on the main thread.  LVGL keeps
   * context in task-local storage, so timer handling stays in this thread. */

  ret = display_manager_init(&agent->display, &agent->state_mgr,
                             &agent->events);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[VelaWear] Display init failed: %d\n", ret);
      /* Non-fatal: the agent remains usable without a display. */
    }

  /* Initialize decision engine */

  ret = decision_engine_init(&agent->engine, &agent->config);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] Decision engine init failed: %d\n", ret);
      return ret;
    }

  /* Initialize action manager */

  ret = action_manager_init(&agent->actions);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] Action manager init failed: %d\n", ret);
      return ret;
    }

  ret = decision_engine_set_action_manager(&agent->engine,
                                             &agent->actions);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] Decision action routing setup failed: %d\n",
             ret);
      return ret;
    }

  /* Initialize sensor drivers */

  ret = imu_sensor_init(&agent->imu);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[VelaWear] IMU sensor init failed: %d\n", ret);
      /* Non-fatal: continue without IMU */
    }

  ret = audio_sensor_init(&agent->audio);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[VelaWear] Audio sensor init failed: %d\n", ret);
      /* Non-fatal: continue without audio */
    }

  /* Initialize LLM client */

  ret = velawear_llm_init(&agent->llm);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[VelaWear] LLM init failed: %d\n", ret);
      /* Non-fatal: continue without LLM */
    }

  ret = event_manager_register_handler(&agent->events,
                                       VELAWEAR_EVENT_NONE,
                                       velawear_event_handler,
                                       agent,
                                       VELAWEAR_PRIORITY_NORMAL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] Event handler registration failed: %d\n",
             ret);
      return ret;
    }

  ret = action_manager_register_handler(&agent->actions,
                                        VELAWEAR_ACTION_LOG,
                                        velawear_log_action_handler,
                                        agent);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] Action handler registration failed: %d\n",
             ret);
      return ret;
    }

  ret = action_manager_register_handler(&agent->actions,
                                        VELAWEAR_ACTION_VIBRATE,
                                        velawear_vibrate_action_handler,
                                        agent);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] Vibrate handler registration failed: %d\n",
             ret);
      return ret;
    }

  ret = action_manager_register_handler(&agent->actions,
                                        VELAWEAR_ACTION_SHOW_UI,
                                        velawear_show_ui_action_handler,
                                        agent);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] Show UI handler registration failed: %d\n",
             ret);
      return ret;
    }

  ret = action_manager_register_handler(&agent->actions,
                                        VELAWEAR_ACTION_SEND_BLE,
                                        velawear_send_ble_action_handler,
                                        agent);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] BLE handler registration failed: %d\n",
             ret);
      return ret;
    }

  return VELAWEAR_OK;
}

static int velawear_start(velawear_agent_t *agent)
{
  int ret;

  /* The main loop owns event queue consumption.  Do not start a second
   * consumer thread for the same mqueue. */

  /* Start action manager thread. */

  ret = action_manager_start(&agent->actions);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] Action manager start failed: %d\n", ret);
      return ret;
    }

  /* Decision rules are evaluated synchronously for each event. */

  /* Mark agent as running before starting the sensor producer. */

  agent->running = true;
  ret = velawear_watchdog_start(agent);
  if (ret < 0)
    {
      agent->running = false;
      return ret;
    }

  if (agent->imu.initialized)
    {
      ret = pthread_create(&agent->imu_thread, NULL,
                           velawear_imu_thread, agent);
      if (ret != 0)
        {
          agent->running = false;
          syslog(LOG_ERR, "[VelaWear] IMU thread start failed: %d\n", ret);
          return VELAWEAR_ERR_IO;
        }
      agent->imu_thread_started = true;
    }

  return VELAWEAR_OK;
}

static void velawear_cleanup(velawear_agent_t *agent)
{
  /* Stop subsystems in reverse order */

  agent->running = false;
  velawear_watchdog_stop(agent);
  if (agent->imu_thread_started)
    {
      pthread_join(agent->imu_thread, NULL);
      agent->imu_thread_started = false;
    }

  velawear_llm_cleanup(&agent->llm);
  audio_sensor_cleanup(&agent->audio);
  imu_sensor_cleanup(&agent->imu);
  action_manager_cleanup(&agent->actions);
  event_manager_cleanup(&agent->events);
  decision_engine_cleanup(&agent->engine);
  display_manager_cleanup(&agent->display);
  state_manager_cleanup(&agent->state_mgr);
  velawear_config_cleanup(&agent->config);
}

static void velawear_signal_handler(int signo)
{
  syslog(LOG_INFO, "[VelaWear] Received signal %d, shutting down\n", signo);
  g_agent.running = false;
}

static int velawear_event_handler(velawear_event_t *event, void *context)
{
  velawear_agent_t *agent = (velawear_agent_t *)context;
  velawear_state_t state;
  velawear_action_t action;
  int rule_id;
  int ret;

  ret = state_manager_update_from_event(&agent->state_mgr, event);
  if (ret < 0)
    {
      return ret;
    }

  state = state_manager_get_state(&agent->state_mgr);
  agent->state = state;
  rule_id = decision_engine_evaluate(&agent->engine, &state, event);

  /* Every matched rule produces a traceable action.  Rule-specific handlers
   * can replace this log action later without changing the event pipeline. */
  if (rule_id >= 0 && event->type != VELAWEAR_EVENT_IMU_DATA &&
      event->type != VELAWEAR_EVENT_FALL)
    {
      memset(&action, 0, sizeof(action));
      action.type = VELAWEAR_ACTION_LOG;
      action.priority = event->priority;
      ret = action_manager_execute(&agent->actions, &action);
      if (ret < 0)
        {
          return ret;
        }
    }

  /* The action manager worker performs callbacks; wake it after evaluation. */
  return action_manager_process(&agent->actions);
}

static int velawear_log_action_handler(velawear_action_t *action,
                                       void *context)
{
  if (action)
    {
      syslog(LOG_INFO, "[VelaWear] action %d executed\n", action->type);
    }

  return VELAWEAR_OK;
}

static int velawear_vibrate_action_handler(velawear_action_t *action,
                                            void *context)
{
  if (action == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  /* Hardware PWM/GPIO wiring is board-specific; keep the action observable
   * until the motor pin is assigned in the board configuration. */
  syslog(LOG_INFO, "[Action] Vibrate: duration=%dms, pattern=%d\n",
         action->params.vibrate.duration_ms,
         action->params.vibrate.pattern);
  return VELAWEAR_OK;
}

static int velawear_show_ui_action_handler(velawear_action_t *action,
                                            void *context)
{
  velawear_agent_t *agent = (velawear_agent_t *)context;

  if (action == NULL || agent == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  display_manager_show_alert(&agent->display, action->params.display.text);
  syslog(LOG_WARNING, "[Action] Emergency UI alert shown\n");
  return VELAWEAR_OK;
}

static int velawear_send_ble_action_handler(velawear_action_t *action,
                                             void *context)
{
  velawear_agent_t *agent = (velawear_agent_t *)context;
  velawear_state_t state;

  if (action == NULL || agent == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  state = state_manager_get_state(&agent->state_mgr);
  if (!state.ble_connected)
    {
      syslog(LOG_WARNING,
             "[Action] BLE emergency notification deferred (not connected)\n");
      return VELAWEAR_OK;
    }

  /* The BLE transport is not integrated yet; retain a deterministic log. */
  syslog(LOG_WARNING, "[Action] BLE emergency notification: %s\n",
         action->params.ble.data);
  return VELAWEAR_OK;
}
