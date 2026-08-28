/* VelaWear Agent - compact BLE Agent protocol */
#ifndef __VELAWEAR_AGENT_PROTOCOL_H
#define __VELAWEAR_AGENT_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VELAWEAR_AGENT_PROTOCOL_VERSION       1
#define VELAWEAR_AGENT_EVENT_FRAME_SIZE       11
#define VELAWEAR_AGENT_COMMAND_FRAME_SIZE     6
#define VELAWEAR_AGENT_RESULT_FRAME_SIZE      5
#define VELAWEAR_AGENT_MESSAGE_TYPE            3
#define VELAWEAR_AGENT_MESSAGE_TEXT_MAX      220
#define VELAWEAR_AGENT_MESSAGE_HEADER_SIZE     7
#define VELAWEAR_AGENT_MESSAGE_FRAME_MAX \
  (VELAWEAR_AGENT_MESSAGE_HEADER_SIZE + VELAWEAR_AGENT_MESSAGE_TEXT_MAX)
#define VELAWEAR_AGENT_LLM_REQUEST_TYPE         1
#define VELAWEAR_AGENT_LLM_RESPONSE_TYPE        2
#define VELAWEAR_AGENT_LLM_TEXT_MAX              220
#define VELAWEAR_AGENT_LLM_REQUEST_HEADER_SIZE   6
#define VELAWEAR_AGENT_LLM_RESPONSE_HEADER_SIZE 7
#define VELAWEAR_AGENT_LLM_REQUEST_FRAME_MAX \
  (VELAWEAR_AGENT_LLM_REQUEST_HEADER_SIZE + VELAWEAR_AGENT_LLM_TEXT_MAX)
#define VELAWEAR_AGENT_LLM_RESPONSE_FRAME_MAX \
  (VELAWEAR_AGENT_LLM_RESPONSE_HEADER_SIZE + VELAWEAR_AGENT_LLM_TEXT_MAX)

/* Host-to-device voice stream.  PCM is deliberately uncompressed in v1 so
 * the SF32 playback path can use the existing AUDCODEC DMA buffer directly. */
#define VELAWEAR_AGENT_AUDIO_START_TYPE          4
#define VELAWEAR_AGENT_AUDIO_DATA_TYPE           5
#define VELAWEAR_AGENT_AUDIO_END_TYPE            6
#define VELAWEAR_AGENT_AUDIO_START_FRAME_SIZE   20
#define VELAWEAR_AGENT_AUDIO_DATA_HEADER_SIZE    8
#define VELAWEAR_AGENT_AUDIO_END_FRAME_SIZE     12
#define VELAWEAR_AGENT_AUDIO_SAMPLE_RATE    16000
#define VELAWEAR_AGENT_AUDIO_CHANNELS             1
#define VELAWEAR_AGENT_AUDIO_BITS_PER_SAMPLE    16
#define VELAWEAR_AGENT_AUDIO_MAX_PCM_BYTES   64000

#define VELAWEAR_AGENT_LLM_STATUS_OK             0
#define VELAWEAR_AGENT_LLM_STATUS_ERROR          1

#define VELAWEAR_AGENT_EVENT_FLAG_ALERT       (1u << 0)
#define VELAWEAR_AGENT_EVENT_FLAG_ACTIVE      (1u << 1)

#define VELAWEAR_AGENT_PRIORITY_NORMAL        1
#define VELAWEAR_AGENT_PRIORITY_CRITICAL      3

typedef enum velawear_agent_event_type
{
  VELAWEAR_AGENT_EVENT_MOTION = 1,
  VELAWEAR_AGENT_EVENT_FALL = 3,
  VELAWEAR_AGENT_EVENT_AUDIO = 4,
  VELAWEAR_AGENT_EVENT_SEDENTARY = 9
} velawear_agent_event_type_t;

typedef enum velawear_agent_command_id
{
  VELAWEAR_AGENT_COMMAND_ACK_ALERT = 1,
  VELAWEAR_AGENT_COMMAND_SET_SEDENTARY_THRESHOLD = 2,
  VELAWEAR_AGENT_COMMAND_SHOW_REMINDER = 3
} velawear_agent_command_id_t;

