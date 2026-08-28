/*
 * VelaWear XiaoZhi client for NuttX.
 *
 * This is a native NuttX transport adapter for the public protocol used by
 * 78/xiaozhi-sf32.  The upstream application uses RT-Thread and its own
 * websocket client; this file keeps the protocol and activation flow while
 * using the networking and threading primitives already present in openvela.
 */

#include "xiaozhi_internal.h"

#include <nuttx/config.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <syslog.h>
#include <unistd.h>

#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include <cJSON.h>

#ifdef CONFIG_BT
#  include <zephyr/bluetooth/bluetooth.h>
#  include <zephyr/bluetooth/addr.h>
#endif

#ifdef CONFIG_VELAWEAR_XIAOZHI_AUDIO
#  include "xiaozhi_audio.h"
#endif
#ifdef CONFIG_VELAWEAR_XIAOZHI_PAN
#  include "../drivers/velawear_pan.h"
#endif

#ifndef CONFIG_VELAWEAR_XIAOZHI_HOST
#  define CONFIG_VELAWEAR_XIAOZHI_HOST XIAOZHI_DEFAULT_HOST
#endif
#ifndef CONFIG_VELAWEAR_XIAOZHI_PATH
#  define CONFIG_VELAWEAR_XIAOZHI_PATH XIAOZHI_DEFAULT_PATH
#endif
#ifndef CONFIG_VELAWEAR_XIAOZHI_TOKEN
#  define CONFIG_VELAWEAR_XIAOZHI_TOKEN XIAOZHI_DEFAULT_TOKEN
#endif
#ifndef CONFIG_VELAWEAR_XIAOZHI_DEVICE_ID
#  define CONFIG_VELAWEAR_XIAOZHI_DEVICE_ID ""
#endif

#define XZ_TAG "XiaoZhi"
#define XZ_DEFAULT_PORT 443
#define XZ_OTA_RESPONSE_MAX 8192
#define XZ_HTTP_REQUEST_MAX 4096
#define XZ_WS_HEADER_MAX 2048
#define XZ_RECONNECT_DELAY_SEC 3
#define XZ_ACTIVATION_POLL_SEC 10
#define XZ_READ_TIMEOUT 1

static struct xiaozhi_client g_xz;
static pthread_mutex_t g_xz_io_lock = PTHREAD_MUTEX_INITIALIZER;

static void xz_copy_string(char *dst, size_t dst_size, const char *src)
{
  if (dst == NULL || dst_size == 0)
    {
      return;
    }

  if (src == NULL)
    {
      dst[0] = '\0';
      return;
    }

  strncpy(dst, src, dst_size - 1);
  dst[dst_size - 1] = '\0';
}

static void xz_sleep_interruptible(struct xiaozhi_client *client,
                                   unsigned int seconds)
{
  unsigned int i;

  for (i = 0; i < seconds && !client->stop_requested; i++)
    {
      sleep(1);
    }
}

static void xz_set_state(struct xiaozhi_client *client,
                         xiaozhi_client_state_t state)
{
  client->state = state;
  syslog(LOG_INFO, "[%s] state=%d\n", XZ_TAG, (int)state);
}

