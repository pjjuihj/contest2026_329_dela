/* VelaWear Agent - compact BLE Agent protocol */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "velawear_agent_protocol.h"

static bool velawear_agent_command_is_known(uint8_t command_id)
{
  return command_id == VELAWEAR_AGENT_COMMAND_ACK_ALERT ||
         command_id == VELAWEAR_AGENT_COMMAND_SET_SEDENTARY_THRESHOLD ||
         command_id == VELAWEAR_AGENT_COMMAND_SHOW_REMINDER;
}

static bool velawear_agent_command_is_valid(uint8_t command_id,
                                            uint16_t argument)
{
  if (!velawear_agent_command_is_known(command_id))
    {
      return false;
    }

  switch (command_id)
    {
      case VELAWEAR_AGENT_COMMAND_ACK_ALERT:
        return argument == 0;

      case VELAWEAR_AGENT_COMMAND_SET_SEDENTARY_THRESHOLD:
        return argument >= 10 && argument <= 3600;

      case VELAWEAR_AGENT_COMMAND_SHOW_REMINDER:
        return argument <= VELAWEAR_AGENT_REMINDER_REST;

      default:
        return false;
    }
}

int velawear_agent_encode_event(const velawear_agent_event_t *event,
                                uint8_t *frame, uint16_t frame_size)
{
  if (event == NULL || frame == NULL ||
      frame_size < VELAWEAR_AGENT_EVENT_FRAME_SIZE)
    {
      return -EINVAL;
    }

  frame[0] = VELAWEAR_AGENT_PROTOCOL_VERSION;
  frame[1] = event->event_type;
  frame[2] = (uint8_t)(event->sequence & 0xff);
  frame[3] = (uint8_t)(event->sequence >> 8);
  frame[4] = event->priority;
  frame[5] = event->intensity_percent;
  frame[6] = (uint8_t)(event->value & 0xff);
  frame[7] = (uint8_t)(event->value >> 8);
  frame[8] = (uint8_t)(event->peak & 0xff);
  frame[9] = (uint8_t)(event->peak >> 8);
  frame[10] = event->flags;
  return VELAWEAR_AGENT_EVENT_FRAME_SIZE;
}

int velawear_agent_decode_command(const uint8_t *frame, uint16_t frame_size,
                                  velawear_agent_command_t *command)
{
  uint8_t command_id;
  uint16_t argument;

  if (frame == NULL || command == NULL ||
      frame_size != VELAWEAR_AGENT_COMMAND_FRAME_SIZE ||
      frame[0] != VELAWEAR_AGENT_PROTOCOL_VERSION)
    {
      return -EINVAL;
    }

  command_id = frame[1];
  argument = (uint16_t)frame[4] | ((uint16_t)frame[5] << 8);
  if (!velawear_agent_command_is_valid(command_id, argument))
    {
      return -EINVAL;
    }

  command->sequence = (uint16_t)frame[2] | ((uint16_t)frame[3] << 8);
  command->command_id = command_id;
  command->argument = argument;
  return 0;
}

int velawear_agent_encode_command_result(uint16_t sequence, uint8_t command_id,
                                         uint8_t result, uint8_t *frame,
                                         uint16_t frame_size)
{
  if (frame == NULL || frame_size < VELAWEAR_AGENT_RESULT_FRAME_SIZE ||
      !velawear_agent_command_is_known(command_id) ||
      result > VELAWEAR_AGENT_COMMAND_RESULT_EXECUTION_FAILED)
    {
      return -EINVAL;
    }

  frame[0] = VELAWEAR_AGENT_PROTOCOL_VERSION;
  frame[1] = command_id;
  frame[2] = (uint8_t)(sequence & 0xff);
  frame[3] = (uint8_t)(sequence >> 8);
  frame[4] = result;
  return VELAWEAR_AGENT_RESULT_FRAME_SIZE;
}

