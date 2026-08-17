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

#include "velawear.h"
#include "decision_engine.h"

/****************************************************************************
 * Hardcoded Rules (Layer 0 - Emergency)
 ****************************************************************************/

static bool fall_detected_condition(velawear_state_t *state,
                                    velawear_event_t *event)
{
  return event && event->type == VELAWEAR_EVENT_FALL;
}

static int fall_detected_action(velawear_state_t *state, void *context)
{
  syslog(LOG_WARNING, "[Decision] FALL DETECTED! Sending emergency alert\n");

  /* TODO: Send emergency BLE notification */
  /* TODO: Show emergency UI */
  /* TODO: Vibrate motor */

  return VELAWEAR_OK;
}

static bool heart_rate_critical_condition(velawear_state_t *state,
                                          velawear_event_t *event)
{
  /* Zero is the driver's invalid/no-sensor value, not bradycardia. */
  return state->heart_rate > 0.0f &&
         (state->heart_rate > 180.0f || state->heart_rate < 40.0f);
}

static int heart_rate_critical_action(velawear_state_t *state, void *context)
{
  syslog(LOG_WARNING, "[Decision] Heart rate critical: %.1f\n",
         state->heart_rate);

  /* TODO: Show warning UI */
  /* TODO: Send BLE alert */

  return VELAWEAR_OK;
}

static bool battery_critical_condition(velawear_state_t *state,
                                       velawear_event_t *event)
{
  return state->battery_level < 5 && !state->is_charging;
}

static int battery_critical_action(velawear_state_t *state, void *context)
{
  syslog(LOG_WARNING, "[Decision] Battery critical: %lu%%\n",
         (unsigned long)state->battery_level);

  /* Switch to deep sleep mode */
  state->power_mode = VELAWEAR_POWER_DEEP_SLEEP;

  return VELAWEAR_OK;
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void *decision_engine_thread(void *arg)
{
  velawear_engine_t *engine = (velawear_engine_t *)arg;
  velawear_state_t state;
  velawear_event_t event;
  int ret;

  syslog(LOG_INFO, "[Decision] Thread started\n");

  while (engine->running)
    {
      /* Sleep for decision interval */

      usleep(100000);  /* 100ms */

      /* Get current state */

      /* The event-driven path supplies real state and events.  Keep this
       * worker safe for callers that explicitly start it by using neutral
       * values instead of uninitialized stack data. */
      memset(&state, 0, sizeof(state));
      memset(&event, 0, sizeof(event));
      state.battery_level = 100;
      state.heart_rate = 60.0f;
      state.is_charging = true;

      /* TODO: Get state from state manager */

      /* Evaluate rules by priority */

      pthread_mutex_lock(&engine->lock);

      for (int i = 0; i < engine->rule_count; i++)
        {
          if (!engine->rules[i].enabled)
            {
              continue;
            }

          if (engine->rules[i].condition &&
              engine->rules[i].condition(&state, &event))
            {
              ret = engine->rules[i].action(&state,
                                            engine->rules[i].context);
              if (ret < 0)
                {
                  syslog(LOG_WARNING,
                         "[Decision] Rule '%s' action failed: %d\n",
                         engine->rules[i].name, ret);
                }

              /* For emergency rules, stop after first match */

              if (engine->rules[i].layer == 0)
                {
                  break;
                }
            }
        }

      pthread_mutex_unlock(&engine->lock);
    }

  syslog(LOG_INFO, "[Decision] Thread stopped\n");
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int decision_engine_init(velawear_engine_t *engine, velawear_config_t *config)
{
  memset(engine, 0, sizeof(velawear_engine_t));

  pthread_mutex_init(&engine->lock, NULL);
  engine->config = config;
  engine->running = false;

  /* Register hardcoded emergency rules */

  decision_engine_add_rule(engine, "fall_detected", 0,
                           VELAWEAR_PRIORITY_CRITICAL,
                           fall_detected_condition,
                           fall_detected_action, NULL);

  decision_engine_add_rule(engine, "heart_rate_critical", 0,
                           VELAWEAR_PRIORITY_CRITICAL,
                           heart_rate_critical_condition,
                           heart_rate_critical_action, NULL);

  decision_engine_add_rule(engine, "battery_critical", 0,
                           VELAWEAR_PRIORITY_HIGH,
                           battery_critical_condition,
                           battery_critical_action, NULL);

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

  pthread_mutex_lock(&engine->lock);

  for (int i = 0; i < engine->rule_count; i++)
    {
      if (!engine->rules[i].enabled)
        {
          continue;
        }

      if (engine->rules[i].condition &&
          engine->rules[i].condition(state, event))
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
