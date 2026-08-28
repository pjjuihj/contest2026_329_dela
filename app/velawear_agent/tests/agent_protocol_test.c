#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../drivers/velawear_agent_protocol.h"

static int g_failures;

static void expect_true(int condition, const char *name)
{
    if (!condition)
    {
        fprintf(stderr, "FAIL: %s\\n", name);
        g_failures++;
    }
}

static void test_event_encoding(void)
{
    velawear_agent_event_t event =
    {
        .sequence = 0x1234,
        .event_type = VELAWEAR_AGENT_EVENT_FALL,
        .priority = VELAWEAR_AGENT_PRIORITY_CRITICAL,
        .intensity_percent = 100,
        .value = 7,
        .peak = 9,
        .flags = VELAWEAR_AGENT_EVENT_FLAG_ALERT,
    };
    uint8_t frame[VELAWEAR_AGENT_EVENT_FRAME_SIZE];
    const uint8_t expected[] = {1, 3, 0x34, 0x12, 3, 100, 7, 0, 9, 0, 1};

    expect_true(velawear_agent_encode_event(&event, frame, sizeof(frame)) ==
                (int)sizeof(expected), "event encode size");
    expect_true(memcmp(frame, expected, sizeof(expected)) == 0,
                "event encode bytes");
}

static void test_command_decoding(void)
{
    const uint8_t frame[] = {1, VELAWEAR_AGENT_COMMAND_SET_SEDENTARY_THRESHOLD,
                             0x34, 0x12, 0x2c, 0x01};
    velawear_agent_command_t command;

    expect_true(velawear_agent_decode_command(frame, sizeof(frame), &command) == 0,
                "valid command accepted");
    expect_true(command.sequence == 0x1234, "command sequence");
    expect_true(command.command_id ==
                VELAWEAR_AGENT_COMMAND_SET_SEDENTARY_THRESHOLD,
                "command id");
    expect_true(command.argument == 300, "command argument");
}

static void test_invalid_commands_are_rejected(void)
{
    const uint8_t unsupported_version[] = {2, VELAWEAR_AGENT_COMMAND_ACK_ALERT,
                                            1, 0, 0, 0};
    const uint8_t invalid_threshold[] =
        {1, VELAWEAR_AGENT_COMMAND_SET_SEDENTARY_THRESHOLD, 1, 0, 9, 0};
    const uint8_t invalid_ack[] = {1, VELAWEAR_AGENT_COMMAND_ACK_ALERT,
                                   1, 0, 1, 0};
    velawear_agent_command_t command;

    expect_true(velawear_agent_decode_command(unsupported_version,
                                               sizeof(unsupported_version),
                                               &command) < 0,
                "unsupported version rejected");
    expect_true(velawear_agent_decode_command(invalid_threshold,
                                               sizeof(invalid_threshold),
                                               &command) < 0,
                "out-of-range threshold rejected");
    expect_true(velawear_agent_decode_command(invalid_ack, sizeof(invalid_ack),
                                               &command) < 0,
                "ack argument rejected");
}

static void test_command_result_encoding(void)
{
    uint8_t frame[VELAWEAR_AGENT_RESULT_FRAME_SIZE];
    const uint8_t expected[] =
        {1, VELAWEAR_AGENT_COMMAND_SHOW_REMINDER, 0x34, 0x12, 0};

    expect_true(velawear_agent_encode_command_result(
                    0x1234, VELAWEAR_AGENT_COMMAND_SHOW_REMINDER,
                    VELAWEAR_AGENT_COMMAND_RESULT_OK, frame, sizeof(frame)) ==
                (int)sizeof(expected), "command result size");
    expect_true(memcmp(frame, expected, sizeof(expected)) == 0,
                "command result bytes");
    expect_true(velawear_agent_encode_command_result(
                    1, 0xff, VELAWEAR_AGENT_COMMAND_RESULT_OK,
                    frame, sizeof(frame)) < 0,
                "unknown result command rejected");
}

static void test_message_encoding(void)
{
    uint8_t frame[VELAWEAR_AGENT_MESSAGE_FRAME_MAX];
    const uint8_t expected[] =
        {1, VELAWEAR_AGENT_MESSAGE_TYPE, 0x34, 0x12, 3, 5, 0,
         'h', 'e', 'l', 'l', 'o'};
    char oversized[VELAWEAR_AGENT_MESSAGE_TEXT_MAX + 2];

    expect_true(velawear_agent_encode_message(
                    0x1234, VELAWEAR_AGENT_PRIORITY_CRITICAL, "hello",
                    frame, sizeof(frame)) == (int)sizeof(expected),
                "message encode size");
    expect_true(memcmp(frame, expected, sizeof(expected)) == 0,
                "message encode bytes");

    memset(oversized, 'x', sizeof(oversized));
    oversized[sizeof(oversized) - 1] = '\0';
    expect_true(velawear_agent_encode_message(
                    1, VELAWEAR_AGENT_PRIORITY_NORMAL, oversized,
                    frame, sizeof(frame)) < 0,
                "oversized message rejected");
}

static void test_llm_request_encoding(void)
{
    uint8_t frame[VELAWEAR_AGENT_LLM_REQUEST_FRAME_MAX];
    const uint8_t expected[] = {1, 1, 0x34, 0x12, 4, 0, 'p', 'i', 'n', 'g'};
    char oversized[VELAWEAR_AGENT_LLM_TEXT_MAX + 2];

    expect_true(velawear_agent_encode_llm_request(
                    0x1234, "ping", frame, sizeof(frame)) ==
                (int)sizeof(expected), "LLM request size");
    expect_true(memcmp(frame, expected, sizeof(expected)) == 0,
                "LLM request bytes");

    memset(oversized, 'x', sizeof(oversized));
    oversized[sizeof(oversized) - 1] = '\0';
    expect_true(velawear_agent_encode_llm_request(
                    1, oversized, frame, sizeof(frame)) < 0,
                "oversized LLM request rejected");
}