static void xz_format_mac(const uint8_t *addr, char *out, size_t out_size)
{
  if (addr == NULL || out == NULL || out_size < 18)
    {
      return;
    }

  /* Bluetooth addresses are stored little-endian in the framework. */
  snprintf(out, out_size, "%02x:%02x:%02x:%02x:%02x:%02x",
           addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

static int xz_prepare_identity(struct xiaozhi_client *client)
{
  uint8_t digest[32];
  mbedtls_sha256_context sha;
  const char *configured = CONFIG_VELAWEAR_XIAOZHI_DEVICE_ID;
  bool have_mac = false;

  if (configured != NULL && configured[0] != '\0')
    {
      xz_copy_string(client->device_id, sizeof(client->device_id),
                     configured);
      have_mac = true;
    }

#ifdef CONFIG_BT
  if (!have_mac)
    {
      bt_addr_le_t identity;
      size_t count = 1;

      memset(&identity, 0, sizeof(identity));
      bt_id_get(&identity, &count);
      if (count > 0)
        {
          xz_format_mac(identity.a.val, client->device_id,
                        sizeof(client->device_id));
          have_mac = true;
        }
    }
#endif

  if (!have_mac)
    {
      /* This is only a build/runtime fallback.  A real board with BT enabled
       * should take the controller address path above. */
      xz_copy_string(client->device_id, sizeof(client->device_id),
                     "02:00:00:52:00:01");
      syslog(LOG_WARNING, "[%s] Bluetooth address unavailable; using fallback "
             "device id %s\n", XZ_TAG, client->device_id);
    }

  /* XiaoZhi expects a UUID-shaped client id.  Derive a stable one from the
   * board address without pulling in chip-specific hash HAL code. */
  memset(digest, 0, sizeof(digest));
  mbedtls_sha256_init(&sha);
  if (mbedtls_sha256_starts(&sha, 0) == 0 &&
      mbedtls_sha256_update(&sha,
                            (const unsigned char *)client->device_id,
                            strlen(client->device_id)) == 0 &&
      mbedtls_sha256_finish(&sha, digest) == 0)
    {
      snprintf(client->client_id, sizeof(client->client_id),
               "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
               "%02x%02x%02x%02x%02x%02x",
               digest[0], digest[1], digest[2], digest[3],
               digest[4], digest[5], digest[6], digest[7],
               digest[8], digest[9], digest[10], digest[11],
               digest[12], digest[13], digest[14], digest[15]);
    }
  else
    {
      snprintf(client->client_id, sizeof(client->client_id),
               "00000000-0000-4000-8000-%02x%02x%02x%02x%02x%02x",
               client->device_id[0], client->device_id[1],
               client->device_id[3], client->device_id[4],
               client->device_id[6], client->device_id[7]);
    }
  mbedtls_sha256_free(&sha);

  syslog(LOG_INFO, "[%s] identity device=%s client=%s\n", XZ_TAG,
         client->device_id, client->client_id);
  return 0;
}

static void xz_set_default_endpoint(struct xiaozhi_client *client)
{
  xz_copy_string(client->host, sizeof(client->host),
                 CONFIG_VELAWEAR_XIAOZHI_HOST);
  xz_copy_string(client->path, sizeof(client->path),
                 CONFIG_VELAWEAR_XIAOZHI_PATH);
  xz_copy_string(client->token, sizeof(client->token),
                 CONFIG_VELAWEAR_XIAOZHI_TOKEN);
  client->port = XZ_DEFAULT_PORT;
  client->endpoint_ready = true;
}

static int xz_apply_websocket_url(struct xiaozhi_client *client,
                                  const char *url)
{
  const char *authority;
  const char *slash;
  char authority_copy[XIAOZHI_HOST_MAX];
  char *colon;
  size_t authority_len;
  int port = XZ_DEFAULT_PORT;

  if (url == NULL || url[0] == '\0')
    {
      return -EINVAL;
    }

  if (strncmp(url, "wss://", 6) == 0)
    {
      authority = url + 6;
    }
  else if (strncmp(url, "ws://", 5) == 0)
    {
      /* The public endpoint is normally wss.  Keep TLS enabled for the
       * NuttX port even if an OTA response omits the secure scheme. */
      authority = url + 5;
      port = 80;
      syslog(LOG_WARNING, "[%s] OTA returned ws://; TLS transport is still "
             "used\n", XZ_TAG);
    }
  else
    {
      return -ENOTSUP;
    }

  slash = strchr(authority, '/');
  authority_len = slash == NULL ? strlen(authority) :
                                  (size_t)(slash - authority);
  if (authority_len == 0 || authority_len >= sizeof(authority_copy))
    {
      return -EINVAL;
    }

  memcpy(authority_copy, authority, authority_len);
  authority_copy[authority_len] = '\0';
  colon = strrchr(authority_copy, ':');
  if (colon != NULL && colon != authority_copy)
    {
      *colon = '\0';
      port = atoi(colon + 1);
      if (port <= 0 || port > 65535)
        {
          return -EINVAL;
        }
    }

  xz_copy_string(client->host, sizeof(client->host), authority_copy);
  if (slash != NULL)
    {
      xz_copy_string(client->path, sizeof(client->path), slash);
    }
  else
    {
      xz_copy_string(client->path, sizeof(client->path),
                     XIAOZHI_DEFAULT_PATH);
    }
  client->port = port;
  client->endpoint_ready = true;
  syslog(LOG_INFO, "[%s] endpoint wss://%s:%d%s\n", XZ_TAG,
         client->host, client->port, client->path);
  return 0;
}

static void xz_tls_init_context(xiaozhi_tls_t *tls)
{
  memset(tls, 0, sizeof(*tls));
  mbedtls_net_init(&tls->net);
  mbedtls_ssl_init(&tls->ssl);
  mbedtls_ssl_config_init(&tls->conf);
  mbedtls_entropy_init(&tls->entropy);
  mbedtls_ctr_drbg_init(&tls->drbg);
}

static void xz_tls_close(xiaozhi_tls_t *tls)
{
  if (tls == NULL)
    {
      return;
    }

  if (tls->ssl_ready)
    {
      (void)mbedtls_ssl_close_notify(&tls->ssl);
    }
  mbedtls_ssl_free(&tls->ssl);
  mbedtls_ssl_config_free(&tls->conf);
  mbedtls_ctr_drbg_free(&tls->drbg);
  mbedtls_entropy_free(&tls->entropy);
  mbedtls_net_free(&tls->net);
  tls->initialized = false;
  tls->ssl_ready = false;
}

static int xz_tls_open(xiaozhi_tls_t *tls, const char *host, int port)
{
  char port_string[8];
  const char personalization[] = "velawear-xiaozhi";
  struct timeval timeout;
  int ret;

  xz_tls_init_context(tls);
  snprintf(port_string, sizeof(port_string), "%d", port);

  ret = mbedtls_ctr_drbg_seed(&tls->drbg, mbedtls_entropy_func,
                              &tls->entropy,
                              (const unsigned char *)personalization,
                              sizeof(personalization) - 1);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[%s] RNG seed failed: -0x%04x\n", XZ_TAG, -ret);
      goto fail;
    }

  ret = mbedtls_net_connect(&tls->net, host, port_string,
                            MBEDTLS_NET_PROTO_TCP);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[%s] TCP connect %s:%d failed: -0x%04x\n", XZ_TAG,
             host, port, -ret);
      goto fail;
    }

  mbedtls_net_set_block(&tls->net);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  (void)setsockopt(tls->net.fd, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout));
  (void)setsockopt(tls->net.fd, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout));

  ret = mbedtls_ssl_config_defaults(&tls->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[%s] TLS defaults failed: -0x%04x\n", XZ_TAG, -ret);
      goto fail;
    }

  /* The current Huangshan image does not ship a CA bundle.  Keep hostname
   * and SNI enabled, but use optional verification until a board CA store is
   * added; the transport still uses TLS encryption. */
  mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
  mbedtls_ssl_conf_rng(&tls->conf, mbedtls_ctr_drbg_random, &tls->drbg);

  ret = mbedtls_ssl_setup(&tls->ssl, &tls->conf);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[%s] TLS setup failed: -0x%04x\n", XZ_TAG, -ret);
      goto fail;
    }

  ret = mbedtls_ssl_set_hostname(&tls->ssl, host);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[%s] TLS hostname failed: -0x%04x\n", XZ_TAG, -ret);
      goto fail;
    }

  mbedtls_ssl_set_bio(&tls->ssl, &tls->net, mbedtls_net_send,
                      mbedtls_net_recv, NULL);
  tls->initialized = true;
  tls->ssl_ready = true;

  do
    {
      ret = mbedtls_ssl_handshake(&tls->ssl);
    }
  while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
         ret == MBEDTLS_ERR_SSL_WANT_WRITE);

  if (ret != 0)
    {
      syslog(LOG_ERR, "[%s] TLS handshake failed: -0x%04x\n", XZ_TAG, -ret);
      goto fail;
    }

  return 0;