typedef enum velawear_agent_reminder_id
{
  VELAWEAR_AGENT_REMINDER_STRETCH = 0,
  VELAWEAR_AGENT_REMINDER_HYDRATE = 1,
  VELAWEAR_AGENT_REMINDER_REST = 2
} velawear_agent_reminder_id_t;

typedef enum velawear_agent_command_result
{
  VELAWEAR_AGENT_COMMAND_RESULT_OK = 0,
  VELAWEAR_AGENT_COMMAND_RESULT_REJECTED = 1,
  VELAWEAR_AGENT_COMMAND_RESULT_EXECUTION_FAILED = 2
} velawear_agent_command_result_t;

typedef struct velawear_agent_event
{
  uint16_t sequence;
  uint8_t event_type;
  uint8_t priority;
  uint8_t intensity_percent;
  uint16_t value;
  uint16_t peak;
  uint8_t flags;
} velawear_agent_event_t;

typedef struct velawear_agent_command
{
  uint16_t sequence;
  uint8_t command_id;
  uint16_t argument;
} velawear_agent_command_t;

typedef struct velawear_agent_llm_response
{
  uint16_t sequence;
  uint8_t status;
  uint16_t length;
  char text[VELAWEAR_AGENT_LLM_TEXT_MAX + 1];
} velawear_agent_llm_response_t;

typedef struct velawear_agent_message
{
  uint16_t sequence;
  uint8_t priority;
  uint16_t length;
  char text[VELAWEAR_AGENT_MESSAGE_TEXT_MAX + 1];
} velawear_agent_message_t;

typedef struct velawear_agent_audio_start
{
  uint16_t stream_id;
  uint16_t sample_rate;
  uint8_t channels;
  uint8_t bits_per_sample;
  uint32_t sample_count;
  uint32_t total_bytes;
  uint32_t crc32;
} velawear_agent_audio_start_t;

typedef struct velawear_agent_audio_data
{
  uint16_t stream_id;
  uint32_t offset;
  const uint8_t *payload;
  uint16_t payload_length;
} velawear_agent_audio_data_t;

typedef struct velawear_agent_audio_end
{
  uint16_t stream_id;
  uint32_t total_bytes;
  uint32_t crc32;
} velawear_agent_audio_end_t;

int velawear_agent_encode_event(const velawear_agent_event_t *event,
                                uint8_t *frame, uint16_t frame_size);
int velawear_agent_decode_command(const uint8_t *frame, uint16_t frame_size,
                                  velawear_agent_command_t *command);
int velawear_agent_encode_command_result(uint16_t sequence, uint8_t command_id,
                                         uint8_t result, uint8_t *frame,
                                         uint16_t frame_size);
int velawear_agent_encode_message(uint16_t sequence, uint8_t priority,
                                  const char *text, uint8_t *frame,
                                  uint16_t frame_size);
int velawear_agent_encode_llm_request(uint16_t sequence,
                                      const char *prompt,
                                      uint8_t *frame, uint16_t frame_size);
int velawear_agent_decode_llm_response(const uint8_t *frame,
                                      uint16_t frame_size,
                                      velawear_agent_llm_response_t *response);
int velawear_agent_decode_audio_start(const uint8_t *frame,
                                      uint16_t frame_size,
                                      velawear_agent_audio_start_t *start);
int velawear_agent_decode_audio_data(const uint8_t *frame,
                                     uint16_t frame_size,
                                     velawear_agent_audio_data_t *data);
int velawear_agent_decode_audio_end(const uint8_t *frame,
                                    uint16_t frame_size,
                                    velawear_agent_audio_end_t *end);
uint32_t velawear_agent_crc32(const uint8_t *data, size_t length);
bool velawear_agent_command_sequence_is_newer(uint16_t candidate,
                                              uint16_t previous);

#endif /* __VELAWEAR_AGENT_PROTOCOL_H */
