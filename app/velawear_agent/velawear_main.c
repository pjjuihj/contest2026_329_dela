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
#include <malloc.h>

#include <nuttx/timers/watchdog.h>
#include <nuttx/input/buttons.h>

#include "velawear.h"
#include "audio_hw_test.h"
#include "drivers/velawear_ble.h"
#include "drivers/velawear_agent_protocol.h"
#include "skills/velawear_skill.h"
#ifdef CONFIG_VELAWEAR_XIAOZHI_PAN
#  include "drivers/velawear_pan.h"
#endif
#ifdef CONFIG_VELAWEAR_XIAOZHI
#  include "xiaozhi/xiaozhi.h"
#endif




/* Global agent instance */

static velawear_agent_t g_agent;
static int g_buttons_fd = -1;
static bool g_key2_down;
static uint32_t g_key2_down_ms;

/* Forward declarations */

static uint32_t velawear_now_ms(void);
static uint32_t velawear_free_memory(void);
static void velawear_poll_timer(velawear_agent_t *agent);
static void velawear_buttons_open(void);
static void velawear_buttons_poll(velawear_display_t *display,
                                  uint32_t *last_activity_ms);
static void velawear_buttons_close(void);
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
static void velawear_set_degradation_mode(velawear_agent_t *agent,
                                             int mode,
                                             const char *reason);
static int velawear_retry_imu(velawear_agent_t *agent);
static int velawear_retry_audio(velawear_agent_t *agent);
static void velawear_set_power_mode(velawear_agent_t *agent,
                                    int mode,
                                    const char *reason);
static int velawear_event_handler(velawear_event_t *event, void *context);
static int velawear_log_action_handler(velawear_action_t *action,
                                       void *context);
static int velawear_vibrate_action_handler(velawear_action_t *action,
                                            void *context);
static int velawear_play_sound_action_handler(velawear_action_t *action,
                                               void *context);
static int velawear_play_voice_action_handler(velawear_action_t *action,
                                               void *context);
static int velawear_show_ui_action_handler(velawear_action_t *action,
                                            void *context);
static int velawear_send_ble_action_handler(velawear_action_t *action,
                                             void *context);
static int velawear_call_llm_action_handler(velawear_action_t *action,
                                             void *context);
static int velawear_set_timer_action_handler(velawear_action_t *action,
                                               void *context);
static int velawear_handle_agent_command(velawear_agent_t *agent,
                                         const velawear_event_t *event);
static void velawear_publish_agent_event(velawear_agent_t *agent,
                                         const velawear_event_t *event);

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

  if (argc > 1 && strcmp(argv[1], "beep") == 0)
    {
      return velawear_speaker_alert(500, 0);
    }

  if (argc > 1 && strcmp(argv[1], "music") == 0)
    {
      return velawear_music_hw_test();
    }

#ifdef CONFIG_VELAWEAR_XIAOZHI
  if (argc > 1 && strcmp(argv[1], "xiaozhi") == 0)
    {
      return velawear_xiaozhi_run_cli(argc - 2, argv + 2);
    }