fail:
  xz_tls_close(tls);
  return -EIO;
}

static int xz_tls_write_all(xiaozhi_tls_t *tls, const void *buffer,
                            size_t length)
{
  const unsigned char *data = buffer;
  size_t written = 0;

  while (written < length)
    {
      int ret = mbedtls_ssl_write(&tls->ssl, data + written,
                                  length - written);
      if (ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
          ret == MBEDTLS_ERR_SSL_WANT_READ)
        {
          continue;
        }
      if (ret <= 0)
        {
          return -EIO;
        }
      written += (size_t)ret;
    }

  return 0;
}

static int xz_tls_read_some(xiaozhi_tls_t *tls, void *buffer, size_t length)
{
  for (;;)
    {
      int ret = mbedtls_ssl_read(&tls->ssl, buffer, length);
      if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
          ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          return XZ_READ_TIMEOUT;
        }
      if (ret == 0 || ret < 0)
        {
          return -EIO;
        }
      return ret;
    }
}

static int xz_tls_read_exact(struct xiaozhi_client *client, void *buffer,
                             size_t length)
{
  unsigned char *data = buffer;
  size_t received = 0;

  while (received < length)
    {
      int ret;

      if (client->stop_requested)
        {
          return -ECANCELED;
        }

      ret = xz_tls_read_some(&client->tls, data + received,
                             length - received);
      if (ret == XZ_READ_TIMEOUT)
        {
          return received == 0 ? XZ_READ_TIMEOUT : -EIO;
        }
      if (ret < 0)
        {
          return ret;
        }
      received += (size_t)ret;
    }

  return 0;
}

static int xz_ws_send_frame(struct xiaozhi_client *client, uint8_t opcode,
                            const void *data, size_t length)
{
  uint8_t header[14];
  uint8_t mask[4];
  uint8_t *frame;
  size_t header_length;
  size_t total_length;
  size_t i;
  int ret;

  if (!client->connected || !client->tls.ssl_ready ||
      (data == NULL && length != 0))
    {
      return -ENOTCONN;
    }

  if (length > 0xffff)
    {
      return -E2BIG;
    }

  header[0] = 0x80 | (opcode & 0x0f);
  if (length < 126)
    {
      header[1] = (uint8_t)(0x80 | length);
      header_length = 2;
    }
  else
    {
      header[1] = 0x80 | 126;
      header[2] = (uint8_t)(length >> 8);
      header[3] = (uint8_t)length;
      header_length = 4;
    }

  ret = mbedtls_ctr_drbg_random(&client->tls.drbg, mask, sizeof(mask));
  if (ret != 0)
    {
      return -EIO;
    }
  memcpy(header + header_length, mask, sizeof(mask));
  header_length += sizeof(mask);

  total_length = header_length + length;
  frame = malloc(total_length == 0 ? 1 : total_length);
  if (frame == NULL)
    {
      return -ENOMEM;
    }

  memcpy(frame, header, header_length);
  for (i = 0; i < length; i++)
    {
      frame[header_length + i] = ((const uint8_t *)data)[i] ^
                                  mask[i & 3];
    }

  pthread_mutex_lock(&g_xz_io_lock);
  ret = xz_tls_write_all(&client->tls, frame, total_length);
  pthread_mutex_unlock(&g_xz_io_lock);
  free(frame);
  return ret;
}

int xiaozhi_transport_send_text(struct xiaozhi_client *client,
                                const char *text)
{
  if (text == NULL)
    {
      return -EINVAL;
    }
  return xz_ws_send_frame(client, 0x1, text, strlen(text));
}

int xiaozhi_transport_send_binary(struct xiaozhi_client *client,
                                  const void *data, size_t length)
{
  return xz_ws_send_frame(client, 0x2, data, length);
}

bool xiaozhi_transport_should_stop(const struct xiaozhi_client *client)
{
  return client == NULL || client->stop_requested;
}

static int xz_ws_read_frame(struct xiaozhi_client *client, uint8_t *opcode,
                            uint8_t **payload, size_t *payload_length)
{
  uint8_t first[2];
  uint8_t mask[4];
  uint64_t length64;
  size_t length;
  size_t i;
  bool masked;
  int ret;

  *payload = NULL;
  *payload_length = 0;
  ret = xz_tls_read_exact(client, first, sizeof(first));
  if (ret != 0)
    {
      return ret;
    }

  if ((first[0] & 0x80) == 0)
    {
      syslog(LOG_WARNING, "[%s] fragmented websocket frames are unsupported\n",
             XZ_TAG);
      return -ENOTSUP;
    }

  *opcode = first[0] & 0x0f;
  masked = (first[1] & 0x80) != 0;
  length64 = first[1] & 0x7f;
  if (length64 == 126)
    {
      uint8_t ext[2];
      ret = xz_tls_read_exact(client, ext, sizeof(ext));
      if (ret != 0)
        {
          return ret;
        }
      length64 = ((uint64_t)ext[0] << 8) | ext[1];
    }
  else if (length64 == 127)
    {
      uint8_t ext[8];
      ret = xz_tls_read_exact(client, ext, sizeof(ext));
      if (ret != 0)
        {
          return ret;
        }
      length64 = 0;
      for (i = 0; i < sizeof(ext); i++)
        {
          length64 = (length64 << 8) | ext[i];
        }
    }

  if (length64 > XIAOZHI_WS_MAX_FRAME)
    {
      syslog(LOG_ERR, "[%s] websocket frame too large: %lu\n", XZ_TAG,
             (unsigned long)length64);
      return -E2BIG;
    }
  length = (size_t)length64;

  if (masked)
    {
      ret = xz_tls_read_exact(client, mask, sizeof(mask));
      if (ret != 0)
        {
          return ret;
        }
    }

  *payload = malloc(length + 1);
  if (*payload == NULL)
    {
      return -ENOMEM;
    }

  if (length != 0)
    {
      ret = xz_tls_read_exact(client, *payload, length);
      if (ret != 0)
        {
          free(*payload);
          *payload = NULL;
          return ret;
        }
    }
  if (masked)
    {
      for (i = 0; i < length; i++)
        {
          (*payload)[i] ^= mask[i & 3];
        }
    }
  (*payload)[length] = '\0';
  *payload_length = length;
  return 0;
}

