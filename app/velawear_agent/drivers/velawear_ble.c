/*
 * VelaWear Agent - BLE service
 *
 * Service contract:
 *   status     READ + NOTIFY: [motion_type, intensity_percent, alert]
 *   threshold  READ + WRITE:  uint16 little-endian seconds
 */

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>

#include "velawear_ble.h"
#include "velawear_agent_protocol.h"
#include "audio_hw_test.h"

#define VELAWEAR_BLE_SERVICE_UUID \
  BT_UUID_DECLARE_128( \
    BT_UUID_128_ENCODE(0x12345678, 0x9abc, 0xdef0, \
                       0x1234, 0x56789abcdef0))

#define VELAWEAR_BLE_STATUS_UUID \
  BT_UUID_DECLARE_128( \
    BT_UUID_128_ENCODE(0x12345679, 0x9abc, 0xdef0, \
                       0x1234, 0x56789abcdef0))

#define VELAWEAR_BLE_THRESHOLD_UUID \
  BT_UUID_DECLARE_128( \
    BT_UUID_128_ENCODE(0x1234567a, 0x9abc, 0xdef0, \
                       0x1234, 0x56789abcdef0))

#define VELAWEAR_BLE_AGENT_EVENT_UUID \
  BT_UUID_DECLARE_128( \
    BT_UUID_128_ENCODE(0x1234567b, 0x9abc, 0xdef0, \
                       0x1234, 0x56789abcdef0))

#define VELAWEAR_BLE_AGENT_COMMAND_UUID \
  BT_UUID_DECLARE_128( \
    BT_UUID_128_ENCODE(0x1234567c, 0x9abc, 0xdef0, \
                       0x1234, 0x56789abcdef0))

#define VELAWEAR_BLE_AGENT_RESULT_UUID \
  BT_UUID_DECLARE_128( \
    BT_UUID_128_ENCODE(0x1234567d, 0x9abc, 0xdef0, \
                       0x1234, 0x56789abcdef0))

#define VELAWEAR_BLE_LLM_REQUEST_UUID \
  BT_UUID_DECLARE_128( \
    BT_UUID_128_ENCODE(0x1234567e, 0x9abc, 0xdef0, \
                       0x1234, 0x56789abcdef0))

#define VELAWEAR_BLE_LLM_RESPONSE_UUID \
  BT_UUID_DECLARE_128( \
    BT_UUID_128_ENCODE(0x1234567f, 0x9abc, 0xdef0, \
                       0x1234, 0x56789abcdef0))

#define VELAWEAR_BLE_AGENT_MESSAGE_UUID \
  BT_UUID_DECLARE_128( \
    BT_UUID_128_ENCODE(0x12345680, 0x9abc, 0xdef0, \
                       0x1234, 0x56789abcdef0))

#define VELAWEAR_BLE_AUDIO_STREAM_UUID \
  BT_UUID_DECLARE_128( \
    BT_UUID_128_ENCODE(0x12345681, 0x9abc, 0xdef0, \
                       0x1234, 0x56789abcdef0))

#define VELAWEAR_BLE_CHAT_INPUT_UUID \
  BT_UUID_DECLARE_128( \
    BT_UUID_128_ENCODE(0x12345682, 0x9abc, 0xdef0, \
                       0x1234, 0x56789abcdef0))
#define VELAWEAR_BLE_STATUS_ATTR_INDEX       2
#define VELAWEAR_BLE_AGENT_EVENT_ATTR_INDEX  7
#define VELAWEAR_BLE_AGENT_RESULT_ATTR_INDEX 12
#define VELAWEAR_BLE_LLM_REQUEST_ATTR_INDEX  15
#define VELAWEAR_BLE_LLM_RESPONSE_ATTR_INDEX 18
#define VELAWEAR_BLE_AGENT_MESSAGE_ATTR_INDEX 20
#define VELAWEAR_BLE_AUDIO_STREAM_ATTR_INDEX 23
#define VELAWEAR_BLE_CHAT_INPUT_ATTR_INDEX 25

static velawear_config_t *g_config;
static velawear_events_t *g_events;
static pthread_mutex_t g_lock;
static bool g_initialized;
static bool g_status_notify_enabled;
static struct bt_conn *g_status_notify_conn;
static bool g_agent_event_notify_enabled;
static struct bt_conn *g_agent_event_notify_conn;
static bool g_agent_result_notify_enabled;
static struct bt_conn *g_agent_result_notify_conn;
static bool g_agent_message_notify_enabled;
static struct bt_conn *g_agent_message_notify_conn;
static struct k_work g_status_notify_work;
static bool g_alert_active;
static int g_motion_type;
static uint8_t g_intensity_percent;
static uint16_t g_agent_event_sequence;
static uint16_t g_agent_message_sequence;
static uint16_t g_last_agent_command_sequence;
static bool g_have_last_agent_command_sequence;
static struct bt_conn *g_llm_request_notify_conn;
static bool g_llm_request_notify_enabled;
static struct k_sem g_llm_response_sem;
static bool g_llm_waiting;
static uint16_t g_llm_sequence;
static uint16_t g_llm_pending_sequence;
static uint8_t g_llm_response_status;
static uint16_t g_llm_response_length;
static char g_llm_response_text[VELAWEAR_AGENT_LLM_TEXT_MAX + 1];