#endif

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
  g_agent.operation_mode = VELAWEAR_MODE_ONLINE;
  g_agent.imu_available = false;
  g_agent.audio_available = false;
  g_agent.ble_available = false;

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
    bool skill_installed = false;
    uint32_t last_activity_ms = velawear_now_ms();
    uint32_t runtime_update_ms = 0;
    uint32_t skill_retry_ms = 0;

    while (g_agent.running)
      {
        velawear_event_t event;
        int event_ret;
        int wait_ms;
        uint32_t now_ms;

        now_ms = velawear_now_ms();

        /* Install the team Skill after the official Agent has created /data.
         * Retry without blocking the sensor/event loop when the mount is not
         * ready yet. */
        if (!skill_installed &&
            (skill_retry_ms == 0 ||
             (int32_t)(now_ms - skill_retry_ms) >= 0))
          {
            if (velawear_skill_install() == 0)
              {
                skill_installed = true;
              }
            else
              {
                skill_retry_ms = now_ms + 1000;
              }
          }

        velawear_poll_timer(&g_agent);
        if (runtime_update_ms == 0 ||
            (uint32_t)(now_ms - runtime_update_ms) >= 1000)
          {
            state_manager_update_runtime(&g_agent.state_mgr,
                                          (now_ms - g_agent.started_ms) / 1000,
                                          velawear_free_memory());
            runtime_update_ms = now_ms;
          }

        if (g_agent.power_mode == VELAWEAR_POWER_DEEP_SLEEP)
          {
            wait_ms = 100;
          }
        else if (g_agent.power_mode == VELAWEAR_POWER_LIGHT_SLEEP)
          {
            wait_ms = 100;
          }
        else if (g_agent.power_mode == VELAWEAR_POWER_IDLE)
          {
            wait_ms = 100;
          }
        else
          {
            wait_ms = 100;
          }

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

        event_ret = event_manager_pop(&g_agent.events, &event, wait_ms);
        if (event_ret == VELAWEAR_OK)
          {
            if (event.type != VELAWEAR_EVENT_MOTION)
              {
                last_activity_ms = velawear_now_ms();
                velawear_set_power_mode(&g_agent, VELAWEAR_POWER_ACTIVE,
                                        "event received");
              }
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

        velawear_buttons_poll(&g_agent.display, &last_activity_ms);

        {
          uint32_t idle_ms = velawear_now_ms() - last_activity_ms;

          if (display_manager_is_busy(&g_agent.display))
            {
              /* Keep interactive apps and active tests awake. */
              last_activity_ms = velawear_now_ms();
            }
          else if (g_agent.ble_available)
            {
              /* Keep the BLE MVP responsive until low-power policy is verified. */
              velawear_set_power_mode(&g_agent, VELAWEAR_POWER_ACTIVE,
                                      "BLE available");
            }
          else if (idle_ms >= 60000)
            {
              velawear_set_power_mode(&g_agent, VELAWEAR_POWER_DEEP_SLEEP,
                                      "60 seconds without user event");
            }
          else if (idle_ms >= 30000)
            {
              velawear_set_power_mode(&g_agent, VELAWEAR_POWER_LIGHT_SLEEP,
                                      "30 seconds without user event");
            }
          else if (idle_ms >= 10000)
            {
              velawear_set_power_mode(&g_agent, VELAWEAR_POWER_IDLE,
                                      "10 seconds without user event");
            }
        }

        /* Keep LVGL timers and touch input alive so sleep can be woken. */
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

static uint32_t velawear_free_memory(void)
{
  struct mallinfo info = mallinfo();

  return info.fordblks > 0 ? (uint32_t)info.fordblks : 0;
}

static void velawear_poll_timer(velawear_agent_t *agent)
{
  velawear_event_t event;
  uint32_t now;
  int timer_id = 0;
  bool expired = false;

  if (agent == NULL || !agent->timer_lock_initialized)
    {
      return;
    }

  now = velawear_now_ms();
  pthread_mutex_lock(&agent->timer_lock);
  if (agent->timer_active &&
      (int32_t)(now - agent->timer_deadline_ms) >= 0)
    {
      timer_id = agent->timer_id;
      agent->timer_active = false;
      expired = true;
    }
  pthread_mutex_unlock(&agent->timer_lock);

  if (!expired)
    {
      return;
    }

  memset(&event, 0, sizeof(event));
  event.type = VELAWEAR_EVENT_TIMER;
  event.priority = VELAWEAR_PRIORITY_NORMAL;
  event.timestamp = now;
  event.data.timer.id = timer_id;
  if (event_manager_push(&agent->events, &event) < 0)
    {
      syslog(LOG_WARNING, "[Timer] Failed to queue timer id=%d\n", timer_id);
    }
  else
    {
      syslog(LOG_INFO, "[Timer] Fired id=%d\n", timer_id);
    }
}

#define VELAWEAR_KEY2_BIT (1u << 0)

static void velawear_buttons_open(void)
{
  g_buttons_fd = open("/dev/buttons", O_RDONLY | O_NONBLOCK);
  if (g_buttons_fd < 0)
    {
      syslog(LOG_WARNING, "[Buttons] /dev/buttons unavailable: %d\n", errno);
      return;
    }

  syslog(LOG_INFO, "[Buttons] KEY2 input enabled\n");
}

static void velawear_buttons_poll(velawear_display_t *display,
                                  uint32_t *last_activity_ms)
{
  btn_buttonset_t sample;
  ssize_t nbytes;
  bool down;
  uint32_t now;
  uint32_t held_ms;

  if (g_buttons_fd < 0)
    {
      return;
    }

  do
    {
      nbytes = read(g_buttons_fd, &sample, sizeof(sample));
    }
  while (nbytes < 0 && errno == EINTR);

  if (nbytes != (ssize_t)sizeof(sample))
    {
      return;
    }

  down = (sample & VELAWEAR_KEY2_BIT) != 0;
  now = velawear_now_ms();
  if (down && !g_key2_down)
    {
      g_key2_down = true;
      g_key2_down_ms = now;
      if (last_activity_ms != NULL)
        {
          *last_activity_ms = now;
        }
#ifdef CONFIG_VELAWEAR_XIAOZHI
      if (velawear_xiaozhi_is_connected())
        {
          syslog(LOG_INFO, "[Buttons] KEY2 pressed: XiaoZhi listen start\n");
          (void)velawear_xiaozhi_button_down();
          return;
        }
#endif
      return;
    }

  if (!down && g_key2_down)
    {
      g_key2_down = false;
      held_ms = now - g_key2_down_ms;
      if (last_activity_ms != NULL)
        {
          *last_activity_ms = now;
        }

#ifdef CONFIG_VELAWEAR_XIAOZHI
      if (velawear_xiaozhi_is_listening())
        {
          syslog(LOG_INFO, "[Buttons] KEY2 released: XiaoZhi listen stop "
                 "held=%lu ms\n", (unsigned long)held_ms);
          (void)velawear_xiaozhi_button_up();
          return;
        }
#endif
      if (display != NULL && display->initialized)
        {
          if (held_ms >= 1000 || display->detail_active ||
              display->page_index == VELAWEAR_PAGE_APPS)
            {
              display_manager_close_app(display);
              display_manager_set_page(display, VELAWEAR_PAGE_WATCHFACE);
            }
          else
            {
              display_manager_set_page(display, VELAWEAR_PAGE_WATCHFACE);
            }
          syslog(LOG_INFO, "[Buttons] KEY2 released held=%lu ms\n",
                 (unsigned long)held_ms);
        }
    }
}

static void velawear_buttons_close(void)
{
  if (g_buttons_fd >= 0)
    {
      close(g_buttons_fd);
      g_buttons_fd = -1;
    }
  g_key2_down = false;
  g_key2_down_ms = 0;
}

static void velawear_set_degradation_mode(velawear_agent_t *agent,
                                             int mode,
                                             const char *reason)
{
  const char *name;

  if (agent == NULL || agent->operation_mode == mode)
    {
      return;
    }

  agent->operation_mode = mode;
  if (mode == VELAWEAR_MODE_ONLINE)
    {
      name = "online";
    }
  else if (mode == VELAWEAR_MODE_INDEPENDENT)
    {
      name = "independent";
    }
  else
    {
      name = "survival";
    }

  syslog(LOG_WARNING, "[Degrade] mode=%s reason=%s\n",
         name, reason ? reason : "unspecified");
}

static int velawear_retry_imu(velawear_agent_t *agent)
{
  int ret;

  ret = imu_sensor_init(&agent->imu);
  if (ret < 0)
    {
      usleep(20000);
      ret = imu_sensor_init(&agent->imu);
    }

  agent->imu_available = ret == VELAWEAR_OK;
  return ret;
}

static int velawear_retry_audio(velawear_agent_t *agent)
{
  int ret;

  ret = audio_sensor_init(&agent->audio);
  if (ret < 0)
    {
      usleep(20000);
      ret = audio_sensor_init(&agent->audio);
    }

  agent->audio_available = ret == VELAWEAR_OK;
  return ret;
}

static void velawear_set_power_mode(velawear_agent_t *agent,
                                    int mode,
                                    const char *reason)
{
  if (agent == NULL || agent->power_mode == mode)
    {
      return;
    }

  agent->power_mode = mode;
  state_manager_update_power_mode(&agent->state_mgr, mode);
  syslog(LOG_INFO, "[Power] mode=%d reason=%s\n",
         mode, reason ? reason : "policy");
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
  int last_motion_type = -1;

  while (agent->running)
    {
      imu_data_t sample;
      motion_state_t motion;
      velawear_event_t event;
      int period_us = 20000;

      if (imu_sensor_read(&agent->imu, &sample) == VELAWEAR_OK)
        {
          int motion_type;

          motion = imu_sensor_get_motion_state(&agent->imu);
          motion_type = motion.is_running ? 2 :
                        (motion.is_moving ? 1 : 0);

          memset(&event, 0, sizeof(event));
          event.type = VELAWEAR_EVENT_MOTION;
          event.priority = VELAWEAR_PRIORITY_NORMAL;
          event.timestamp = sample.timestamp;
          event.data.motion.motion_type = motion_type;
          event.data.motion.intensity = motion.motion_intensity;

          if (motion_type != last_motion_type)
            {
              const char *motion_name = motion_type == 2 ? "跑步" :
                                        (motion_type == 1 ? "活动" : "静止");
              syslog(LOG_INFO, "[MVP] IMU state=%s intensity=%.2f\n",
                     motion_name, motion.motion_intensity);
              velawear_ble_update_motion(motion_type,
                                         motion.motion_intensity);
              /* 活动恢复时清除 BLE 告警状态。 */
              if (last_motion_type == 0 && motion_type != 0)
                {
                  velawear_ble_set_alert(false);
                }
              last_motion_type = motion_type;

              /* Motion state is edge-triggered for the event queue.  The IMU
               * still samples at its configured rate for fall detection, but
               * repeated identical states must not consume all MQ objects. */
              if (event_manager_push(&agent->events, &event) < 0)
                {
                  syslog(LOG_WARNING,
                         "[VelaWear] Failed to queue motion event\n");
                }
            }

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

  ret = event_manager_init(&agent->events, agent->config.event_queue_size);
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
#ifndef VELAWEAR_ENABLE_DISPLAY
#define VELAWEAR_ENABLE_DISPLAY 1
#endif
#if VELAWEAR_ENABLE_DISPLAY
  ret = display_manager_init(&agent->display, &agent->state_mgr,
                             &agent->events);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[VelaWear] Display init failed: %d\n", ret);
      /* Non-fatal: the agent remains usable without a display. */
    }
#else
  ret = -ENOTSUP;
  syslog(LOG_INFO, "[Display] disabled for BLE isolation\n");
#endif

  /*
   * Core MVP mode keeps BLE optional so a controller/HCI fault cannot block
   * the IMU -> Agent -> LCD reminder loop. Re-enable with
   * -DVELAWEAR_ENABLE_BLE=1 after the HCI path is verified.
   */
#ifndef VELAWEAR_ENABLE_BLE
#define VELAWEAR_ENABLE_BLE 0
#endif
#if VELAWEAR_ENABLE_BLE
  ret = velawear_ble_init(&agent->config, &agent->events);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[VelaWear] BLE init failed: %d\n", ret);
    }
  else
    {
      agent->ble_available = true;
    }
  syslog(LOG_INFO, "[BLE] init result=%d available=%d\n", ret,
         agent->ble_available ? 1 : 0);
#ifdef CONFIG_VELAWEAR_XIAOZHI_PAN
  if (agent->ble_available)
    {
      int pan_ret = velawear_pan_init();
      if (pan_ret < 0)
        {
          syslog(LOG_WARNING, "[VelaWear] PAN init failed: %d\n", pan_ret);
        }
    }
#endif
#else
  ret = -ENOTSUP;
  syslog(LOG_INFO, "[BLE] deferred; core MVP mode\n");
#endif

  velawear_buttons_open();

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

  ret = decision_engine_set_state_manager(&agent->engine,
                                          &agent->state_mgr);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] Decision state routing setup failed: %d\n",
             ret);
      return ret;
    }

  ret = decision_engine_set_event_manager(&agent->engine,
                                          &agent->events);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] Decision event routing setup failed: %d\n",
             ret);
      return ret;
    }

  /* Initialize sensor drivers */

  ret = velawear_retry_imu(agent);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[VelaWear] IMU sensor init failed after retry: %d\n",
             ret);
      velawear_set_degradation_mode(agent, VELAWEAR_MODE_INDEPENDENT,
                                     "imu unavailable");
    }

  ret = velawear_retry_audio(agent);
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "[VelaWear] Audio sensor init failed after retry: %d\n", ret);
      syslog(LOG_WARNING,
             "[Degrade] Microphone input unavailable; display/speaker actions remain\n");
    }

  /* Initialize LLM client */

  ret = velawear_llm_init(&agent->llm);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[VelaWear] LLM init failed: %d\n", ret);
      velawear_set_degradation_mode(agent, VELAWEAR_MODE_INDEPENDENT,
                                     "llm unavailable");
    }
  else if (!agent->llm.connected)
    {
      velawear_set_degradation_mode(agent, VELAWEAR_MODE_INDEPENDENT,
                                     "cloud disconnected");
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
                                        VELAWEAR_ACTION_PLAY_SOUND,
                                        velawear_play_sound_action_handler,
                                        agent);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] Sound handler registration failed: %d\n",
             ret);
      return ret;
    }

  ret = action_manager_register_handler(&agent->actions,
                                        VELAWEAR_ACTION_PLAY_VOICE,
                                        velawear_play_voice_action_handler,
                                        agent);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] Voice handler registration failed: %d\n",
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

  ret = action_manager_register_handler(&agent->actions,
                                        VELAWEAR_ACTION_CALL_LLM,
                                        velawear_call_llm_action_handler,
                                        agent);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] LLM handler registration failed: %d\n",
             ret);
      return ret;
    }

  ret = action_manager_register_handler(&agent->actions,
                                        VELAWEAR_ACTION_SET_TIMER,
                                        velawear_set_timer_action_handler,
                                        agent);
  if (ret < 0)
    {
      syslog(LOG_ERR, "[VelaWear] Timer handler registration failed: %d\n",
             ret);
      return ret;
    }

  ret = pthread_mutex_init(&agent->timer_lock, NULL);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[VelaWear] Timer lock init failed: %d\n", ret);
      return VELAWEAR_ERR_IO;
    }
  agent->timer_lock_initialized = true;

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

  /* Decision rules use both event-driven evaluation and a real-state
   * periodic worker so inactivity can cross its threshold without another
   * motion edge. */
  agent->running = true;
  agent->started_ms = velawear_now_ms();
  ret = decision_engine_start(&agent->engine);
  if (ret < 0)
    {
      agent->running = false;
      syslog(LOG_ERR, "[VelaWear] Decision engine start failed: %d\n", ret);
      return ret;
    }

  /* Mark agent as running before starting the sensor producer. */

  ret = velawear_watchdog_start(agent);
  if (ret < 0)
    {
      agent->running = false;
      return ret;
    }

  if (agent->audio.initialized)
    {
      ret = audio_sensor_start(&agent->audio, &agent->events);
      if (ret < 0)
        {
          agent->audio_available = false;
          syslog(LOG_WARNING,
                 "[VelaWear] Continuous microphone start failed: %d\n", ret);
        }
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
#ifdef CONFIG_VELAWEAR_XIAOZHI_AUTOSTART
  ret = velawear_xiaozhi_start();
  if (ret < 0 && ret != -EALREADY)
    {
      syslog(LOG_WARNING, "[VelaWear] XiaoZhi start failed: %d\n", ret);
    }
  else
    {
      syslog(LOG_INFO, "[VelaWear] XiaoZhi worker requested\n");
    }
#endif
  return VELAWEAR_OK;
}