static int xz_ws_handshake(struct xiaozhi_client *client)
{
  uint8_t raw_key[16];
  uint8_t key_b64[32];
  size_t key_length = 0;
  char auth[ XIAOZHI_TOKEN_MAX + 16];
  char request[XZ_HTTP_REQUEST_MAX];
  char response[XZ_WS_HEADER_MAX];
  size_t response_length = 0;
  int request_length;
  int ret;

  ret = mbedtls_ctr_drbg_random(&client->tls.drbg, raw_key,
                                sizeof(raw_key));
  if (ret != 0)
    {
      return -EIO;
    }
  ret = mbedtls_base64_encode(key_b64, sizeof(key_b64) - 1, &key_length,
                              raw_key, sizeof(raw_key));
  if (ret != 0)
    {
      return -EIO;
    }
  key_b64[key_length] = '\0';

  if (client->token[0] == '\0')
    {
      xz_copy_string(auth, sizeof(auth), XIAOZHI_DEFAULT_TOKEN);
    }
  else if (strncmp(client->token, "Bearer ", 7) == 0)
    {
      xz_copy_string(auth, sizeof(auth), client->token);
    }
  else
    {
      snprintf(auth, sizeof(auth), "Bearer %s", client->token);
    }

  request_length = snprintf(request, sizeof(request),
                            "GET %s HTTP/1.1\r\n"
                            "Host: %s\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Key: %s\r\n"
                            "Sec-WebSocket-Version: 13\r\n"
                            "Authorization: %s\r\n"
                            "Protocol-Version: 1\r\n"
                            "Device-Id: %s\r\n"
                            "Client-Id: %s\r\n"
                            "\r\n",
                            client->path, client->host, key_b64, auth,
                            client->device_id, client->client_id);
  if (request_length <= 0 || (size_t)request_length >= sizeof(request))
    {
      return -E2BIG;
    }

  ret = xz_tls_write_all(&client->tls, request, (size_t)request_length);
  if (ret < 0)
    {
      return ret;
    }

  memset(response, 0, sizeof(response));
  while (response_length < sizeof(response) - 1)
    {
      uint8_t ch;

      ret = xz_tls_read_some(&client->tls, &ch, 1);
      if (ret == XZ_READ_TIMEOUT)
        {
          continue;
        }
      if (ret < 0)
        {
          return ret;
        }
      response[response_length++] = (char)ch;
      if (response_length >= 4 &&
          memcmp(response + response_length - 4, "\r\n\r\n", 4) == 0)
        {
          break;
        }
    }
  response[response_length] = '\0';
  if (strstr(response, " 101 ") == NULL)
    {
      syslog(LOG_ERR, "[%s] websocket upgrade rejected: %.180s\n", XZ_TAG,
             response);
      return -EPROTO;
    }

  client->connected = true;
  syslog(LOG_INFO, "[%s] websocket connected wss://%s%s\n", XZ_TAG,
         client->host, client->path);
  return 0;
}

static int xz_ws_connect(struct xiaozhi_client *client)
{
  int ret;

  xz_set_state(client, XIAOZHI_STATE_CONNECTING);
  ret = xz_tls_open(&client->tls, client->host, client->port);
  if (ret < 0)
    {
      return ret;
    }

  ret = xz_ws_handshake(client);
  if (ret < 0)
    {
      xz_tls_close(&client->tls);
      client->connected = false;
      return ret;
    }
  return 0;
}

static void xz_ws_disconnect(struct xiaozhi_client *client)
{
  client->connected = false;
  client->session_ready = false;
  client->listening = false;
  xz_tls_close(&client->tls);
}

static int xz_send_hello(struct xiaozhi_client *client)
{
  char hello[512];

  snprintf(hello, sizeof(hello),
           "{\"type\":\"hello\",\"version\":3,"
           "\"features\":{\"mcp\":false},"
           "\"transport\":\"websocket\","
           "\"audio_params\":{\"format\":\"opus\","
           "\"sample_rate\":%u,\"channels\":1,"
           "\"frame_duration\":%u}}",
           client->sample_rate, client->frame_duration_ms);
  return xiaozhi_transport_send_text(client, hello);
}

static int xz_send_iot_descriptors(struct xiaozhi_client *client)
{
  char message[1024];

  if (!client->session_ready)
    {
      return -ENOTCONN;
    }

  snprintf(message, sizeof(message),
           "{\"session_id\":\"%s\",\"type\":\"iot\","
           "\"update\":true,\"descriptors\":[{"
           "\"name\":\"velawear\","
           "\"description\":\"SF32LB52 wearable health agent\","
           "\"properties\":[{\"name\":\"state\","
           "\"description\":\"current VelaWear state\","
           "\"type\":\"string\"}],"
           "\"methods\":[]}]}\n",
           client->session_id);
  return xiaozhi_transport_send_text(client, message);
}