int velawear_agent_encode_llm_request(uint16_t sequence,
                                      const char *prompt,
                                      uint8_t *frame, uint16_t frame_size)
{
  size_t length;

  if (prompt == NULL || frame == NULL)
    {
      return -EINVAL;
    }

  length = strlen(prompt);
  if (length > VELAWEAR_AGENT_LLM_TEXT_MAX ||
      frame_size < VELAWEAR_AGENT_LLM_REQUEST_HEADER_SIZE + length)
    {
      return -EMSGSIZE;
    }

  frame[0] = VELAWEAR_AGENT_PROTOCOL_VERSION;
  frame[1] = VELAWEAR_AGENT_LLM_REQUEST_TYPE;
  frame[2] = (uint8_t)(sequence & 0xff);
  frame[3] = (uint8_t)(sequence >> 8);
  frame[4] = (uint8_t)(length & 0xff);
  frame[5] = (uint8_t)(length >> 8);
  memcpy(frame + VELAWEAR_AGENT_LLM_REQUEST_HEADER_SIZE, prompt, length);
  return VELAWEAR_AGENT_LLM_REQUEST_HEADER_SIZE + (int)length;
}

int velawear_agent_decode_llm_response(const uint8_t *frame,
                                      uint16_t frame_size,
                                      velawear_agent_llm_response_t *response)
{
  uint16_t length;
  uint16_t expected_size;

  if (frame == NULL || response == NULL ||
      frame_size < VELAWEAR_AGENT_LLM_RESPONSE_HEADER_SIZE ||
      frame[0] != VELAWEAR_AGENT_PROTOCOL_VERSION ||
      frame[1] != VELAWEAR_AGENT_LLM_RESPONSE_TYPE)
    {
      return -EINVAL;
    }

  length = (uint16_t)frame[5] | ((uint16_t)frame[6] << 8);
  if (length > VELAWEAR_AGENT_LLM_TEXT_MAX ||
      frame[4] > VELAWEAR_AGENT_LLM_STATUS_ERROR)
    {
      return -EINVAL;
    }

  expected_size = (uint16_t)(VELAWEAR_AGENT_LLM_RESPONSE_HEADER_SIZE + length);
  if (frame_size != expected_size)
    {
      return -EINVAL;
    }

  memset(response, 0, sizeof(*response));
  response->sequence = (uint16_t)frame[2] | ((uint16_t)frame[3] << 8);
  response->status = frame[4];
  response->length = length;
  memcpy(response->text, frame + VELAWEAR_AGENT_LLM_RESPONSE_HEADER_SIZE,
         length);
  response->text[length] = '\0';
  return 0;
}

static uint16_t velawear_agent_read_u16(const uint8_t *data)
{
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t velawear_agent_read_u32(const uint8_t *data)
{
  return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24);
}

int velawear_agent_decode_audio_start(const uint8_t *frame,
                                      uint16_t frame_size,
                                      velawear_agent_audio_start_t *start)
{
  uint32_t sample_count;
  uint32_t total_bytes;

  if (frame == NULL || start == NULL ||
      frame_size != VELAWEAR_AGENT_AUDIO_START_FRAME_SIZE ||
      frame[0] != VELAWEAR_AGENT_PROTOCOL_VERSION ||
      frame[1] != VELAWEAR_AGENT_AUDIO_START_TYPE)
    {
      return -EINVAL;
    }

  sample_count = velawear_agent_read_u32(frame + 8);
  total_bytes = velawear_agent_read_u32(frame + 12);
  if (velawear_agent_read_u16(frame + 4) !=
          VELAWEAR_AGENT_AUDIO_SAMPLE_RATE ||
      frame[6] != VELAWEAR_AGENT_AUDIO_CHANNELS ||
      frame[7] != VELAWEAR_AGENT_AUDIO_BITS_PER_SAMPLE ||
      sample_count == 0 || sample_count > VELAWEAR_AGENT_AUDIO_MAX_PCM_BYTES / 2 ||
      total_bytes != sample_count * 2U ||
      total_bytes > VELAWEAR_AGENT_AUDIO_MAX_PCM_BYTES)
    {
      return -EINVAL;
    }

  start->stream_id = velawear_agent_read_u16(frame + 2);
  start->sample_rate = velawear_agent_read_u16(frame + 4);
  start->channels = frame[6];
  start->bits_per_sample = frame[7];
  start->sample_count = sample_count;
  start->total_bytes = total_bytes;
  start->crc32 = velawear_agent_read_u32(frame + 16);
  return 0;
}