static void velawear_cleanup(velawear_agent_t *agent)
{
  /* Stop subsystems in reverse order */

  agent->running = false;
#ifdef CONFIG_VELAWEAR_XIAOZHI
  (void)velawear_xiaozhi_stop();
#endif
  velawear_watchdog_stop(agent);
  if (agent->imu_thread_started)
    {
      pthread_join(agent->imu_thread, NULL);
      agent->imu_thread_started = false;
    }

  audio_sensor_cleanup(&agent->audio);
  velawear_llm_cleanup(&agent->llm);
#ifdef CONFIG_VELAWEAR_XIAOZHI_PAN
  velawear_pan_cleanup();
#endif
  velawear_ble_cleanup();
  imu_sensor_cleanup(&agent->imu);
  /* Stop the periodic decision worker before destroying the managers it
   * references. */
  decision_engine_cleanup(&agent->engine);
  action_manager_cleanup(&agent->actions);
  event_manager_cleanup(&agent->events);
  velawear_buttons_close();
  display_manager_cleanup(&agent->display);
  state_manager_cleanup(&agent->state_mgr);
  velawear_config_cleanup(&agent->config);
  if (agent->timer_lock_initialized)
    {
      pthread_mutex_destroy(&agent->timer_lock);
      agent->timer_lock_initialized = false;
    }
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

  if (event->type == VELAWEAR_EVENT_CHAT_INPUT)
    {
      if (event->data.chat.length <= 0)
        {
          return VELAWEAR_ERR_INVAL;
        }

      memset(&action, 0, sizeof(action));
      action.type = VELAWEAR_ACTION_CALL_LLM;
      action.priority = event->priority;
      snprintf(action.params.llm.prompt, sizeof(action.params.llm.prompt),
               "%s", event->data.chat.text);
      ret = action_manager_execute(&agent->actions, &action);
      if (ret < 0)
        {
          return ret;
        }
      display_manager_set_companion_phase(&agent->display,
                                          VELAWEAR_COMPANION_THINKING,
                                          "我正在认真想怎么回复");
      syslog(LOG_INFO, "[HCI] Chat input accepted bytes=%d\n",
             event->data.chat.length);
      return action_manager_process(&agent->actions);
    }

  if (event->type == VELAWEAR_EVENT_VOICE_CMD)
    {
      memset(&action, 0, sizeof(action));
      action.type = VELAWEAR_ACTION_PLAY_VOICE;
      action.priority = event->priority;
      action.params.voice.stream_id = event->data.voice.stream_id;
      action.params.voice.sample_count = event->data.voice.sample_count;
      ret = action_manager_execute(&agent->actions, &action);
      if (ret < 0)
        {
          return ret;
        }
      return action_manager_process(&agent->actions);
    }

  if (event->type == VELAWEAR_EVENT_BLE_MSG)
    {
      return velawear_handle_agent_command(agent, event);
    }

  if (event->type == VELAWEAR_EVENT_AUDIO)
    {
      syslog(LOG_INFO,
             "[Audio] event=%s avg_abs=%lu peak_abs=%lu active=%d\n",
             event->data.audio.text,
             (unsigned long)event->data.audio.avg_abs,
             (unsigned long)event->data.audio.peak_abs,
             event->data.audio.active ? 1 : 0);
    }

  ret = state_manager_update_from_event(&agent->state_mgr, event);
  if (ret < 0)
    {
      return ret;
    }

  state = state_manager_get_state(&agent->state_mgr);
  agent->state = state;
  velawear_publish_agent_event(agent, event);
  rule_id = decision_engine_evaluate(&agent->engine, &state, event);

  /* Every matched rule produces a traceable action.  Rule-specific handlers
   * can replace this log action later without changing the event pipeline. */
  if (rule_id >= 0 && event->type != VELAWEAR_EVENT_MOTION &&
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

static void velawear_publish_agent_event(velawear_agent_t *agent,
                                         const velawear_event_t *event)
{
  static int last_motion_type = -1;
  int ret;

  if (agent == NULL || event == NULL || !agent->ble_available)
    {
      return;
    }

  if (event->type == VELAWEAR_EVENT_MOTION)
    {
      if (event->data.motion.motion_type == last_motion_type)
        {
          return;
        }

      last_motion_type = event->data.motion.motion_type;
    }
  else if (event->type != VELAWEAR_EVENT_FALL &&
           event->type != VELAWEAR_EVENT_AUDIO &&
           event->type != VELAWEAR_EVENT_TIMER)
    {
      return;
    }

  ret = velawear_ble_publish_agent_event(event);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[BLE] Agent event publish failed: %d\n", ret);
    }
}

static int velawear_handle_agent_command(velawear_agent_t *agent,
                                         const velawear_event_t *event)
{
  velawear_agent_command_t command;
  velawear_action_t action;
  const char *message = NULL;
  int result = VELAWEAR_AGENT_COMMAND_RESULT_REJECTED;
  int ret;

  if (agent == NULL || event == NULL || event->data.ble.length < 0 ||
      velawear_agent_decode_command((const uint8_t *)event->data.ble.data,
                                    (uint16_t)event->data.ble.length,
                                    &command) < 0)
    {
      syslog(LOG_WARNING, "[BLE] Invalid queued Agent command\n");
      return VELAWEAR_OK;
    }

  switch (command.command_id)
    {
      case VELAWEAR_AGENT_COMMAND_ACK_ALERT:
        display_manager_clear_alert(&agent->display);
        velawear_ble_set_alert(false);
        result = VELAWEAR_AGENT_COMMAND_RESULT_OK;
        break;

      case VELAWEAR_AGENT_COMMAND_SET_SEDENTARY_THRESHOLD:
        ret = velawear_ble_set_sedentary_threshold(command.argument);
        result = ret == VELAWEAR_OK ? VELAWEAR_AGENT_COMMAND_RESULT_OK :
                                      VELAWEAR_AGENT_COMMAND_RESULT_EXECUTION_FAILED;
        break;

      case VELAWEAR_AGENT_COMMAND_SHOW_REMINDER:
        if (command.argument == VELAWEAR_AGENT_REMINDER_HYDRATE)
          {
            message = "AI 建议：补充水分";
          }
        else if (command.argument == VELAWEAR_AGENT_REMINDER_REST)
          {
            message = "AI 建议：休息一下";
          }
        else
          {
            message = "AI 建议：起来活动一下";
          }

        memset(&action, 0, sizeof(action));
        action.type = VELAWEAR_ACTION_CALL_LLM;
        action.priority = VELAWEAR_PRIORITY_HIGH;
        snprintf(action.params.llm.prompt, sizeof(action.params.llm.prompt),
                 "%s", message);
        ret = action_manager_execute(&agent->actions, &action);
        result = ret == VELAWEAR_OK ?
                 VELAWEAR_AGENT_COMMAND_RESULT_OK :
                 VELAWEAR_AGENT_COMMAND_RESULT_EXECUTION_FAILED;
        break;

      default:
        break;
    }

  velawear_ble_report_agent_command_result(command.sequence, command.command_id,
                                           (uint8_t)result);
  syslog(LOG_INFO, "[BLE] Agent command handled: seq=%u id=%u result=%d\n",
         (unsigned int)command.sequence, (unsigned int)command.command_id,
         result);
  return VELAWEAR_OK;
}

static int velawear_log_action_handler(velawear_action_t *action,
                                       void *context)
{
  (void)context;
  if (action)
    {
      syslog(LOG_INFO, "[VelaWear] action %d executed\n", action->type);
    }

  return VELAWEAR_OK;
}

static int velawear_vibrate_action_handler(velawear_action_t *action,
                                            void *context)
{
  int ret;

  if (action == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  ret = velawear_speaker_alert(action->params.vibrate.duration_ms,
                                action->params.vibrate.pattern);
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "[Action] Speaker alert failed; no motor hardware is present\n");
      return VELAWEAR_OK;
    }

  syslog(LOG_INFO, "[Action] Speaker alert: duration=%dms, pattern=%d\n",
         action->params.vibrate.duration_ms,
         action->params.vibrate.pattern);
  return VELAWEAR_OK;
}