static int xz_send_iot_state(struct xiaozhi_client *client)
{
  char message[512];

  if (!client->session_ready)
    {
      return -ENOTCONN;
    }

  snprintf(message, sizeof(message),
           "{\"session_id\":\"%s\",\"type\":\"iot\","
           "\"update\":true,\"states\":[{\"name\":\"velawear\","
           "\"state\":{\"state\":\"online\"}}]}\n",
           client->session_id);
  return xiaozhi_transport_send_text(client, message);
}

static int xz_json_number(cJSON *object, const char *name, int fallback)
{
  cJSON *item = cJSON_GetObjectItem(object, name);

  if (item == NULL)
    {
      return fallback;
    }
  if (cJSON_IsNumber(item))
    {
      return item->valueint;
    }
  if (cJSON_IsString(item) && item->valuestring != NULL)
    {
      return atoi(item->valuestring);
    }
  return fallback;
}

static void xz_save_text(struct xiaozhi_client *client, cJSON *root,
                         const char *name)
{
  cJSON *item = cJSON_GetObjectItem(root, name);
  const char *text = cJSON_GetStringValue(item);

  if (text != NULL)
    {
      xz_copy_string(client->last_text, sizeof(client->last_text), text);
      syslog(LOG_INFO, "[%s] %s: %s\n", XZ_TAG, name, client->last_text);
    }
}

static void xz_handle_json(struct xiaozhi_client *client, const char *data,
                           size_t length)
{
  cJSON *root;
  cJSON *type_item;
  const char *type;

  (void)length;
  root = cJSON_Parse(data);
  if (root == NULL)
    {
      syslog(LOG_WARNING, "[%s] invalid JSON from server\n", XZ_TAG);
      return;
    }

  type_item = cJSON_GetObjectItem(root, "type");
  type = cJSON_GetStringValue(type_item);
  if (type == NULL)
    {
      cJSON_Delete(root);
      return;
    }

  if (strcmp(type, "hello") == 0)
    {
      cJSON *session = cJSON_GetObjectItem(root, "session_id");
      cJSON *audio = cJSON_GetObjectItem(root, "audio_params");
      const char *session_id = cJSON_GetStringValue(session);
      int sample_rate = xz_json_number(audio, "sample_rate", 16000);
      int frame_duration = xz_json_number(audio, "frame_duration",
                                           xz_json_number(audio, "duration", 60));

      if (session_id != NULL)
        {
          xz_copy_string(client->session_id, sizeof(client->session_id),
                         session_id);
        }
      if (sample_rate > 0)
        {
          client->sample_rate = (unsigned int)sample_rate;
        }
      if (frame_duration > 0)
        {
          client->frame_duration_ms = (unsigned int)frame_duration;
        }
      client->session_ready = true;
      client->connected = true;
      xz_set_state(client, XIAOZHI_STATE_IDLE);
      syslog(LOG_INFO, "[%s] hello session=%s audio=%uHz/%ums\n", XZ_TAG,
             client->session_id, client->sample_rate,
             client->frame_duration_ms);
#ifdef CONFIG_VELAWEAR_XIAOZHI_AUDIO
      if (client->audio_enabled)
        {
          (void)xiaozhi_audio_configure(client, client->sample_rate,
                                        client->frame_duration_ms);
        }
#endif
      (void)xz_send_iot_descriptors(client);
      (void)xz_send_iot_state(client);
    }
  else if (strcmp(type, "stt") == 0)
    {
      xz_save_text(client, root, "text");
    }
  else if (strcmp(type, "tts") == 0)
    {
      cJSON *state_item = cJSON_GetObjectItem(root, "state");
      const char *state = cJSON_GetStringValue(state_item);

      xz_save_text(client, root, "text");
      if (state != NULL && strcmp(state, "start") == 0)
        {
          xz_set_state(client, XIAOZHI_STATE_SPEAKING);
#ifdef CONFIG_VELAWEAR_XIAOZHI_AUDIO
          if (client->audio_enabled)
            {
              (void)xiaozhi_audio_tts_start(client);
            }
#endif
        }
      else if (state != NULL && strcmp(state, "stop") == 0)
        {
#ifdef CONFIG_VELAWEAR_XIAOZHI_AUDIO
          if (client->audio_enabled)
            {
              (void)xiaozhi_audio_tts_stop(client);
            }
#endif
          xz_set_state(client, XIAOZHI_STATE_IDLE);
        }
    }
  else if (strcmp(type, "llm") == 0)
    {
      cJSON *emotion = cJSON_GetObjectItem(root, "emotion");
      const char *value = cJSON_GetStringValue(emotion);

      if (value != NULL)
        {
          xz_copy_string(client->last_emotion, sizeof(client->last_emotion),
                         value);
          syslog(LOG_INFO, "[%s] emotion=%s\n", XZ_TAG,
                 client->last_emotion);
        }
    }
  else if (strcmp(type, "goodbye") == 0)
    {
      syslog(LOG_INFO, "[%s] server closed session\n", XZ_TAG);
#ifdef CONFIG_VELAWEAR_XIAOZHI_AUDIO
      if (client->audio_enabled)
        {
          (void)xiaozhi_audio_tts_stop(client);
          (void)xiaozhi_audio_listen_stop(client);
        }
#endif
      client->session_ready = false;
      client->listening = false;
      xz_set_state(client, XIAOZHI_STATE_CONNECTING);
    }
  else if (strcmp(type, "iot") == 0 || strcmp(type, "mcp") == 0)
    {
      /* The VelaWear MVP exposes descriptors/states first.  Keep commands
       * visible in logs until a board action mapping is added. */
      syslog(LOG_INFO, "[%s] received %s command\n", XZ_TAG, type);
    }
  else if (strcmp(type, "error") == 0)
    {
      xz_save_text(client, root, "message");
      client->last_error = -EPROTO;
      xz_set_state(client, XIAOZHI_STATE_ERROR);
    }

  cJSON_Delete(root);
}

