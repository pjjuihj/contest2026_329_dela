/*
 * VelaWear Agent - Decision Engine
 *
 * Implements 3-layer decision model: hardcoded, configurable, LLM.
 *
 * Author: pjjuihj
 * Team: 329 - dela
 */

#ifndef __VELAWEAR_DECISION_ENGINE_H
#define __VELAWEAR_DECISION_ENGINE_H

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

#define DECISION_RULES_MAX      32
#define DECISION_LLM_BUFFER     1024

/****************************************************************************
 * Type Definitions
 ****************************************************************************/

/* Rule callback types */

typedef bool (*rule_condition_t)(velawear_state_t *state,
                                 velawear_event_t *event);
typedef int (*rule_action_t)(velawear_state_t *state, void *context);

/* Rule structure */

typedef struct decision_rule
{
  int id;
  char name[64];
  uint32_t priority;
  int layer;  /* 0=hardcoded, 1=configurable, 2=LLM */
  rule_condition_t condition;
  rule_action_t action;
  void *context;
  bool enabled;
} decision_rule_t;

/* Decision engine structure */

typedef struct velawear_engine
{
  pthread_t thread;
  bool running;
  decision_rule_t rules[DECISION_RULES_MAX];
  int rule_count;
  pthread_mutex_t lock;
  velawear_config_t *config;
  velawear_actions_t *actions;
  uint32_t last_sedentary_reminder;
} velawear_engine_t;

/****************************************************************************
 * Function Prototypes
 ****************************************************************************/

int decision_engine_init(velawear_engine_t *engine, velawear_config_t *config);
int decision_engine_start(velawear_engine_t *engine);
int decision_engine_set_action_manager(velawear_engine_t *engine,
                                        velawear_actions_t *actions);
void decision_engine_cleanup(velawear_engine_t *engine);

int decision_engine_add_rule(velawear_engine_t *engine,
                             const char *name,
                             int layer,
                             uint32_t priority,
                             rule_condition_t condition,
                             rule_action_t action,
                             void *context);
int decision_engine_remove_rule(velawear_engine_t *engine, int rule_id);
int decision_engine_evaluate(velawear_engine_t *engine,
                             velawear_state_t *state,
                             velawear_event_t *event);

#endif /* __VELAWEAR_DECISION_ENGINE_H */