static int velawear_play_sound_action_handler(velawear_action_t *action,
                                               void *context)
{
  int ret;

  (void)context;
  if (action == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  ret = velawear_speaker_alert(action->params.vibrate.duration_ms,
                               action->params.vibrate.pattern);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[Action] Play sound failed: %d\n", ret);
      return VELAWEAR_OK;
    }

  syslog(LOG_INFO, "[Action] Play sound: duration=%lu ms pattern=%d\n",
         (unsigned long)action->params.vibrate.duration_ms,
         action->params.vibrate.pattern);
  return VELAWEAR_OK;
}

static int velawear_play_voice_action_handler(velawear_action_t *action,
                                               void *context)
{
  velawear_agent_t *agent = (velawear_agent_t *)context;
  int ret;

  if (action == NULL || agent == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  display_manager_set_companion_phase(&agent->display,
                                          VELAWEAR_COMPANION_SPEAKING,
                                          NULL);
  ret = velawear_audio_voice_play(action->params.voice.stream_id,
                                  action->params.voice.sample_count);
  if (ret < 0)
    {
      display_manager_set_companion_phase(&agent->display,
                                          VELAWEAR_COMPANION_ERROR,
                                          "语音播放失败，文字回复已完成");
      syslog(LOG_WARNING,
             "[Action] AI companion voice playback failed: %d\n", ret);
      return VELAWEAR_ERR_IO;
    }

  display_manager_set_companion_phase(&agent->display,
                                      VELAWEAR_COMPANION_IDLE,
                                      NULL);
  syslog(LOG_INFO,
         "[Action] AI companion voice played: stream=%u samples=%lu\n",
         (unsigned int)action->params.voice.stream_id,
         (unsigned long)action->params.voice.sample_count);
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
  velawear_ble_set_alert(true);
  syslog(LOG_WARNING, "[Action] UI alert shown: %s\n",
         action->params.display.text);
  return VELAWEAR_OK;
}

static int velawear_send_ble_action_handler(velawear_action_t *action,
                                             void *context)
{
  velawear_agent_t *agent = (velawear_agent_t *)context;
  int ret;

  if (action == NULL || agent == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  if (!agent->ble_available || !agent->state.ble_connected)
    {
      agent->ble_available = false;
      velawear_set_degradation_mode(agent, VELAWEAR_MODE_INDEPENDENT,
                                     "ble disconnected");
      syslog(LOG_WARNING,
             "[Action] BLE emergency notification deferred (not connected)\n");
      return VELAWEAR_OK;
    }

  agent->ble_available = true;

  velawear_ble_set_alert(true);
  ret = velawear_ble_send_message(action->params.ble.data,
                                  action->priority);
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "[Action] BLE notification send failed: %d\n", ret);
      return ret;
    }
  syslog(LOG_WARNING, "[Action] BLE emergency notification: %s\n",
         action->params.ble.data);
  return VELAWEAR_OK;
}