static int xz_send_listen(struct xiaozhi_client *client, const char *state,
                          const char *mode)
{
  char message[256];

  if (!client->session_ready)
    {
      return -ENOTCONN;
    }

  snprintf(message, sizeof(message),
           "{\"session_id\":\"%s\",\"type\":\"listen\","
           "\"state\":\"%s\",\"mode\":\"%s\"}",
           client->session_id, state, mode == NULL ? "manual" : mode);
  return xiaozhi_transport_send_text(client, message);
}

static int xz_send_abort(struct xiaozhi_client *client)
{
  char message[192];

  if (!client->session_ready)
    {
      return -ENOTCONN;
    }

  snprintf(message, sizeof(message),
           "{\"session_id\":\"%s\",\"type\":\"abort\"}",
           client->session_id);
  return xiaozhi_transport_send_text(client, message);
}

static int xz_network_loop(struct xiaozhi_client *client)
{
  while (!client->stop_requested && client->connected)
    {
      uint8_t opcode = 0;
      uint8_t *payload = NULL;
      size_t payload_length = 0;
      int ret;

      ret = xz_ws_read_frame(client, &opcode, &payload, &payload_length);
      if (ret == XZ_READ_TIMEOUT)
        {
          continue;
        }
      if (ret < 0)
        {
          free(payload);
          return ret;
        }

      if (opcode == 0x1)
        {
          xz_handle_json(client, (const char *)payload, payload_length);
        }
      else if (opcode == 0x2)
        {
#ifdef CONFIG_VELAWEAR_XIAOZHI_AUDIO
          if (client->audio_enabled)
            {
              (void)xiaozhi_audio_downlink(client, payload, payload_length);
            }
#else
          syslog(LOG_WARNING, "[%s] binary audio frame ignored; audio is "
                 "disabled\n", XZ_TAG);
#endif
        }
      else if (opcode == 0x8)
        {
          free(payload);
          return -ECONNRESET;
        }
      else if (opcode == 0x9)
        {
          (void)xz_ws_send_frame(client, 0xA, payload, payload_length);
        }
      else if (opcode != 0xA)
        {
          syslog(LOG_WARNING, "[%s] unsupported websocket opcode=%u\n",
                 XZ_TAG, opcode);
        }

      free(payload);
    }
  return client->stop_requested ? -ECANCELED : -ECONNRESET;
}

static int xz_http_post_ota(struct xiaozhi_client *client, char *body,
                            size_t body_size)
{
  xiaozhi_tls_t *tls = &client->tls;
  char *payload = NULL;
  char *request = NULL;
  char *response = NULL;
  const char *body_start;
  size_t response_length = 0;
  size_t response_body_length;
  int request_length;
  int status = 0;
  int ret = -ENOMEM;
  bool tls_open = false;

  payload = malloc(2048);
  request = malloc(XZ_HTTP_REQUEST_MAX);
  response = malloc(XZ_OTA_RESPONSE_MAX);
  if (payload == NULL || request == NULL || response == NULL)
    {
      goto cleanup;
    }

  snprintf(payload, 2048,
           "{\"version\":2,\"flash_size\":4194304,"
           "\"psram_size\":0,\"minimum_free_heap_size\":0,"
           "\"mac_address\":\"%s\",\"uuid\":\"%s\","
           "\"chip_model_name\":\"sf32lb52\","
           "\"application\":{\"name\":\"velawear\","
           "\"version\":\"1.0.0\",\"compile_time\":\"unknown\"},"
           "\"board\":{\"type\":\"sf32lb52-lchspi-ulp\","
           "\"mac\":\"%s\"}}",
           client->device_id, client->client_id, client->device_id);

  request_length = snprintf(request, XZ_HTTP_REQUEST_MAX,
                            "POST %s HTTP/1.1\r\n"
                            "Host: %s\r\n"
                            "User-Agent: velawear-xiaozhi/1.0\r\n"
                            "Device-Id: %s\r\n"
                            "Client-Id: %s\r\n"
                            "Content-Type: application/json\r\n"
                            "Content-Length: %u\r\n"
                            "Connection: close\r\n\r\n%s",
                            XIAOZHI_OTA_PATH, XIAOZHI_DEFAULT_HOST,
                            client->device_id, client->client_id,
                            (unsigned int)strlen(payload), payload);
  if (request_length <= 0 || (size_t)request_length >= XZ_HTTP_REQUEST_MAX)
    {
      ret = -E2BIG;
      goto cleanup;
    }

  ret = xz_tls_open(tls, XIAOZHI_DEFAULT_HOST, XZ_DEFAULT_PORT);
  if (ret < 0)
    {
      goto cleanup;
    }
  tls_open = true;

  ret = xz_tls_write_all(tls, request, (size_t)request_length);
  if (ret < 0)
    {
      goto cleanup;
    }

  while (response_length < XZ_OTA_RESPONSE_MAX - 1)
    {
      ret = xz_tls_read_some(tls, response + response_length,
                             XZ_OTA_RESPONSE_MAX - 1 - response_length);
      if (ret == XZ_READ_TIMEOUT)
        {
          /* Connection-close HTTP responses normally finish with ret=0;
           * accept a timeout after a complete body as a useful fallback. */
          break;
        }
      if (ret < 0)
        {
          break;
        }
      response_length += (size_t)ret;
    }
  response[response_length] = '\0';
  xz_tls_close(tls);
  tls_open = false;

  if (sscanf(response, "HTTP/%*s %d", &status) != 1 || status != 200)
    {
      syslog(LOG_ERR, "[%s] OTA HTTP status=%d response=%.160s\n", XZ_TAG,
             status, response);
      ret = -EPROTO;
      goto cleanup;
    }

  body_start = strstr(response, "\r\n\r\n");
  if (body_start == NULL)
    {
      ret = -EPROTO;
      goto cleanup;
    }
  body_start += 4;
  response_body_length = response_length -
                         (size_t)(body_start - response);
  if (response_body_length >= body_size)
    {
      ret = -E2BIG;
      goto cleanup;
    }
  memcpy(body, body_start, response_body_length);
  body[response_body_length] = '\0';
  ret = (int)response_body_length;

cleanup:
  if (tls_open)
    {
      xz_tls_close(tls);
    }
  free(response);
  free(request);
  free(payload);
  return ret;
}