int velawear_agent_decode_audio_data(const uint8_t *frame,
                                     uint16_t frame_size,
                                     velawear_agent_audio_data_t *data)
{
  uint32_t offset;
  uint16_t payload_length;

  if (frame == NULL || data == NULL ||
      frame_size <= VELAWEAR_AGENT_AUDIO_DATA_HEADER_SIZE ||
      frame[0] != VELAWEAR_AGENT_PROTOCOL_VERSION ||
      frame[1] != VELAWEAR_AGENT_AUDIO_DATA_TYPE)
    {
      return -EINVAL;
    }

  offset = velawear_agent_read_u32(frame + 4);
  payload_length = frame_size - VELAWEAR_AGENT_AUDIO_DATA_HEADER_SIZE;
  if ((offset & 1U) != 0 || (payload_length & 1U) != 0 ||
      offset > VELAWEAR_AGENT_AUDIO_MAX_PCM_BYTES ||
      payload_length > VELAWEAR_AGENT_AUDIO_MAX_PCM_BYTES - offset)
    {
      return -EINVAL;
    }

  data->stream_id = velawear_agent_read_u16(frame + 2);
  data->offset = offset;
  data->payload = frame + VELAWEAR_AGENT_AUDIO_DATA_HEADER_SIZE;
  data->payload_length = payload_length;
  return 0;
}

int velawear_agent_decode_audio_end(const uint8_t *frame,
                                    uint16_t frame_size,
                                    velawear_agent_audio_end_t *end)
{
  if (frame == NULL || end == NULL ||
      frame_size != VELAWEAR_AGENT_AUDIO_END_FRAME_SIZE ||
      frame[0] != VELAWEAR_AGENT_PROTOCOL_VERSION ||
      frame[1] != VELAWEAR_AGENT_AUDIO_END_TYPE)
    {
      return -EINVAL;
    }

  end->stream_id = velawear_agent_read_u16(frame + 2);
  end->total_bytes = velawear_agent_read_u32(frame + 4);
  end->crc32 = velawear_agent_read_u32(frame + 8);
  if (end->total_bytes == 0 ||
      end->total_bytes > VELAWEAR_AGENT_AUDIO_MAX_PCM_BYTES ||
      (end->total_bytes & 1U) != 0)
    {
      return -EINVAL;
    }

  return 0;
}

uint32_t velawear_agent_crc32(const uint8_t *data, size_t length)
{
  uint32_t crc = UINT32_MAX;

  if (data == NULL && length != 0)
    {
      return 0;
    }

  for (size_t i = 0; i < length; i++)
    {
      crc ^= data[i];
      for (uint8_t bit = 0; bit < 8; bit++)
        {
          crc = (crc & 1U) != 0 ? (crc >> 1) ^ 0xedb88320U : crc >> 1;
        }
    }

  return crc ^ UINT32_MAX;
}

int velawear_agent_encode_message(uint16_t sequence, uint8_t priority,
                                  const char *text, uint8_t *frame,
                                  uint16_t frame_size)
{
  size_t length;

  if (text == NULL || frame == NULL)
    {
      return -EINVAL;
    }

  length = strlen(text);
  if (length > VELAWEAR_AGENT_MESSAGE_TEXT_MAX ||
      frame_size < VELAWEAR_AGENT_MESSAGE_HEADER_SIZE + length)
    {
      return -EMSGSIZE;
    }

  frame[0] = VELAWEAR_AGENT_PROTOCOL_VERSION;
  frame[1] = VELAWEAR_AGENT_MESSAGE_TYPE;
  frame[2] = (uint8_t)(sequence & 0xff);
  frame[3] = (uint8_t)(sequence >> 8);
  frame[4] = priority;
  frame[5] = (uint8_t)(length & 0xff);
  frame[6] = (uint8_t)(length >> 8);
  memcpy(frame + VELAWEAR_AGENT_MESSAGE_HEADER_SIZE, text, length);
  return VELAWEAR_AGENT_MESSAGE_HEADER_SIZE + (int)length;
}

bool velawear_agent_command_sequence_is_newer(uint16_t candidate,
                                              uint16_t previous)
{
  uint16_t distance = (uint16_t)(candidate - previous);

  return distance != 0 && distance < 0x8000;
}