static ssize_t velawear_ble_read_status(struct bt_conn *conn,
                                        const struct bt_gatt_attr *attr,
                                        void *buf, uint16_t len,
                                        uint16_t offset)
{
  uint8_t value[3];

  (void)attr;
  pthread_mutex_lock(&g_lock);
  value[0] = (uint8_t)g_motion_type;
  value[1] = g_intensity_percent;
  value[2] = g_alert_active ? 1 : 0;
  pthread_mutex_unlock(&g_lock);

  return bt_gatt_attr_read(conn, attr, buf, len, offset,
                           value, sizeof(value));
}

static ssize_t velawear_ble_read_threshold(struct bt_conn *conn,
                                           const struct bt_gatt_attr *attr,
                                           void *buf, uint16_t len,
                                           uint16_t offset)
{
  uint16_t seconds;

  (void)attr;
  pthread_mutex_lock(&g_lock);
  seconds = (uint16_t)g_config->sedentary_threshold_sec;
  pthread_mutex_unlock(&g_lock);

  return bt_gatt_attr_read(conn, attr, buf, len, offset,
                           &seconds, sizeof(seconds));
}

static ssize_t velawear_ble_write_threshold(struct bt_conn *conn,
                                            const struct bt_gatt_attr *attr,
                                            const void *buf, uint16_t len,
                                            uint16_t offset, uint8_t flags)
{
  const uint8_t *data = (const uint8_t *)buf;
  uint16_t seconds;

  (void)conn;
  (void)attr;
  if (buf == NULL || offset != 0 || len != sizeof(uint16_t) ||
      (flags & BT_GATT_WRITE_FLAG_PREPARE) != 0)
    {
      return -EINVAL;
    }

  seconds = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
  if (seconds < 10 || seconds > 3600)
    {
      return -EINVAL;
    }

  if (velawear_ble_set_sedentary_threshold(seconds) < 0)
    {
      return -EINVAL;
    }

  return len;
}

int velawear_ble_set_sedentary_threshold(uint16_t seconds)
{
  if (seconds < 10 || seconds > 3600)
    {
      return VELAWEAR_ERR_INVAL;
    }

  pthread_mutex_lock(&g_lock);
  if (!g_initialized || g_config == NULL)
    {
      pthread_mutex_unlock(&g_lock);
      return VELAWEAR_ERR_IO;
    }

  g_config->sedentary_threshold_sec = seconds;
  pthread_mutex_unlock(&g_lock);
  syslog(LOG_INFO, "[BLE] Sedentary threshold updated: %u s\n",
         (unsigned int)seconds);
  return VELAWEAR_OK;
}

static void velawear_ble_notify_current_status(void);

static void velawear_ble_notify_work(struct k_work *work)
{
  (void)work;
  velawear_ble_notify_current_status();
}

static ssize_t velawear_ble_notify_ccc_write(struct bt_conn *conn,
                                              uint16_t value,
                                              bool *notify_enabled,
                                              struct bt_conn **notify_conn)
{
  struct bt_conn *old_conn;
  bool enabled = (value & BT_GATT_CCC_NOTIFY) != 0;

  pthread_mutex_lock(&g_lock);
  old_conn = *notify_conn;
  *notify_conn = enabled && conn != NULL ? bt_conn_ref(conn) : NULL;
  *notify_enabled = enabled;
  pthread_mutex_unlock(&g_lock);

  if (old_conn != NULL)
    {
      bt_conn_unref(old_conn);
    }

  return sizeof(value);
}

static ssize_t velawear_ble_status_ccc_write(
    struct bt_conn *conn, const struct bt_gatt_attr *attr, uint16_t value)
{
  ssize_t ret;

  (void)attr;
  ret = velawear_ble_notify_ccc_write(conn, value, &g_status_notify_enabled,
                                      &g_status_notify_conn);
  if ((value & BT_GATT_CCC_NOTIFY) != 0)
    {
      (void)k_work_submit(&g_status_notify_work);
    }

  return ret;
}

static ssize_t velawear_ble_agent_event_ccc_write(
    struct bt_conn *conn, const struct bt_gatt_attr *attr, uint16_t value)
{
  (void)attr;
  return velawear_ble_notify_ccc_write(conn, value,
                                       &g_agent_event_notify_enabled,
                                       &g_agent_event_notify_conn);
}

static ssize_t velawear_ble_agent_result_ccc_write(
    struct bt_conn *conn, const struct bt_gatt_attr *attr, uint16_t value)
{
  (void)attr;
  return velawear_ble_notify_ccc_write(conn, value,
                                       &g_agent_result_notify_enabled,
                                       &g_agent_result_notify_conn);
}

static ssize_t velawear_ble_agent_message_ccc_write(
    struct bt_conn *conn, const struct bt_gatt_attr *attr, uint16_t value)
{
  (void)attr;
  return velawear_ble_notify_ccc_write(conn, value,
                                       &g_agent_message_notify_enabled,
                                       &g_agent_message_notify_conn);
}