/* Returns 0 for an endpoint, 1 while activation is pending, or <0 on error. */
static int xz_ensure_ota(struct xiaozhi_client *client)
{
  char *response;
  cJSON *root = NULL;
  cJSON *websocket;
  cJSON *activation;
  cJSON *item;
  const char *value;
  int ret;
  bool have_endpoint = false;

  response = malloc(XZ_OTA_RESPONSE_MAX);
  if (response == NULL)
    {
      return -ENOMEM;
    }

  ret = xz_http_post_ota(client, response, XZ_OTA_RESPONSE_MAX);
  if (ret < 0)
    {
      goto cleanup;
    }

  root = cJSON_Parse(response);
  if (root == NULL)
    {
      syslog(LOG_ERR, "[%s] OTA returned invalid JSON\n", XZ_TAG);
      ret = -EPROTO;
      goto cleanup;
    }

  websocket = cJSON_GetObjectItem(root, "websocket");
  if (websocket != NULL && cJSON_IsObject(websocket))
    {
      item = cJSON_GetObjectItem(websocket, "url");
      value = cJSON_GetStringValue(item);
      if (value != NULL && xz_apply_websocket_url(client, value) == 0)
        {
          have_endpoint = true;
        }

      item = cJSON_GetObjectItem(websocket, "token");
      value = cJSON_GetStringValue(item);
      if (value != NULL)
        {
          xz_copy_string(client->token, sizeof(client->token), value);
          have_endpoint = true;
        }
    }

  activation = cJSON_GetObjectItem(root, "activation");
  if (activation != NULL && cJSON_IsObject(activation))
    {
      item = cJSON_GetObjectItem(activation, "code");
      value = cJSON_GetStringValue(item);
      if (value != NULL && value[0] != '\0')
        {
          syslog(LOG_WARNING, "[%s] activation required: code=%s; add this "
                 "device at xiaozhi.me\n", XZ_TAG, value);
        }
    }

  cJSON_Delete(root);
  root = NULL;
  if (have_endpoint)
    {
      client->endpoint_ready = true;
      ret = 0;
      goto cleanup;
    }
  ret = 1;

cleanup:
  if (root != NULL)
    {
      cJSON_Delete(root);
    }
  free(response);
  return ret;
}

static void *xz_worker(void *argument)
{
  struct xiaozhi_client *client = argument;
  int ret;
#ifdef CONFIG_VELAWEAR_XIAOZHI_PAN
  bool pan_wait_logged = false;
#endif

  xz_set_state(client, XIAOZHI_STATE_CONNECTING);
  (void)xz_prepare_identity(client);
  xz_set_default_endpoint(client);
  if (client->ota_enabled && !client->no_ota)
    {
      client->endpoint_ready = false;
    }

#ifdef CONFIG_VELAWEAR_XIAOZHI_AUDIO
  if (client->audio_enabled)
    {
      ret = xiaozhi_audio_init(client);
      if (ret < 0)
        {
          syslog(LOG_WARNING, "[%s] audio init failed: %d\n", XZ_TAG, ret);
          client->audio_enabled = false;
        }
    }
#endif

  while (!client->stop_requested)
    {
#ifdef CONFIG_VELAWEAR_XIAOZHI_PAN
      /* The endpoint is reached through the phone's Bluetooth NAP. */
      if (!velawear_pan_is_ready())
        {
          if (!pan_wait_logged)
            {
              if (velawear_pan_is_configured())
                {
                  syslog(LOG_INFO, "[%s] waiting for Bluetooth PAN/DHCP\n",
                         XZ_TAG);
                }
              else
                {
                  syslog(LOG_INFO,
                         "[%s] waiting for PAN peer configuration\n", XZ_TAG);
                }
              pan_wait_logged = true;
            }
          xz_sleep_interruptible(client, XZ_RECONNECT_DELAY_SEC);
          continue;
        }
      pan_wait_logged = false;
#endif

      if (client->ota_enabled && !client->no_ota && !client->endpoint_ready)
        {
          ret = xz_ensure_ota(client);
          if (ret == 1)
            {
              xz_sleep_interruptible(client, XZ_ACTIVATION_POLL_SEC);
              continue;
            }
          if (ret < 0)
            {
              xz_sleep_interruptible(client, XZ_RECONNECT_DELAY_SEC);
              continue;
            }
        }

      ret = xz_ws_connect(client);
      if (ret < 0)
        {
          client->last_error = ret;
          xz_set_state(client, XIAOZHI_STATE_ERROR);
          xz_sleep_interruptible(client, XZ_RECONNECT_DELAY_SEC);
          continue;
        }

      ret = xz_send_hello(client);
      if (ret < 0)
        {
          syslog(LOG_ERR, "[%s] hello send failed: %d\n", XZ_TAG, ret);
          xz_ws_disconnect(client);
          xz_sleep_interruptible(client, XZ_RECONNECT_DELAY_SEC);
          continue;
        }

      (void)xz_network_loop(client);
#ifdef CONFIG_VELAWEAR_XIAOZHI_AUDIO
      if (client->audio_enabled)
        {
          (void)xiaozhi_audio_listen_stop(client);
          (void)xiaozhi_audio_tts_stop(client);
        }
#endif
      xz_ws_disconnect(client);
      if (!client->stop_requested)
        {
          xz_sleep_interruptible(client, XZ_RECONNECT_DELAY_SEC);
        }
    }

#ifdef CONFIG_VELAWEAR_XIAOZHI_AUDIO
  if (client->audio_enabled)
    {
      xiaozhi_audio_deinit(client);
    }
#endif
  xz_ws_disconnect(client);
  xz_set_state(client, XIAOZHI_STATE_STOPPED);
  return NULL;
}

