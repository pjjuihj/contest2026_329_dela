/*
 * VelaWear Agent - LLM Client
 *
 * Manages LLM inference requests via velaclaw_client.
 *
 * Author: pjjuihj
 * Team: 329 - dela
 */

#include <nuttx/config.h>
#include <string.h>
#include <syslog.h>

#include "velawear.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int velawear_llm_init(velawear_llm_t *llm)
{
  memset(llm, 0, sizeof(velawear_llm_t));

  strncpy(llm->model_name, "mimo", sizeof(llm->model_name) - 1);
  llm->initialized = true;
  llm->connected = false;
  llm->request_count = 0;

  syslog(LOG_INFO, "[LLM] Initialized\n");
  return VELAWEAR_OK;
}

void velawear_llm_cleanup(velawear_llm_t *llm)
{
  llm->initialized = false;
  llm->connected = false;
  syslog(LOG_INFO, "[LLM] Cleaned up\n");
}