static int velawear_call_llm_action_handler(velawear_action_t *action,
                                             void *context)
{
  velawear_agent_t *agent = (velawear_agent_t *)context;
  char response[256];
  int ret;

  if (action == NULL || agent == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  ret = velawear_llm_request(&agent->llm, action->params.llm.prompt,
                             response, sizeof(response));
  if (ret < 0)
    {
      display_manager_set_companion_phase(&agent->display,
                                          VELAWEAR_COMPANION_ERROR,
                                          "回复失败，请稍后再试");
      syslog(LOG_WARNING, "[Action] LLM request failed: %d\n", ret);
      return ret;
    }

  display_manager_set_companion_phase(&agent->display,
                                      VELAWEAR_COMPANION_IDLE,
                                      response);
  syslog(LOG_INFO,
         "[Action] LLM response delivered; voice playback optional: %s\n",
         response);
  return VELAWEAR_OK;
}

static int velawear_set_timer_action_handler(velawear_action_t *action,
                                               void *context)
{
  velawear_agent_t *agent = (velawear_agent_t *)context;
  uint32_t now;

  if (action == NULL || agent == NULL ||
      !agent->timer_lock_initialized ||
      action->params.timer.duration_ms == 0 ||
      action->params.timer.duration_ms > 86400000U)
    {
      return VELAWEAR_ERR_INVAL;
    }

  now = velawear_now_ms();
  pthread_mutex_lock(&agent->timer_lock);
  agent->timer_deadline_ms = now + action->params.timer.duration_ms;
  agent->timer_id = action->params.timer.id;
  agent->timer_active = true;
  pthread_mutex_unlock(&agent->timer_lock);

  syslog(LOG_INFO, "[Timer] Scheduled id=%d duration=%lu ms\n",
         action->params.timer.id,
         (unsigned long)action->params.timer.duration_ms);
  return VELAWEAR_OK;
}