static int xz_start_internal(bool no_ota)
{
  int ret;

  if (g_xz.thread_started)
    {
      return -EALREADY;
    }

  memset(&g_xz, 0, sizeof(g_xz));
  g_xz.stop_requested = false;
  g_xz.ota_enabled = true;
  g_xz.no_ota = no_ota;
  g_xz.auto_reconnect = true;
#ifdef CONFIG_VELAWEAR_XIAOZHI_AUDIO
  g_xz.audio_enabled = true;
#else
  g_xz.audio_enabled = false;
#endif
  g_xz.sample_rate = 16000;
  g_xz.frame_duration_ms = 60;

#ifdef CONFIG_VELAWEAR_XIAOZHI_OTA
  g_xz.ota_enabled = true;
#else
  g_xz.ota_enabled = false;
#endif

  ret = pthread_create(&g_xz.thread, NULL, xz_worker, &g_xz);
  if (ret != 0)
    {
      g_xz.state = XIAOZHI_STATE_STOPPED;
      return -ret;
    }
  g_xz.thread_started = true;
  syslog(LOG_INFO, "[%s] worker started (ota=%d audio=%d)\n", XZ_TAG,
         g_xz.ota_enabled && !g_xz.no_ota, g_xz.audio_enabled);
  return 0;
}

int velawear_xiaozhi_start(void)
{
  return xz_start_internal(false);
}

int velawear_xiaozhi_stop(void)
{
  if (!g_xz.thread_started)
    {
      return 0;
    }

  g_xz.stop_requested = true;
  (void)pthread_join(g_xz.thread, NULL);
  g_xz.thread_started = false;
  return 0;
}

bool velawear_xiaozhi_is_connected(void)
{
  return g_xz.thread_started && g_xz.connected && g_xz.session_ready;
}

bool velawear_xiaozhi_is_listening(void)
{
  return g_xz.thread_started && g_xz.listening;
}

int velawear_xiaozhi_listen_start(void)
{
  int ret;

  if (!velawear_xiaozhi_is_connected())
    {
      return -ENOTCONN;
    }

  ret = xz_send_listen(&g_xz, "start", "manual");
  if (ret < 0)
    {
      return ret;
    }
#ifdef CONFIG_VELAWEAR_XIAOZHI_AUDIO
  if (g_xz.audio_enabled)
    {
      ret = xiaozhi_audio_listen_start(&g_xz);
      if (ret < 0)
        {
          (void)xz_send_listen(&g_xz, "stop", "manual");
          return ret;
        }
    }
#endif
  g_xz.listening = true;
  xz_set_state(&g_xz, XIAOZHI_STATE_LISTENING);
  return 0;
}

int velawear_xiaozhi_listen_stop(void)
{
  int ret = 0;

  if (!g_xz.thread_started)
    {
      return -ENODEV;
    }
#ifdef CONFIG_VELAWEAR_XIAOZHI_AUDIO
  if (g_xz.audio_enabled)
    {
      ret = xiaozhi_audio_listen_stop(&g_xz);
    }
#endif
  if (g_xz.session_ready)
    {
      int send_ret = xz_send_listen(&g_xz, "stop", "manual");
      if (ret == 0)
        {
          ret = send_ret;
        }
    }
  g_xz.listening = false;
  if (g_xz.session_ready)
    {
      xz_set_state(&g_xz, XIAOZHI_STATE_IDLE);
    }
  return ret;
}

int velawear_xiaozhi_abort(void)
{
  int ret;

  ret = xz_send_abort(&g_xz);
#ifdef CONFIG_VELAWEAR_XIAOZHI_AUDIO
  if (g_xz.audio_enabled)
    {
      (void)xiaozhi_audio_listen_stop(&g_xz);
      (void)xiaozhi_audio_tts_stop(&g_xz);
    }
#endif
  g_xz.listening = false;
  return ret;
}

int velawear_xiaozhi_button_down(void)
{
  return velawear_xiaozhi_listen_start();
}

int velawear_xiaozhi_button_up(void)
{
  return velawear_xiaozhi_listen_stop();
}

static void xz_cli_signal_handler(int signo)
{
  (void)signo;
  g_xz.stop_requested = true;
}

int velawear_xiaozhi_run_cli(int argc, char **argv)
{
  bool no_ota = false;
  bool start_listening = false;
  int i;
  int ret;

  for (i = 0; i < argc; i++)
    {
      if (strcmp(argv[i], "--no-ota") == 0)
        {
          no_ota = true;
        }
      else if (strcmp(argv[i], "--listen") == 0)
        {
          start_listening = true;
        }
      else
        {
          printf("usage: velawear xiaozhi [--no-ota] [--listen]\n");
          return -EINVAL;
        }
    }

  signal(SIGINT, xz_cli_signal_handler);
  signal(SIGTERM, xz_cli_signal_handler);
  ret = xz_start_internal(no_ota);
  if (ret < 0)
    {
      return ret;
    }

  while (!g_xz.stop_requested)
    {
      if (start_listening && velawear_xiaozhi_is_connected() &&
          !velawear_xiaozhi_is_listening())
        {
          ret = velawear_xiaozhi_listen_start();
          if (ret < 0)
            {
              syslog(LOG_WARNING, "[%s] CLI listen start failed: %d\n",
                     XZ_TAG, ret);
            }
          start_listening = false;
        }
      sleep(1);
    }

  return velawear_xiaozhi_stop();
}