static void test_llm_response_decoding(void)
{
    const uint8_t frame[] = {1, 2, 0x34, 0x12, 0, 5, 0,
                             'h', 'e', 'l', 'l', 'o'};
    const uint8_t wrong_type[] = {1, 1, 0x34, 0x12, 0, 0, 0};
    const uint8_t wrong_length[] = {1, 2, 0x34, 0x12, 0, 6, 0,
                                    'h', 'e', 'l', 'l', 'o'};
    velawear_agent_llm_response_t response;

    expect_true(velawear_agent_decode_llm_response(
                    frame, sizeof(frame), &response) == 0,
                "valid LLM response accepted");
    expect_true(response.sequence == 0x1234, "LLM response sequence");
    expect_true(response.status == VELAWEAR_AGENT_LLM_STATUS_OK,
                "LLM response status");
    expect_true(response.length == 5 && strcmp(response.text, "hello") == 0,
                "LLM response text");
    expect_true(velawear_agent_decode_llm_response(
                    wrong_type, sizeof(wrong_type), &response) < 0,
                "wrong LLM response type rejected");
    expect_true(velawear_agent_decode_llm_response(
                    wrong_length, sizeof(wrong_length), &response) < 0,
                "truncated LLM response rejected");
}

static void test_audio_stream_decoding(void)
{
    const uint8_t start_frame[] = {
        1, VELAWEAR_AGENT_AUDIO_START_TYPE, 0x34, 0x12,
        0x80, 0x3e, 1, 16,
        2, 0, 0, 0,
        4, 0, 0, 0,
        0x12, 0x34, 0x56, 0x78,
    };
    const uint8_t data_frame[] = {
        1, VELAWEAR_AGENT_AUDIO_DATA_TYPE, 0x34, 0x12,
        0, 0, 0, 0,
        0x01, 0x00, 0xff, 0x7f,
    };
    const uint8_t end_frame[] = {
        1, VELAWEAR_AGENT_AUDIO_END_TYPE, 0x34, 0x12,
        4, 0, 0, 0,
        0x12, 0x34, 0x56, 0x78,
    };
    velawear_agent_audio_start_t start;
    velawear_agent_audio_data_t data;
    velawear_agent_audio_end_t end;

    expect_true(velawear_agent_decode_audio_start(
                    start_frame, sizeof(start_frame), &start) == 0,
                "audio start accepted");
    expect_true(start.stream_id == 0x1234 && start.sample_count == 2 &&
                    start.total_bytes == 4,
                "audio start fields");
    expect_true(velawear_agent_decode_audio_data(
                    data_frame, sizeof(data_frame), &data) == 0,
                "audio data accepted");
    expect_true(data.stream_id == 0x1234 && data.offset == 0 &&
                    data.payload_length == 4 && data.payload[2] == 0xff,
                "audio data fields");
    expect_true(velawear_agent_decode_audio_end(
                    end_frame, sizeof(end_frame), &end) == 0,
                "audio end accepted");
    expect_true(end.stream_id == 0x1234 && end.total_bytes == 4,
                "audio end fields");
    expect_true(velawear_agent_crc32((const uint8_t *)"abc", 3) ==
                    0x352441c2U,
                "audio CRC32");
}

static void test_invalid_audio_frames_are_rejected(void)
{
    uint8_t frame[VELAWEAR_AGENT_AUDIO_START_FRAME_SIZE] = {0};
    velawear_agent_audio_start_t start;
    velawear_agent_audio_data_t data;

    frame[0] = VELAWEAR_AGENT_PROTOCOL_VERSION;
    frame[1] = VELAWEAR_AGENT_AUDIO_START_TYPE;
    frame[4] = 0x80;
    frame[5] = 0x3e;
    frame[6] = 1;
    frame[7] = 16;
    frame[8] = 1;
    frame[12] = 3;
    expect_true(velawear_agent_decode_audio_start(
                    frame, sizeof(frame), &start) < 0,
                "inconsistent audio start rejected");

    {
        const uint8_t odd_payload[] = {
            1, VELAWEAR_AGENT_AUDIO_DATA_TYPE, 0, 0,
            0, 0, 0, 0, 0x01,
        };
        expect_true(velawear_agent_decode_audio_data(
                        odd_payload, sizeof(odd_payload), &data) < 0,
                    "odd audio sample rejected");
    }
}

static void test_command_sequence_replay_protection(void)
{
    expect_true(velawear_agent_command_sequence_is_newer(1, 0),
                "next sequence accepted");
    expect_true(!velawear_agent_command_sequence_is_newer(1, 1),
                "duplicate sequence rejected");
    expect_true(velawear_agent_command_sequence_is_newer(0, UINT16_MAX),
                "sequence wraps forward");
    expect_true(!velawear_agent_command_sequence_is_newer(0x8000, 0),
                "ambiguous half-range rejected");
}

int main(void)
{
    test_event_encoding();
    test_command_decoding();
    test_invalid_commands_are_rejected();
    test_command_result_encoding();
    test_message_encoding();
    test_llm_request_encoding();
    test_llm_response_decoding();
    test_audio_stream_decoding();
    test_invalid_audio_frames_are_rejected();
    test_command_sequence_replay_protection();

    if (g_failures != 0)
    {
        fprintf(stderr, "%d protocol test(s) failed\\n", g_failures);
        return 1;
    }

    puts("agent protocol tests passed");
    return 0;
}
