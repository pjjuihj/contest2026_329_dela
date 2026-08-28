/* Internal shared definitions for the VelaWear XiaoZhi port. */

#ifndef VELAWEAR_XIAOZHI_INTERNAL_H
#define VELAWEAR_XIAOZHI_INTERNAL_H

#include "xiaozhi.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#define XIAOZHI_DEFAULT_HOST "api.tenclass.net"
#define XIAOZHI_DEFAULT_PATH "/xiaozhi/v1/"
#define XIAOZHI_DEFAULT_TOKEN "Bearer 12345678"
#define XIAOZHI_OTA_PATH "/xiaozhi/ota/"

#define XIAOZHI_DEVICE_ID_MAX 32
#define XIAOZHI_CLIENT_ID_MAX 64
#define XIAOZHI_HOST_MAX 96
#define XIAOZHI_PATH_MAX 192
#define XIAOZHI_TOKEN_MAX 192
#define XIAOZHI_SESSION_MAX 80
#define XIAOZHI_TEXT_MAX 512
#define XIAOZHI_WS_MAX_FRAME 8192

typedef struct xiaozhi_tls
{
  mbedtls_net_context net;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  bool initialized;
  bool ssl_ready;
} xiaozhi_tls_t;

typedef enum xiaozhi_client_state
{
  XIAOZHI_STATE_STOPPED = 0,
  XIAOZHI_STATE_CONNECTING,
  XIAOZHI_STATE_IDLE,
  XIAOZHI_STATE_LISTENING,
  XIAOZHI_STATE_SPEAKING,
  XIAOZHI_STATE_ERROR
} xiaozhi_client_state_t;

struct xiaozhi_client
{
  pthread_t thread;
  bool thread_started;
  volatile bool stop_requested;
  volatile bool connected;
  volatile bool session_ready;
  volatile bool listening;
  bool ota_enabled;
  bool no_ota;
  bool auto_reconnect;
  bool audio_enabled;
  bool endpoint_ready;
  xiaozhi_tls_t tls;
  xiaozhi_client_state_t state;
  int last_error;

  char device_id[XIAOZHI_DEVICE_ID_MAX];
  char client_id[XIAOZHI_CLIENT_ID_MAX];
  char host[XIAOZHI_HOST_MAX];
  char path[XIAOZHI_PATH_MAX];
  char token[XIAOZHI_TOKEN_MAX];
  int port;
  char session_id[XIAOZHI_SESSION_MAX];
  char last_text[XIAOZHI_TEXT_MAX];
  char last_emotion[32];
  unsigned int sample_rate;
  unsigned int frame_duration_ms;

  /* Opaque Opus/audio state owned by xiaozhi_audio.c. */
  void *audio_encoder;
  void *audio_decoder;
  pthread_t audio_thread;
  bool audio_thread_started;
  bool audio_capture_started;
  bool audio_playback_started;
};

/* Transport functions shared with xiaozhi_audio.c. */
int xiaozhi_transport_send_text(struct xiaozhi_client *client,
                                const char *text);
int xiaozhi_transport_send_binary(struct xiaozhi_client *client,
                                  const void *data, size_t length);
bool xiaozhi_transport_should_stop(const struct xiaozhi_client *client);

#endif /* VELAWEAR_XIAOZHI_INTERNAL_H */