static ssize_t velawear_ble_llm_request_ccc_write(
    struct bt_conn *conn, const struct bt_gatt_attr *attr, uint16_t value)
{
  (void)attr;
  return velawear_ble_notify_ccc_write(conn, value,
                                       &g_llm_request_notify_enabled,
                                       &g_llm_request_notify_conn);
}

static ssize_t velawear_ble_write_llm_response(
    struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
    uint16_t len, uint16_t offset, uint8_t flags)
{
  velawear_agent_llm_response_t response;

  (void)conn;
  (void)attr;
  if (buf == NULL || offset != 0 ||
      (flags & BT_GATT_WRITE_FLAG_PREPARE) != 0 ||
      velawear_agent_decode_llm_response((const uint8_t *)buf, len,
                                         &response) < 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_lock);
  if (!g_initialized || !g_llm_waiting ||
      response.sequence != g_llm_pending_sequence)
    {
      pthread_mutex_unlock(&g_lock);
      return -EALREADY;
    }

  g_llm_response_status = response.status;
  g_llm_response_length = response.length;
  memcpy(g_llm_response_text, response.text,
         sizeof(g_llm_response_text));
  g_llm_waiting = false;
  pthread_mutex_unlock(&g_lock);

  k_sem_give(&g_llm_response_sem);
  syslog(LOG_INFO, "[BLE] LLM response accepted: seq=%u status=%u bytes=%u\n",
         (unsigned int)response.sequence, (unsigned int)response.status,
         (unsigned int)response.length);
  return len;
}

static ssize_t velawear_ble_write_agent_command(
    struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
    uint16_t len, uint16_t offset, uint8_t flags)
{
  const uint8_t *data = (const uint8_t *)buf;
  velawear_agent_command_t command;
  velawear_event_t event;
  int ret;

  (void)conn;
  (void)attr;
  if (buf == NULL || offset != 0 ||
      (flags & BT_GATT_WRITE_FLAG_PREPARE) != 0 ||
      velawear_agent_decode_command(data, len, &command) < 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_lock);
  if (g_events == NULL ||
      (g_have_last_agent_command_sequence &&
       !velawear_agent_command_sequence_is_newer(
           command.sequence, g_last_agent_command_sequence)))
    {
      pthread_mutex_unlock(&g_lock);
      return -EALREADY;
    }

  g_last_agent_command_sequence = command.sequence;
  g_have_last_agent_command_sequence = true;
  pthread_mutex_unlock(&g_lock);

  memset(&event, 0, sizeof(event));
  event.type = VELAWEAR_EVENT_BLE_MSG;
  event.priority = VELAWEAR_PRIORITY_HIGH;
  event.data.ble.length = len;
  memcpy(event.data.ble.data, data, len);
  ret = event_manager_push(g_events, &event);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[BLE] Agent command queue full: %d\n", ret);
      return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    }

  syslog(LOG_INFO, "[BLE] Agent command queued: seq=%u id=%u\n",
         (unsigned int)command.sequence, (unsigned int)command.command_id);
  return len;
}

