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
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/uuid.h>

#include "velawear_ble.h"

#define VELAWEAR_BLE_SERVICE_UUID \
  BT_UUID_DECLARE_128(0x12345678, 0x9abc, 0xdef0, 0x1234, 0x56789abcdef0)

#define VELAWEAR_BLE_STATUS_UUID \
  BT_UUID_DECLARE_128(0x12345679, 0x9abc, 0xdef0, 0x1234, 0x56789abcdef0)

#define VELAWEAR_BLE_THRESHOLD_UUID \
  BT_UUID_DECLARE_128(0x1234567a, 0x9abc, 0xdef0, 0x1234, 0x56789abcdef0)

static velawear_config_t *g_config;
static pthread_mutex_t g_lock;
static bool g_initialized;
static bool g_alert_active;
static int g_motion_type;
static uint8_t g_intensity_percent;

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

  pthread_mutex_lock(&g_lock);
  g_config->sedentary_threshold_sec = seconds;
  pthread_mutex_unlock(&g_lock);
  syslog(LOG_INFO, "[BLE] Sedentary threshold updated: %u s\n",
         (unsigned int)seconds);
  return len;
}

static void velawear_ble_status_ccc_changed(const struct bt_gatt_attr *attr,
                                            uint16_t value)
{
  (void)attr;
  syslog(LOG_INFO, "[BLE] Status notifications %s\n",
         (value == BT_GATT_CCC_NOTIFY) ? "enabled" : "disabled");
}

/* Static registration is performed by the Zblue host during bt_enable(). */
BT_GATT_SERVICE_DEFINE(velawear_svc,
  BT_GATT_PRIMARY_SERVICE(VELAWEAR_BLE_SERVICE_UUID),
  BT_GATT_CHARACTERISTIC(VELAWEAR_BLE_STATUS_UUID,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                         BT_GATT_PERM_READ,
                         velawear_ble_read_status, NULL, NULL),
  BT_GATT_CCC(velawear_ble_status_ccc_changed,
              BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
  BT_GATT_CHARACTERISTIC(VELAWEAR_BLE_THRESHOLD_UUID,
                         BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                         BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                         velawear_ble_read_threshold,
                         velawear_ble_write_threshold, NULL)
);

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

int velawear_ble_init(velawear_config_t *config)
{
  int ret;

  if (config == NULL)
    {
      return VELAWEAR_ERR_INVAL;
    }

  g_config = config;
  ret = pthread_mutex_init(&g_lock, NULL);
  if (ret != 0)
    {
      g_config = NULL;
      return -ret;
    }

  g_alert_active = false;
  g_motion_type = 0;
  g_intensity_percent = 0;

  ret = bt_enable(NULL);
  if (ret < 0)
    {
      pthread_mutex_destroy(&g_lock);
      g_config = NULL;
      syslog(LOG_WARNING, "[BLE] Bluetooth init failed: %d\n", ret);
      return ret;
    }

  ret = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
                        g_advertising_data, ARRAY_SIZE(g_advertising_data),
                        g_scan_response_data, ARRAY_SIZE(g_scan_response_data));
  if (ret < 0)
    {
      pthread_mutex_destroy(&g_lock);
      g_config = NULL;
      syslog(LOG_WARNING, "[BLE] Advertising start failed: %d\n", ret);
      return ret;
    }

  g_initialized = true;
  syslog(LOG_INFO, "[BLE] VelaWear GATT service registered; advertising\n");
  return VELAWEAR_OK;
}

void velawear_ble_cleanup(void)
{
  if (!g_initialized)
    {
      return;
    }

  g_initialized = false;
  pthread_mutex_destroy(&g_lock);
  g_config = NULL;
}

void velawear_ble_update_motion(int motion_type, float intensity)
{
  uint8_t value[3];

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
  value[0] = (uint8_t)g_motion_type;
  value[1] = g_intensity_percent;
  value[2] = g_alert_active ? 1 : 0;
  pthread_mutex_unlock(&g_lock);

  (void)bt_gatt_notify(NULL, &attr_velawear_svc[2], value, sizeof(value));
}

void velawear_ble_set_alert(bool active)
{
  uint8_t value[3];

  if (!g_initialized)
    {
      return;
    }

  pthread_mutex_lock(&g_lock);
  g_alert_active = active;
  value[0] = (uint8_t)g_motion_type;
  value[1] = g_intensity_percent;
  value[2] = g_alert_active ? 1 : 0;
  pthread_mutex_unlock(&g_lock);

  (void)bt_gatt_notify(NULL, &attr_velawear_svc[2], value, sizeof(value));
}
