/*
 * VelaWear Agent - LLM Client
 *
 * Manages LLM inference requests via velaclaw_client.
 *
 * Author: pjjuihj
 * Team: 329 - dela
 */

#include <nuttx/config.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <syslog.h>

#include "velawear.h"
#include "drivers/velawear_ble.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int velawear_llm_init(velawear_llm_t *llm)
{
  if (llm == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  memset(llm, 0, sizeof(velawear_llm_t));

  strncpy(llm->model_name, "mimo", sizeof(llm->model_name) - 1);
  llm->initialized = true;
  llm->connected = false;
  llm->request_count = 0;

  syslog(LOG_INFO, "[LLM] Initialized (BLE cloud bridge + local fallback ready)\n");
  return VELAWEAR_OK;
}

void velawear_llm_cleanup(velawear_llm_t *llm)
{
  if (llm == NULL)
    {
      return;
    }

  llm->initialized = false;
  llm->connected = false;
  syslog(LOG_INFO, "[LLM] Cleaned up\n");
}

int velawear_llm_request(velawear_llm_t *llm, const char *prompt,
                         char *response, size_t response_size)
{
  int ret;

  if (llm == NULL || !llm->initialized || prompt == NULL ||
      response == NULL || response_size == 0)
    {
      return VELAWEAR_ERR_INVAL;
    }

  llm->request_count++;

  ret = velawear_ble_request_llm(prompt, response, response_size, 15000);
  if (ret == 0 && response[0] != '\0')
    {
      llm->connected = true;
      syslog(LOG_INFO, "[LLM] request=%d cloud_bridge connected=1\n",
             llm->request_count);
      return VELAWEAR_OK;
    }

  llm->connected = false;

  /* The BLE bridge may be absent, disconnected, or unable to reach its
   * configured cloud endpoint. Keep the Agent usable offline with short,
   * warm companion replies instead of dropping a CALL_LLM action. */
  if (strstr(prompt, "陪") != NULL || strstr(prompt, "聊") != NULL ||
      strstr(prompt, "想你") != NULL)
    {
      snprintf(response, response_size,
               "我在呢，先别急。你想从今天哪一件小事说起？");
    }
  else if (strstr(prompt, "夸") != NULL || strstr(prompt, "鼓励") != NULL ||
           strstr(prompt, "加油") != NULL)
    {
      snprintf(response, response_size,
               "当然可以，你已经做得很好了，愿意坚持到现在就很棒。");
    }
  else if (strstr(prompt, "休息") != NULL)
    {
      snprintf(response, response_size,
               "好呀，先放下手里的事，慢慢呼吸，我陪你休息一会儿。");
    }
  else if (strstr(prompt, "水") != NULL || strstr(prompt, "喝") != NULL)
    {
      snprintf(response, response_size,
               "记得喝点水，我会陪你把状态照顾好。");
    }
  else if (strstr(prompt, "活动") != NULL)
    {
      snprintf(response, response_size,
               "起来陪我走几步吧，活动一下会舒服很多。");
    }
  else
    {
      snprintf(response, response_size,
               "我在这里陪着你，先照顾好自己的状态，好吗？");
    }

  syslog(LOG_INFO, "[LLM] request=%d local_fallback cloud_error=%d\n",
         llm->request_count, ret);
  return VELAWEAR_OK;
}