static ssize_t velawear_ble_write_audio_stream(
    struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
    uint16_t len, uint16_t offset, uint8_t flags)
{
  const uint8_t *data = (const uint8_t *)buf;
  int ret;

  (void)conn;
  (void)attr;
  if (buf == NULL || offset != 0 || len < 2 ||
      (flags & BT_GATT_WRITE_FLAG_PREPARE) != 0)
    {
      return -EINVAL;
    }

  switch (data[1])
    {
      case VELAWEAR_AGENT_AUDIO_START_TYPE:
        {
          velawear_agent_audio_start_t start;

          if (velawear_agent_decode_audio_start(data, len, &start) < 0)
            {
              return -EINVAL;
            }
          ret = velawear_audio_voice_start(start.stream_id,
                                           start.sample_count,
                                           start.total_bytes,
                                           start.crc32);
          if (ret == 0)
            {
              syslog(LOG_INFO,
                     "[BLE] Audio stream start: id=%u samples=%lu bytes=%lu\n",
                     (unsigned int)start.stream_id,
                     (unsigned long)start.sample_count,
                     (unsigned long)start.total_bytes);
            }
        }
        break;

      case VELAWEAR_AGENT_AUDIO_DATA_TYPE:
        {
          velawear_agent_audio_data_t audio_data;

          if (velawear_agent_decode_audio_data(data, len, &audio_data) < 0)
            {
              return -EINVAL;
            }
          ret = velawear_audio_voice_write(audio_data.stream_id,
                                           audio_data.offset,
                                           audio_data.payload,
                                           audio_data.payload_length);
        }
        break;

      case VELAWEAR_AGENT_AUDIO_END_TYPE:
        {
          velawear_agent_audio_end_t end;
          velawear_event_t event;
          uint32_t sample_count = 0;

          if (velawear_agent_decode_audio_end(data, len, &end) < 0)
            {
              return -EINVAL;
            }
          ret = velawear_audio_voice_end(end.stream_id, end.total_bytes,
                                         end.crc32, &sample_count);
          if (ret == 0)
            {
              memset(&event, 0, sizeof(event));
              event.type = VELAWEAR_EVENT_VOICE_CMD;
              event.priority = VELAWEAR_PRIORITY_NORMAL;
              event.timestamp = k_uptime_get_32();
              event.data.voice.stream_id = end.stream_id;
              event.data.voice.sample_count = sample_count;
              ret = g_events == NULL ? -EINVAL :
                    event_manager_push(g_events, &event);
              if (ret == 0)
                {
                  syslog(LOG_INFO,
                         "[BLE] Audio stream complete: id=%u samples=%lu\n",
                         (unsigned int)end.stream_id,
                         (unsigned long)sample_count);
                }
              else
                {
                  syslog(LOG_WARNING,
                         "[BLE] Audio voice event queue failed: %d\n", ret);
                  (void)velawear_audio_voice_abort(end.stream_id);
                }
            }
        }
        break;

      default:
        return -EINVAL;
    }

  if (ret < 0)
    {
      syslog(LOG_WARNING, "[BLE] Audio stream write rejected type=%u: %d\n",
             (unsigned int)data[1], ret);
      return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

  return len;
}

static ssize_t velawear_ble_write_chat_input(
    struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
    uint16_t len, uint16_t offset, uint8_t flags)
{
  const uint8_t *data = (const uint8_t *)buf;
  velawear_event_t event;
  int ret;

  (void)conn;
  (void)attr;
  if (buf == NULL || offset != 0 || len == 0 ||
      len >= sizeof(event.data.chat.text) ||
      (flags & BT_GATT_WRITE_FLAG_PREPARE) != 0)
    {
      return -EINVAL;
    }

  if (g_events == NULL)
    {
      return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

  memset(&event, 0, sizeof(event));
  event.type = VELAWEAR_EVENT_CHAT_INPUT;
  event.priority = VELAWEAR_PRIORITY_HIGH;
  event.timestamp = k_uptime_get_32();
  event.data.chat.length = len;
  memcpy(event.data.chat.text, data, len);
  event.data.chat.text[len] = '\0';
  ret = event_manager_push(g_events, &event);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[BLE] Chat input queue full: %d\n", ret);
      return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    }

  syslog(LOG_INFO, "[BLE] Chat input queued bytes=%u\n",
         (unsigned int)len);
  return len;
}

/* Static registration is performed by the Zblue host during bt_enable(). */
BT_GATT_SERVICE_DEFINE(velawear_svc,
  BT_GATT_PRIMARY_SERVICE(VELAWEAR_BLE_SERVICE_UUID),
  BT_GATT_CHARACTERISTIC(VELAWEAR_BLE_STATUS_UUID,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ,
                         velawear_ble_read_status, NULL, NULL),
  BT_GATT_CCC_MANAGED(
      ((struct _bt_gatt_ccc[]) {
        BT_GATT_CCC_INITIALIZER(NULL, velawear_ble_status_ccc_write, NULL)
      }),
      BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
  BT_GATT_CHARACTERISTIC(VELAWEAR_BLE_THRESHOLD_UUID,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         velawear_ble_read_threshold,
                         velawear_ble_write_threshold, NULL),
  BT_GATT_CHARACTERISTIC(VELAWEAR_BLE_AGENT_EVENT_UUID,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ,
                         NULL, NULL, NULL),
  BT_GATT_CCC_MANAGED(
      ((struct _bt_gatt_ccc[]) {
        BT_GATT_CCC_INITIALIZER(NULL, velawear_ble_agent_event_ccc_write, NULL)
      }),
      BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
  BT_GATT_CHARACTERISTIC(VELAWEAR_BLE_AGENT_COMMAND_UUID,
                         BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE,
                         NULL, velawear_ble_write_agent_command, NULL),
  BT_GATT_CHARACTERISTIC(VELAWEAR_BLE_AGENT_RESULT_UUID,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ,
                         NULL, NULL, NULL),
  BT_GATT_CCC_MANAGED(
      ((struct _bt_gatt_ccc[]) {
        BT_GATT_CCC_INITIALIZER(NULL, velawear_ble_agent_result_ccc_write, NULL)
      }),
      BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
  BT_GATT_CHARACTERISTIC(VELAWEAR_BLE_LLM_REQUEST_UUID,
                         BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ,
                         NULL, NULL, NULL),
  BT_GATT_CCC_MANAGED(
      ((struct _bt_gatt_ccc[]) {
        BT_GATT_CCC_INITIALIZER(NULL, velawear_ble_llm_request_ccc_write, NULL)
      }),
      BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
  BT_GATT_CHARACTERISTIC(VELAWEAR_BLE_LLM_RESPONSE_UUID,
                         BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE,
                         NULL, velawear_ble_write_llm_response, NULL),
  BT_GATT_CHARACTERISTIC(VELAWEAR_BLE_AGENT_MESSAGE_UUID,
                         BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ,
                         NULL, NULL, NULL),
  BT_GATT_CCC_MANAGED(
      ((struct _bt_gatt_ccc[]) {
        BT_GATT_CCC_INITIALIZER(NULL, velawear_ble_agent_message_ccc_write, NULL)
      }),
      BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
  BT_GATT_CHARACTERISTIC(VELAWEAR_BLE_AUDIO_STREAM_UUID,
                         BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                         BT_GATT_PERM_WRITE,
                         NULL, velawear_ble_write_audio_stream, NULL),
  BT_GATT_CHARACTERISTIC(VELAWEAR_BLE_CHAT_INPUT_UUID,
                         BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                         BT_GATT_PERM_WRITE,
                         NULL, velawear_ble_write_chat_input, NULL)
);

static void velawear_ble_notify_current_status(void)
{
  uint8_t value[3];
  struct bt_conn *conn;

  pthread_mutex_lock(&g_lock);
  value[0] = (uint8_t)g_motion_type;
  value[1] = g_intensity_percent;
  value[2] = g_alert_active ? 1 : 0;
  conn = g_status_notify_conn != NULL ? bt_conn_ref(g_status_notify_conn) : NULL;
  pthread_mutex_unlock(&g_lock);

  if (conn == NULL)
    {
      return;
    }

  (void)bt_gatt_notify(conn, &attr_velawear_svc[VELAWEAR_BLE_STATUS_ATTR_INDEX],
                       value, sizeof(value));
  bt_conn_unref(conn);
}

static int velawear_ble_notify_agent_packet(uint16_t attr_index,
                                            const uint8_t *frame,
                                            uint16_t frame_size)
{
  bool notify_enabled;
  struct bt_conn *notify_conn;
  struct bt_conn *conn;
  int ret;

  pthread_mutex_lock(&g_lock);
  if (attr_index == VELAWEAR_BLE_AGENT_EVENT_ATTR_INDEX)
    {
      notify_enabled = g_agent_event_notify_enabled;
      notify_conn = g_agent_event_notify_conn;
    }
  else if (attr_index == VELAWEAR_BLE_AGENT_RESULT_ATTR_INDEX)
    {
      notify_enabled = g_agent_result_notify_enabled;
      notify_conn = g_agent_result_notify_conn;
    }
  else if (attr_index == VELAWEAR_BLE_AGENT_MESSAGE_ATTR_INDEX)
    {
      notify_enabled = g_agent_message_notify_enabled;
      notify_conn = g_agent_message_notify_conn;
    }
  else
    {
      pthread_mutex_unlock(&g_lock);
      return VELAWEAR_ERR_INVAL;
    }

  conn = notify_enabled && notify_conn != NULL ? bt_conn_ref(notify_conn) : NULL;
  pthread_mutex_unlock(&g_lock);
  if (conn == NULL)
    {
      return VELAWEAR_OK;
    }

  ret = bt_gatt_notify(conn, &attr_velawear_svc[attr_index], frame, frame_size);
  bt_conn_unref(conn);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "[BLE] Agent notification failed: %d\n", ret);
      return ret;
    }

  return VELAWEAR_OK;
}

static uint8_t velawear_ble_percent_from_intensity(float intensity)
{
  if (intensity <= 0.0f)
    {
      return 0;
    }

  if (intensity >= 1.0f)
    {
      return 100;
    }

  return (uint8_t)(intensity * 100.0f);
}

static uint16_t velawear_ble_u16_saturate(uint32_t value)
{
  return value > UINT16_MAX ? UINT16_MAX : (uint16_t)value;
}

int velawear_ble_publish_agent_event(const velawear_event_t *event)
{
  velawear_agent_event_t agent_event;
  uint8_t frame[VELAWEAR_AGENT_EVENT_FRAME_SIZE];
  int ret;

  if (event == NULL || !g_initialized)
    {
      return VELAWEAR_ERR_INVAL;
    }

  memset(&agent_event, 0, sizeof(agent_event));
  switch (event->type)
    {
      case VELAWEAR_EVENT_MOTION:
        agent_event.event_type = VELAWEAR_AGENT_EVENT_MOTION;
        agent_event.intensity_percent =
            velawear_ble_percent_from_intensity(event->data.motion.intensity);
        break;

      case VELAWEAR_EVENT_FALL:
        agent_event.event_type = VELAWEAR_AGENT_EVENT_FALL;
        agent_event.priority = VELAWEAR_AGENT_PRIORITY_CRITICAL;
        agent_event.intensity_percent = 100;
        break;

      case VELAWEAR_EVENT_AUDIO:
        agent_event.event_type = VELAWEAR_AGENT_EVENT_AUDIO;
        agent_event.intensity_percent = event->data.audio.active ? 100 : 0;
        agent_event.value = velawear_ble_u16_saturate(event->data.audio.avg_abs);
        agent_event.peak = velawear_ble_u16_saturate(event->data.audio.peak_abs);
        if (event->data.audio.active)
          {
            agent_event.flags |= VELAWEAR_AGENT_EVENT_FLAG_ACTIVE;
          }
        break;

      case VELAWEAR_EVENT_TIMER:
        agent_event.event_type = VELAWEAR_AGENT_EVENT_SEDENTARY;
        break;

      default:
        return VELAWEAR_ERR_NOSUPPORT;
    }

  pthread_mutex_lock(&g_lock);
  agent_event.sequence = g_agent_event_sequence++;
  if (g_alert_active)
    {
      agent_event.flags |= VELAWEAR_AGENT_EVENT_FLAG_ALERT;
    }

  ret = velawear_agent_encode_event(&agent_event, frame, sizeof(frame));
  pthread_mutex_unlock(&g_lock);
  if (ret < 0)
    {
      return ret;
    }

  return velawear_ble_notify_agent_packet(VELAWEAR_BLE_AGENT_EVENT_ATTR_INDEX,
                                          frame, (uint16_t)ret);
}

void velawear_ble_report_agent_command_result(uint16_t sequence,
                                              uint8_t command_id,
                                              uint8_t result)
{
  uint8_t frame[VELAWEAR_AGENT_RESULT_FRAME_SIZE];
  int ret;

  if (!g_initialized)
    {
      return;
    }

  ret = velawear_agent_encode_command_result(sequence, command_id, result,
                                             frame, sizeof(frame));
  if (ret < 0)
    {
      return;
    }

  (void)velawear_ble_notify_agent_packet(VELAWEAR_BLE_AGENT_RESULT_ATTR_INDEX,
                                         frame, (uint16_t)ret);
}

int velawear_ble_send_message(const char *text, uint32_t priority)
{
  uint8_t frame[VELAWEAR_AGENT_MESSAGE_FRAME_MAX];
  uint16_t sequence;
  int frame_size;

  if (text == NULL || !g_initialized)
    {
      return VELAWEAR_ERR_INVAL;
    }

  pthread_mutex_lock(&g_lock);
  sequence = g_agent_message_sequence++;
  frame_size = velawear_agent_encode_message(
      sequence, (uint8_t)(priority > UINT8_MAX ? UINT8_MAX : priority),
      text, frame, sizeof(frame));
  pthread_mutex_unlock(&g_lock);
  if (frame_size < 0)
    {
      return frame_size;
    }

  return velawear_ble_notify_agent_packet(
      VELAWEAR_BLE_AGENT_MESSAGE_ATTR_INDEX, frame, (uint16_t)frame_size);
}

int velawear_ble_request_llm(const char *prompt, char *response,
                              size_t response_size, uint32_t timeout_ms)
{
  uint8_t frame[VELAWEAR_AGENT_LLM_REQUEST_FRAME_MAX];
  struct bt_conn *conn;
  uint16_t sequence;
  uint16_t response_length;
  uint8_t response_status;
  size_t copy_length;
  int frame_size;
  int ret;

  if (prompt == NULL || response == NULL || response_size == 0 ||
      timeout_ms == 0)
    {
      return -EINVAL;
    }

  frame_size = velawear_agent_encode_llm_request(
      0, prompt, frame, sizeof(frame));
  if (frame_size < 0)
    {
      return frame_size;
    }

  pthread_mutex_lock(&g_lock);
  if (!g_initialized || !g_llm_request_notify_enabled ||
      g_llm_request_notify_conn == NULL)
    {
      pthread_mutex_unlock(&g_lock);
      return -ENOTCONN;
    }

  if (g_llm_waiting)
    {
      pthread_mutex_unlock(&g_lock);
      return -EBUSY;
    }

  while (k_sem_take(&g_llm_response_sem, K_NO_WAIT) == 0)
    {
    }

  sequence = g_llm_sequence++;
  if (g_llm_sequence == 0)
    {
      g_llm_sequence = 1;
    }

  frame[2] = (uint8_t)(sequence & 0xff);
  frame[3] = (uint8_t)(sequence >> 8);
  g_llm_pending_sequence = sequence;
  g_llm_response_status = VELAWEAR_AGENT_LLM_STATUS_ERROR;
  g_llm_response_length = 0;
  g_llm_response_text[0] = '\0';
  g_llm_waiting = true;
  conn = bt_conn_ref(g_llm_request_notify_conn);
  pthread_mutex_unlock(&g_lock);

  ret = bt_gatt_notify(conn,
                       &attr_velawear_svc[VELAWEAR_BLE_LLM_REQUEST_ATTR_INDEX],
                       frame, (uint16_t)frame_size);
  bt_conn_unref(conn);
  if (ret < 0)
    {
      pthread_mutex_lock(&g_lock);
      if (g_llm_pending_sequence == sequence)
        {
          g_llm_waiting = false;
        }
      pthread_mutex_unlock(&g_lock);
      syslog(LOG_WARNING, "[BLE] LLM request notify failed: %d\n", ret);
      return ret;
    }

  ret = k_sem_take(&g_llm_response_sem, K_MSEC(timeout_ms));
  if (ret < 0)
    {
      pthread_mutex_lock(&g_lock);
      if (g_llm_pending_sequence == sequence)
        {
          g_llm_waiting = false;
        }
      pthread_mutex_unlock(&g_lock);
      syslog(LOG_WARNING, "[BLE] LLM response timeout: seq=%u\n",
             (unsigned int)sequence);
      return -ETIMEDOUT;
    }

  pthread_mutex_lock(&g_lock);
  response_status = g_llm_response_status;
  response_length = g_llm_response_length;
  copy_length = response_length < response_size - 1 ?
                response_length : response_size - 1;
  memcpy(response, g_llm_response_text, copy_length);
  response[copy_length] = '\0';
  pthread_mutex_unlock(&g_lock);

  if (response_status != VELAWEAR_AGENT_LLM_STATUS_OK)
    {
      return -EIO;
    }

  return 0;
}

static const struct bt_data g_advertising_data[] =
{
  BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
  BT_DATA_BYTES(BT_DATA_UUID128_ALL,
                BT_UUID_128_ENCODE(0x12345678, 0x9abc, 0xdef0,
                                   0x1234, 0x56789abcdef0)),
};

static const struct bt_data g_scan_response_data[] =
{
  BT_DATA(BT_DATA_NAME_COMPLETE, "VelaWear", sizeof("VelaWear") - 1),
};

/* SF32LB52 controller path: use legacy connectable advertising with the
 * controller identity address instead of extended advertising/random RPA.
 */
static const struct bt_le_adv_param g_advertising_param =
{
  .id = 0,
  .sid = 0,
  .secondary_max_skip = 0,
  .options = BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_USE_IDENTITY,
  .interval_min = BT_GAP_ADV_FAST_INT_MIN_1,
  .interval_max = BT_GAP_ADV_FAST_INT_MAX_1,
  .peer = NULL,
};

static void velawear_ble_log_identity(void)
{
  bt_addr_le_t identity;
  size_t count = 1;
  char address[BT_ADDR_LE_STR_LEN];

  memset(&identity, 0, sizeof(identity));
  memset(address, 0, sizeof(address));
  bt_id_get(&identity, &count);
  if (count == 0)
    {
      syslog(LOG_WARNING, "[BLE] controller returned no identity address\n");
      return;
    }

  bt_addr_le_to_str(&identity, address, sizeof(address));
  syslog(LOG_INFO, "[BLE] identity count=%u addr=%s\n",
         (unsigned int)count, address);
}

static void velawear_ble_publish_state(bool connected)
{
  velawear_event_t event;

  if (g_events == NULL)
    {
      return;
    }

  memset(&event, 0, sizeof(event));
  event.type = VELAWEAR_EVENT_BLE_STATE;
  event.priority = VELAWEAR_PRIORITY_NORMAL;
  event.timestamp = (uint32_t)k_uptime_get_32();
  event.data.ble_state.connected = connected;
  if (connected)
    {
      strncpy(event.data.ble_state.device_name, "BLE controller",
              sizeof(event.data.ble_state.device_name) - 1);
    }

  if (event_manager_push(g_events, &event) < 0)
    {
      syslog(LOG_WARNING, "[BLE] Failed to queue connection state=%d\n",
             connected ? 1 : 0);
    }
}

static void velawear_ble_connected(struct bt_conn *conn, uint8_t err)
{
  (void)conn;
  syslog(LOG_INFO, "[BLE] connected err=0x%02x\n", err);
  velawear_ble_publish_state(err == 0);
}

static void velawear_ble_disconnected(struct bt_conn *conn, uint8_t reason)
{
  struct bt_conn *status_conn;
  struct bt_conn *event_conn;
  struct bt_conn *result_conn;
  struct bt_conn *message_conn;
  struct bt_conn *llm_conn;
  bool wake_llm;

  (void)conn;
  velawear_ble_publish_state(false);
  pthread_mutex_lock(&g_lock);
  status_conn = g_status_notify_conn;
  event_conn = g_agent_event_notify_conn;
  result_conn = g_agent_result_notify_conn;
  message_conn = g_agent_message_notify_conn;
  llm_conn = g_llm_request_notify_conn;
  wake_llm = g_llm_waiting;
  g_status_notify_conn = NULL;
  g_agent_event_notify_conn = NULL;
  g_agent_result_notify_conn = NULL;
  g_agent_message_notify_conn = NULL;
  g_llm_request_notify_conn = NULL;
  g_status_notify_enabled = false;
  g_agent_event_notify_enabled = false;
  g_agent_result_notify_enabled = false;
  g_agent_message_notify_enabled = false;
  g_llm_request_notify_enabled = false;
  g_llm_waiting = false;
  pthread_mutex_unlock(&g_lock);

  if (wake_llm)
    {
      k_sem_give(&g_llm_response_sem);
    }

  if (status_conn != NULL)
    {
      bt_conn_unref(status_conn);
    }

  if (event_conn != NULL)
    {
      bt_conn_unref(event_conn);
    }

  if (result_conn != NULL)
    {
      bt_conn_unref(result_conn);
    }

  if (message_conn != NULL)
    {
      bt_conn_unref(message_conn);
    }

  if (llm_conn != NULL)
    {
      bt_conn_unref(llm_conn);
    }

  syslog(LOG_INFO,
         "[BLE] disconnected reason=0x%02x; persistent advertiser resumes after slot release\n",
         reason);
}

static struct bt_conn_cb velawear_conn_callbacks =
{
  .connected = velawear_ble_connected,
  .disconnected = velawear_ble_disconnected,
};

int velawear_ble_init(velawear_config_t *config, velawear_events_t *events)
{
  int ret;

  if (config == NULL || events == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  g_config = config;
  g_events = events;
  ret = pthread_mutex_init(&g_lock, NULL);
  if (ret != 0)
    {
      g_config = NULL;
      g_events = NULL;
      return -ret;
    }

  g_alert_active = false;
  g_status_notify_enabled = false;
  g_status_notify_conn = NULL;
  g_agent_event_notify_enabled = false;
  g_agent_event_notify_conn = NULL;
  g_agent_result_notify_enabled = false;
  g_agent_result_notify_conn = NULL;
  g_agent_message_notify_enabled = false;
  g_agent_message_notify_conn = NULL;
  g_llm_request_notify_enabled = false;
  g_llm_request_notify_conn = NULL;
  g_llm_waiting = false;
  g_llm_sequence = 1;
  g_llm_pending_sequence = 0;
  g_llm_response_status = VELAWEAR_AGENT_LLM_STATUS_ERROR;
  g_llm_response_length = 0;
  g_llm_response_text[0] = '\0';
  (void)k_sem_init(&g_llm_response_sem, 0, 1);
  g_agent_event_sequence = 0;
  g_agent_message_sequence = 0;
  g_last_agent_command_sequence = 0;
  g_have_last_agent_command_sequence = false;
  k_work_init(&g_status_notify_work, velawear_ble_notify_work);
  g_motion_type = 0;
  g_intensity_percent = 0;

  ret = bt_enable(NULL);
  if (ret < 0)
    {
      pthread_mutex_destroy(&g_lock);
      g_config = NULL;
      g_events = NULL;
      syslog(LOG_WARNING, "[BLE] Bluetooth init failed: %d\n", ret);
      return ret;
    }

  ret = bt_conn_cb_register(&velawear_conn_callbacks);
  if (ret < 0)
    {
      pthread_mutex_destroy(&g_lock);
      g_config = NULL;
      g_events = NULL;
      syslog(LOG_WARNING, "[BLE] Connection callback registration failed: %d\n",
             ret);
      return ret;
    }

  velawear_ble_log_identity();
  ret = bt_le_adv_start(&g_advertising_param,
                        g_advertising_data, ARRAY_SIZE(g_advertising_data),
                        g_scan_response_data, ARRAY_SIZE(g_scan_response_data));
  syslog(LOG_INFO, "[BLE] advertising start ret=%d options=0x%02x\n", ret,
         (unsigned int)g_advertising_param.options);
  if (ret < 0)
    {
      pthread_mutex_destroy(&g_lock);
      g_config = NULL;
      g_events = NULL;
      syslog(LOG_WARNING, "[BLE] Advertising start failed: %d\n", ret);
      return ret;
    }

  g_initialized = true;
  syslog(LOG_INFO, "[BLE] VelaWear GATT service registered; advertising\n");
  return VELAWEAR_OK;
}

void velawear_ble_cleanup(void)
{
  struct bt_conn *status_conn;
  struct bt_conn *event_conn;
  struct bt_conn *result_conn;
  struct bt_conn *message_conn;
  struct bt_conn *llm_conn;
  bool wake_llm;

  if (!g_initialized)
    {
      return;
    }

  g_initialized = false;
  pthread_mutex_lock(&g_lock);
  status_conn = g_status_notify_conn;
  event_conn = g_agent_event_notify_conn;
  result_conn = g_agent_result_notify_conn;
  message_conn = g_agent_message_notify_conn;
  llm_conn = g_llm_request_notify_conn;
  wake_llm = g_llm_waiting;
  g_status_notify_enabled = false;
  g_agent_event_notify_enabled = false;
  g_agent_result_notify_enabled = false;
  g_agent_message_notify_enabled = false;
  g_llm_request_notify_enabled = false;
  g_llm_waiting = false;
  g_status_notify_conn = NULL;
  g_agent_event_notify_conn = NULL;
  g_agent_result_notify_conn = NULL;
  g_agent_message_notify_conn = NULL;
  g_llm_request_notify_conn = NULL;
  pthread_mutex_unlock(&g_lock);

  if (status_conn != NULL)
    {
      bt_conn_unref(status_conn);
    }

  if (event_conn != NULL)
    {
      bt_conn_unref(event_conn);
    }

  if (result_conn != NULL)
    {
      bt_conn_unref(result_conn);
    }

  if (message_conn != NULL)
    {
      bt_conn_unref(message_conn);
    }

  if (llm_conn != NULL)
    {
      bt_conn_unref(llm_conn);
    }

  if (wake_llm)
    {
      k_sem_give(&g_llm_response_sem);
    }

  pthread_mutex_destroy(&g_lock);
  g_config = NULL;
  g_events = NULL;
}

void velawear_ble_update_motion(int motion_type, float intensity)
{
  bool notify;
  if (!g_initialized)
    {
      return;
    }

  if (intensity < 0.0f)
    {
      intensity = 0.0f;
    }
  if (intensity > 1.0f)
    {
      intensity = 1.0f;
    }

  pthread_mutex_lock(&g_lock);
  g_motion_type = motion_type;
  g_intensity_percent = (uint8_t)(intensity * 100.0f);
  notify = g_status_notify_enabled;
  pthread_mutex_unlock(&g_lock);

  if (notify)
    {
      velawear_ble_notify_current_status();
    }
}

void velawear_ble_set_alert(bool active)
{
  bool notify;
  if (!g_initialized)
    {
      return;
    }

  pthread_mutex_lock(&g_lock);
  g_alert_active = active;
  notify = g_status_notify_enabled;
  pthread_mutex_unlock(&g_lock);

  if (notify)
    {
      velawear_ble_notify_current_status();
    }
}
