/*
 * VelaWear Agent - Decision Engine Implementation
 *
 * Implements 3-layer decision model: hardcoded, configurable, LLM.
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
#include <time.h>

#include "velawear.h"
#include "decision_engine.h"

/****************************************************************************
 * Hardcoded Rules (Layer 0 - Emergency)
 ****************************************************************************/

static bool decision_alert_allowed(uint32_t *last_alert,
                                   uint32_t cooldown_sec)
{
  uint32_t now = (uint32_t)time(NULL);

  if (last_alert == NULL)
    {
      return false;
    }

  if (*last_alert != 0 && now >= *last_alert &&
      now - *last_alert < cooldown_sec)
    {
      return false;
    }

  *last_alert = now;
  return true;
}

static int decision_execute_alert(velawear_engine_t *engine,
                                  uint32_t priority,
                                  const char *text,
                                  int duration_ms,
                                  int pattern,
                                  const char *ble_text)
{
  velawear_action_t action;
  int ret;
  int first_error = VELAWEAR_OK;

  if (engine == NULL || engine->actions == NULL || text == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  memset(&action, 0, sizeof(action));
  action.type = VELAWEAR_ACTION_SHOW_UI;
  action.priority = priority;
  snprintf(action.params.display.text, sizeof(action.params.display.text),
           "%s", text);
  ret = action_manager_execute(engine->actions, &action);
  if (ret < 0)
    {
      return ret;
    }

  memset(&action, 0, sizeof(action));
  action.type = VELAWEAR_ACTION_VIBRATE;
  action.priority = priority;
  action.params.vibrate.duration_ms = duration_ms;
  action.params.vibrate.pattern = pattern;
  ret = action_manager_execute(engine->actions, &action);
  if (ret < 0)
    {
      first_error = ret;
    }

  if (ble_text != NULL)
    {
      memset(&action, 0, sizeof(action));
      action.type = VELAWEAR_ACTION_SEND_BLE;
      action.priority = priority;
      snprintf(action.params.ble.data, sizeof(action.params.ble.data),
               "%s", ble_text);
      action.params.ble.length = strlen(action.params.ble.data);
      ret = action_manager_execute(engine->actions, &action);
      if (ret < 0 && first_error == VELAWEAR_OK)
        {
          first_error = ret;
        }
    }

  return first_error;
}

static bool fall_detected_condition(velawear_state_t *state,
                                    velawear_event_t *event,
                                    void *context)
{
  (void)state;
  (void)context;
  return event && event->type == VELAWEAR_EVENT_FALL;
}

static int fall_detected_action(velawear_state_t *state, void *context)
{
  velawear_engine_t *engine = (velawear_engine_t *)context;
  velawear_action_t action;
  int ret;

  if (engine == NULL || engine->actions == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  memset(&action, 0, sizeof(action));
  action.type = VELAWEAR_ACTION_SHOW_UI;
  action.priority = VELAWEAR_PRIORITY_CRITICAL;
  snprintf(action.params.display.text, sizeof(action.params.display.text),
           "检测到运动中断\n检查设备");
  ret = action_manager_execute(engine->actions, &action);
  if (ret < 0)
    {
      return ret;
    }

  memset(&action, 0, sizeof(action));
  action.type = VELAWEAR_ACTION_VIBRATE;
  action.priority = VELAWEAR_PRIORITY_CRITICAL;
  action.params.vibrate.duration_ms = 3000;
  action.params.vibrate.pattern = 8; /* SOS: ... --- ... */
  ret = action_manager_execute(engine->actions, &action);
  if (ret < 0)
    {
      return ret;
    }

  memset(&action, 0, sizeof(action));
  action.type = VELAWEAR_ACTION_SEND_BLE;
  action.priority = VELAWEAR_PRIORITY_CRITICAL;
  snprintf(action.params.ble.data, sizeof(action.params.ble.data),
           "FALL DETECTED: emergency assistance required");
  action.params.ble.length = strlen(action.params.ble.data);
  ret = action_manager_execute(engine->actions, &action);
  if (ret < 0)
    {
      return ret;
    }

  syslog(LOG_WARNING, "[Decision] FALL DETECTED! Emergency actions queued\n");
  return VELAWEAR_OK;
}

static bool heart_rate_critical_condition(velawear_state_t *state,
                                          velawear_event_t *event,
                                          void *context)
{
  (void)event;
  (void)context;
  /* Zero is the driver's invalid/no-sensor value, not bradycardia. */
  return state->heart_rate > 0.0f &&
         (state->heart_rate > 180.0f || state->heart_rate < 40.0f);
}

static int heart_rate_critical_action(velawear_state_t *state, void *context)
{
  velawear_engine_t *engine = (velawear_engine_t *)context;
  char text[128];
  char ble_text[160];

  if (state == NULL || engine == NULL || engine->actions == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  if (!decision_alert_allowed(&engine->last_heart_rate_alert, 60))
    {
      return VELAWEAR_OK;
    }

  snprintf(text, sizeof(text), "心率异常\n请立即休息\n%.0f bpm",
           state->heart_rate);
  snprintf(ble_text, sizeof(ble_text),
           "HEART RATE CRITICAL: %.1f bpm", state->heart_rate);
  syslog(LOG_WARNING, "[Decision] Heart rate critical: %.1f\n",
         state->heart_rate);
  return decision_execute_alert(engine, VELAWEAR_PRIORITY_CRITICAL, text,
                                1800, 8, ble_text);
}

static bool battery_critical_condition(velawear_state_t *state,
                                       velawear_event_t *event,
                                       void *context)
{
  (void)event;
  (void)context;
  return state->battery_level < 5 && !state->is_charging;
}

static int battery_critical_action(velawear_state_t *state, void *context)
{
  velawear_engine_t *engine = (velawear_engine_t *)context;
  int ret;

  if (state == NULL || engine == NULL || engine->actions == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  if (!decision_alert_allowed(&engine->last_battery_alert, 300))
    {
      return VELAWEAR_OK;
    }

  syslog(LOG_WARNING, "[Decision] Battery critical: %lu%%\n",
         (unsigned long)state->battery_level);

  /* Reflect the policy in the shared state so the UI and other consumers see
   * the same low-power decision. */
  state->power_mode = VELAWEAR_POWER_DEEP_SLEEP;
  if (engine->state_mgr != NULL)
    {
      state_manager_update_power_mode(engine->state_mgr,
                                      VELAWEAR_POWER_DEEP_SLEEP);
    }

  ret = decision_execute_alert(engine, VELAWEAR_PRIORITY_HIGH,
                               "电量过低\n请尽快充电", 1200, 2,
                               "BATTERY CRITICAL: charge required");
  return ret;
}

/****************************************************************************
 * Sedentary Reminder (Layer 1 - Configurable)
 ****************************************************************************/

static uint32_t sedentary_threshold_sec(velawear_engine_t *engine)
{
  if (engine != NULL && engine->config != NULL &&
      engine->config->sedentary_threshold_sec > 0)
    {
      return engine->config->sedentary_threshold_sec;
    }

  return 45 * 60;
}

static bool sedentary_condition(velawear_state_t *state,
                                velawear_event_t *event,
                                void *context)
{
  velawear_engine_t *engine = (velawear_engine_t *)context;
  uint32_t now;

  if (event == NULL ||
      (event->type != VELAWEAR_EVENT_MOTION &&
       event->type != VELAWEAR_EVENT_TIMER) ||
      state == NULL || state->is_moving || state->last_move_time == 0)
    {
      return false;
    }

  now = (uint32_t)time(NULL);
  return now >= state->last_move_time &&
         now - state->last_move_time >= sedentary_threshold_sec(engine);
}

static int sedentary_action(velawear_state_t *state, void *context)
{
  velawear_engine_t *engine = (velawear_engine_t *)context;
  velawear_action_t action;
  uint32_t now;
  uint32_t cooldown_sec = 5 * 60;
  int ret;

  if (engine == NULL || engine->actions == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  now = (uint32_t)time(NULL);
  if (engine->last_sedentary_reminder != 0 &&
      now >= engine->last_sedentary_reminder &&
      now - engine->last_sedentary_reminder < cooldown_sec)
    {
      return VELAWEAR_OK;
    }

  memset(&action, 0, sizeof(action));
  action.type = VELAWEAR_ACTION_SHOW_UI;
  action.priority = VELAWEAR_PRIORITY_NORMAL;
  snprintf(action.params.display.text, sizeof(action.params.display.text),
           "该活动一下了\n检测到持续静止");

  ret = action_manager_execute(engine->actions, &action);
  if (ret == VELAWEAR_OK)
    {
      engine->last_sedentary_reminder = now;

      memset(&action, 0, sizeof(action));
      action.type = VELAWEAR_ACTION_CALL_LLM;
      action.priority = VELAWEAR_PRIORITY_NORMAL;
      snprintf(action.params.llm.prompt, sizeof(action.params.llm.prompt),
               "检测到用户持续静止，请给出一句简短的活动建议");
      if (action_manager_execute(engine->actions, &action) < 0)
        {
          syslog(LOG_WARNING,
                 "[Decision] Cloud LLM reminder queue failed\n");
        }

      /* Feed the normal event path once so the BLE Agent receives a
       * sedentary event as well as the local UI/audio reminder. */
      if (engine->events != NULL)
        {
          velawear_event_t timer_event;

          memset(&timer_event, 0, sizeof(timer_event));
          timer_event.type = VELAWEAR_EVENT_TIMER;
          timer_event.priority = VELAWEAR_PRIORITY_NORMAL;
          timer_event.timestamp = now;
          timer_event.data.timer.id = 1;
          if (event_manager_push(engine->events, &timer_event) < 0)
            {
              syslog(LOG_WARNING,
                     "[Decision] Sedentary event queue failed\n");
            }
        }

      syslog(LOG_INFO,
             "[Decision] Sedentary reminder queued after %lu s\n",
             (unsigned long)sedentary_threshold_sec(engine));
    }

  return ret;
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void *decision_engine_thread(void *arg)
{
  velawear_engine_t *engine = (velawear_engine_t *)arg;
  velawear_state_t state;
  velawear_event_t event;

  syslog(LOG_INFO, "[Decision] Thread started\n");

  while (engine->running)
    {
      usleep(100000);  /* 100ms */

      if (engine->state_mgr != NULL)
        {
          memset(&event, 0, sizeof(event));
          state = state_manager_get_state(engine->state_mgr);
          event.type = VELAWEAR_EVENT_TIMER;
          event.priority = VELAWEAR_PRIORITY_NORMAL;
          event.timestamp = (uint32_t)time(NULL);
          event.data.timer.id = 0;
          (void)decision_engine_evaluate(engine, &state, &event);
        }
    }

  syslog(LOG_INFO, "[Decision] Thread stopped\n");
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int decision_engine_init(velawear_engine_t *engine, velawear_config_t *config)
{
  if (engine == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  memset(engine, 0, sizeof(velawear_engine_t));

  pthread_mutex_init(&engine->lock, NULL);
  engine->config = config;
  engine->running = false;
  engine->sedentary_rule_id = -1;

  /* Register hardcoded emergency rules */

  decision_engine_add_rule(engine, "fall_detected", 0,
                           VELAWEAR_PRIORITY_CRITICAL,
                           fall_detected_condition,
                           fall_detected_action, engine);

  decision_engine_add_rule(engine, "heart_rate_critical", 0,
                           VELAWEAR_PRIORITY_CRITICAL,
                           heart_rate_critical_condition,
                           heart_rate_critical_action, engine);

  decision_engine_add_rule(engine, "battery_critical", 0,
                           VELAWEAR_PRIORITY_HIGH,
                           battery_critical_condition,
                           battery_critical_action, engine);

  engine->sedentary_rule_id =
    decision_engine_add_rule(engine, "sedentary_reminder", 1,
                             VELAWEAR_PRIORITY_NORMAL,
                             sedentary_condition,
                             sedentary_action, engine);

  syslog(LOG_INFO, "[Decision] Initialized with %d rules\n",
         engine->rule_count);
  return VELAWEAR_OK;
}

int decision_engine_start(velawear_engine_t *engine)
{
  int ret;

  if (engine->running)
    {
      return VELAWEAR_ERR_BUSY;
    }

  engine->running = true;

  ret = pthread_create(&engine->thread, NULL, decision_engine_thread, engine);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[Decision] Failed to create thread: %d\n", ret);
      engine->running = false;
      return VELAWEAR_ERR_IO;
    }

  syslog(LOG_INFO, "[Decision] Started\n");
  return VELAWEAR_OK;
}

int decision_engine_set_action_manager(velawear_engine_t *engine,
                                        velawear_actions_t *actions)
{
  if (engine == NULL || actions == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  pthread_mutex_lock(&engine->lock);
  engine->actions = actions;
  pthread_mutex_unlock(&engine->lock);
  return VELAWEAR_OK;
}

int decision_engine_set_state_manager(velawear_engine_t *engine,
                                      velawear_state_mgr_t *state_mgr)
{
  if (engine == NULL || state_mgr == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  pthread_mutex_lock(&engine->lock);
  engine->state_mgr = state_mgr;
  pthread_mutex_unlock(&engine->lock);
  return VELAWEAR_OK;
}

int decision_engine_set_event_manager(velawear_engine_t *engine,
                                      velawear_events_t *events)
{
  if (engine == NULL || events == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  pthread_mutex_lock(&engine->lock);
  engine->events = events;
  pthread_mutex_unlock(&engine->lock);
  return VELAWEAR_OK;
}

void decision_engine_cleanup(velawear_engine_t *engine)
{
  if (engine->running)
    {
      engine->running = false;
      pthread_join(engine->thread, NULL);
    }

  pthread_mutex_destroy(&engine->lock);

  syslog(LOG_INFO, "[Decision] Cleaned up\n");
}

int decision_engine_add_rule(velawear_engine_t *engine,
                             const char *name,
                             int layer,
                             uint32_t priority,
                             rule_condition_t condition,
                             rule_action_t action,
                             void *context)
{
  pthread_mutex_lock(&engine->lock);

  if (engine->rule_count >= DECISION_RULES_MAX)
    {
      pthread_mutex_unlock(&engine->lock);
      return VELAWEAR_ERR_NOMEM;
    }

  decision_rule_t *rule = &engine->rules[engine->rule_count];
  rule->id = engine->rule_count;
  strncpy(rule->name, name, sizeof(rule->name) - 1);
  rule->layer = layer;
  rule->priority = priority;
  rule->condition = condition;
  rule->action = action;
  rule->context = context;
  rule->enabled = true;

  engine->rule_count++;

  pthread_mutex_unlock(&engine->lock);

  syslog(LOG_INFO, "[Decision] Added rule '%s' (layer=%d, priority=%lu)\n",
         name, layer, (unsigned long)priority);
  return rule->id;
}

int decision_engine_remove_rule(velawear_engine_t *engine, int rule_id)
{
  pthread_mutex_lock(&engine->lock);

  if (rule_id < 0 || rule_id >= engine->rule_count)
    {
      pthread_mutex_unlock(&engine->lock);
      return VELAWEAR_ERR_INVAL;
    }

  engine->rules[rule_id].enabled = false;

  pthread_mutex_unlock(&engine->lock);

  syslog(LOG_INFO, "[Decision] Disabled rule %d\n", rule_id);
  return VELAWEAR_OK;
}

int decision_engine_evaluate(velawear_engine_t *engine,
                             velawear_state_t *state,
                             velawear_event_t *event)
{
  int ret;

  if (engine == NULL || state == NULL || event == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  pthread_mutex_lock(&engine->lock);

  for (int i = 0; i < engine->rule_count; i++)
    {
      if (!engine->rules[i].enabled)
        {
          continue;
        }

      if (engine->rules[i].condition &&
          engine->rules[i].condition(state, event,
                                     engine->rules[i].context))
        {
          ret = engine->rules[i].action(state, engine->rules[i].context);
          if (ret < 0)
            {
              syslog(LOG_WARNING,
                     "[Decision] Rule '%s' action failed: %d\n",
                     engine->rules[i].name, ret);
            }

          /* For emergency rules, stop after first match */

          if (engine->rules[i].layer == 0)
            {
              pthread_mutex_unlock(&engine->lock);
              return engine->rules[i].id;
            }
        }
    }

  pthread_mutex_unlock(&engine->lock);

  return -1;  /* No rule matched */
}
