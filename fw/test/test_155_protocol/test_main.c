#include "network.h"
#include "unity.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void setUp(void) {}
void tearDown(void) {}

static void test_encode_heartbeat(void)
{
    /* Arrange inputs, call the encoder, then check its output. */
    oratio_packet_t packet = {
        .type = ORATIO_PACKET_HEARTBEAT,
        .node_id = 0x1234,
        .sequence = 0x0102,
        .length = 0};

    uint8_t buffer[ORATIO_HEADER_SIZE] = {0};
    size_t encoded_length = 0;

    const uint8_t expected[] = {
        'H', 0x12, 0x34, 0x01, 0x02, 0x00, 0x00};

    esp_err_t result = oratio_packet_encode(&packet, buffer, sizeof(buffer), &encoded_length);

    TEST_ASSERT_EQUAL(ESP_OK, result);
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), encoded_length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buffer, sizeof(expected));
}

static void test_decode_heartbeat(void)
{
    const uint8_t buffer[] = {
        'H', 0x12, 0x34, 0x01, 0x02, 0x00, 0x00};

    oratio_packet_t packet = {0};

    /* Decode, then check the result and each field. */
    esp_err_t result = oratio_packet_decode(buffer, sizeof(buffer), &packet);
    TEST_ASSERT_EQUAL(ESP_OK, result);
    TEST_ASSERT_EQUAL(ORATIO_PACKET_HEARTBEAT, packet.type);
    TEST_ASSERT_EQUAL(0x1234, packet.node_id);
    TEST_ASSERT_EQUAL(0x0102, packet.sequence);
    TEST_ASSERT_EQUAL(0, packet.length);
}

static void test_encode_telemetry_payload(void)
{
    oratio_packet_t packet = {
        .type = ORATIO_PACKET_TELEMETRY,
        .node_id = 0x1234,
        .sequence = 0x0102,
        .length = 3,
        .payload = {0x00, 0x7F, 0xFF}};

    const uint8_t expected[] = {
        'T', 0x12, 0x34, 0x01, 0x02, 0x00, 0x03,
        0x00, 0x7F, 0xFF};

    uint8_t buffer[ORATIO_HEADER_SIZE + 3] = {0};
    size_t encoded_length = 0;

    esp_err_t result = oratio_packet_encode(&packet, buffer, sizeof(buffer), &encoded_length);

    TEST_ASSERT_EQUAL(ESP_OK, result);
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), encoded_length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buffer, sizeof(expected));
}

static void test_decode_telemetry_payload(void)
{
    const uint8_t buffer[] = {
        'T', 0x12, 0x34, 0x01, 0x02, 0x00, 0x03,
        0x00, 0x7F, 0xFF};

    oratio_packet_t packet = {0};

    esp_err_t result = oratio_packet_decode(buffer, sizeof(buffer), &packet);
    TEST_ASSERT_EQUAL(ESP_OK, result);
    TEST_ASSERT_EQUAL(ORATIO_PACKET_TELEMETRY, packet.type);
    TEST_ASSERT_EQUAL(0x1234, packet.node_id);
    TEST_ASSERT_EQUAL(0x0102, packet.sequence);
    TEST_ASSERT_EQUAL(3, packet.length);

    const uint8_t expected_payload[] = {0x00, 0x7F, 0xFF};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        expected_payload,
        packet.payload,
        sizeof(expected_payload));
}

static void test_decode_invalid_packet_type(void)
{
    const uint8_t buffer[] = {
        'X', 0x12, 0x34, 0x01, 0x02, 0x00, 0x00};

    oratio_packet_t packet = {0};

    esp_err_t result = oratio_packet_decode(buffer, sizeof(buffer), &packet);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, result);
}

static void test_decode_invalid_length(void)
{
    const uint8_t buffer[] = {
        'T', 0x12, 0x34, 0x01, 0x02, 0x00, 0x05,
        0x00, 0x7F, 0xFF};

    oratio_packet_t packet = {0};

    esp_err_t result = oratio_packet_decode(buffer, sizeof(buffer), &packet);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, result);
}

static void test_decode_heartbeat_with_payload(void)
{
    const uint8_t buffer[] = {
        'H', 0x12, 0x34, 0x01, 0x02, 0x00, 0x01,
        0x00};

    oratio_packet_t packet = {0};

    esp_err_t result = oratio_packet_decode(buffer, sizeof(buffer), &packet);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, result);
}

static void test_decode_short_header(void)
{
    const uint8_t buffer[] = {
        'T', 0x12, 0x34, 0x01};

    oratio_packet_t packet = {0};

    esp_err_t result = oratio_packet_decode(buffer, sizeof(buffer), &packet);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, result);
}

/* Invalid inputs must be rejected before they can overrun a payload buffer. */
static void test_encode_invalid_type(void)
{
    oratio_packet_t packet = {.type = 'X'};
    uint8_t buffer[ORATIO_HEADER_SIZE];
    size_t length = 123;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        oratio_packet_encode(&packet, buffer, sizeof(buffer), &length));
    TEST_ASSERT_EQUAL_UINT32(0, length);
}

static void test_encode_oversized_payload(void)
{
    oratio_packet_t packet = {
        .type = ORATIO_PACKET_AUDIO, .length = ORATIO_MAX_PAYLOAD_SIZE + 1};
    uint8_t buffer[ORATIO_HEADER_SIZE + ORATIO_MAX_PAYLOAD_SIZE + 1];
    size_t length = 123;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        oratio_packet_encode(&packet, buffer, sizeof(buffer), &length));
    TEST_ASSERT_EQUAL_UINT32(0, length);
}

static void test_encode_heartbeat_with_payload(void)
{
    oratio_packet_t packet = {.type = ORATIO_PACKET_HEARTBEAT, .length = 1};
    uint8_t buffer[ORATIO_HEADER_SIZE + 1];
    size_t length = 123;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        oratio_packet_encode(&packet, buffer, sizeof(buffer), &length));
    TEST_ASSERT_EQUAL_UINT32(0, length);
}

static void test_encode_insufficient_capacity(void)
{
    oratio_packet_t packet = {.type = ORATIO_PACKET_TELEMETRY, .length = 3};
    uint8_t buffer[ORATIO_HEADER_SIZE + 3];
    /* Try every capacity smaller than the required ten bytes. */
    for (size_t capacity = 0; capacity < sizeof(buffer); ++capacity)
    {
        memset(buffer, 0xA5, sizeof(buffer));
        size_t length = 123;
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
            oratio_packet_encode(&packet, buffer, capacity, &length));
        TEST_ASSERT_EQUAL_UINT32(0, length);
        TEST_ASSERT_EACH_EQUAL_UINT8(0xA5, buffer, sizeof(buffer));
    }
}

static void test_encode_null_arguments(void)
{
    oratio_packet_t packet = {.type = ORATIO_PACKET_HEARTBEAT};
    uint8_t buffer[ORATIO_HEADER_SIZE];
    size_t length = 123;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        oratio_packet_encode(NULL, buffer, sizeof(buffer), &length));
    TEST_ASSERT_EQUAL_UINT32(0, length);
    length = 123;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        oratio_packet_encode(&packet, NULL, sizeof(buffer), &length));
    TEST_ASSERT_EQUAL_UINT32(0, length);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        oratio_packet_encode(&packet, buffer, sizeof(buffer), NULL));
}

static void test_decode_null_arguments(void)
{
    const uint8_t buffer[] = {'H', 0, 0, 0, 0, 0, 0};
    oratio_packet_t packet = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        oratio_packet_decode(NULL, sizeof(buffer), &packet));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        oratio_packet_decode(buffer, sizeof(buffer), NULL));
}

static void test_decode_oversized_payload(void)
{
    /* 0x0281 = 641; supply all declared bytes so only the maximum rule fails. */
    uint8_t buffer[ORATIO_HEADER_SIZE + ORATIO_MAX_PAYLOAD_SIZE + 1] = {
        'T', 0x12, 0x34, 0x01, 0x02, 0x02, 0x81};
    struct {
        oratio_packet_t packet;
        uint8_t guard[8];
    } output = {0};
    memset(output.guard, 0xA5, sizeof(output.guard));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
        oratio_packet_decode(buffer, sizeof(buffer), &output.packet));
    TEST_ASSERT_EACH_EQUAL_UINT8(0xA5, output.guard, sizeof(output.guard));
}

static void test_decode_trailing_bytes(void)
{
    const uint8_t buffer[] = {'T', 0x12, 0x34, 0, 1, 0, 1, 0xAA, 0xBB};
    oratio_packet_t packet = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
        oratio_packet_decode(buffer, sizeof(buffer), &packet));
}

/* Known wire bytes test both byte order and the full 640-byte boundary.
 * Keep expected data independent of the encoder/decoder under test.
 */
static void test_encode_maximum_audio_payload(void)
{
    oratio_packet_t packet = {
        .type = ORATIO_PACKET_AUDIO, .node_id = 0xFEDC,
        .sequence = 0xFFFF, .length = ORATIO_MAX_PAYLOAD_SIZE};
    uint8_t buffer[ORATIO_HEADER_SIZE + ORATIO_MAX_PAYLOAD_SIZE + 2];
    memset(buffer, 0xA5, sizeof(buffer));
    for (size_t i = 0; i < ORATIO_MAX_PAYLOAD_SIZE; ++i)
    {
        packet.payload[i] = (uint8_t)i;
    }
    size_t length = 0;
    TEST_ASSERT_EQUAL(ESP_OK,
        oratio_packet_encode(&packet, buffer + 1, sizeof(buffer) - 2, &length));
    const uint8_t header[] = {'A', 0xFE, 0xDC, 0xFF, 0xFF, 0x02, 0x80};
    TEST_ASSERT_EQUAL_UINT32(647, length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(header, buffer + 1, sizeof(header));
    for (size_t i = 0; i < ORATIO_MAX_PAYLOAD_SIZE; ++i)
    {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)i, buffer[1 + ORATIO_HEADER_SIZE + i]);
    }
    TEST_ASSERT_EQUAL_HEX8(0xA5, buffer[0]);
    TEST_ASSERT_EQUAL_HEX8(0xA5, buffer[sizeof(buffer) - 1]);
}

static void test_decode_maximum_audio_payload(void)
{
    uint8_t buffer[ORATIO_HEADER_SIZE + ORATIO_MAX_PAYLOAD_SIZE] = {
        'A', 0xFE, 0xDC, 0xFF, 0xFF, 0x02, 0x80};
    for (size_t i = 0; i < ORATIO_MAX_PAYLOAD_SIZE; ++i)
    {
        buffer[ORATIO_HEADER_SIZE + i] = (uint8_t)i;
    }
    oratio_packet_t packet = {0};
    TEST_ASSERT_EQUAL(ESP_OK, oratio_packet_decode(buffer, sizeof(buffer), &packet));
    TEST_ASSERT_EQUAL_UINT8(ORATIO_PACKET_AUDIO, packet.type);
    TEST_ASSERT_EQUAL_HEX16(0xFEDC, packet.node_id);
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, packet.sequence);
    TEST_ASSERT_EQUAL_UINT16(640, packet.length);
    for (size_t i = 0; i < ORATIO_MAX_PAYLOAD_SIZE; ++i)
    {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)i, packet.payload[i]);
    }
}

static void test_zero_header_values_and_empty_payloads(void)
{
    /* No minimum payload size has been specified for audio or telemetry. */
    const uint8_t types[] = {'A', 'T', 'H'};
    for (size_t i = 0; i < sizeof(types); ++i)
    {
        const uint8_t expected[] = {types[i], 0, 0, 0, 0, 0, 0};
        oratio_packet_t packet = {.type = types[i]};
        uint8_t buffer[ORATIO_HEADER_SIZE];
        size_t length = 0;
        TEST_ASSERT_EQUAL(ESP_OK,
            oratio_packet_encode(&packet, buffer, sizeof(buffer), &length));
        TEST_ASSERT_EQUAL_UINT32(sizeof(expected), length);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buffer, sizeof(expected));
        /* Deliberately dirty output verifies decoding does not use old fields. */
        memset(&packet, 0xA5, sizeof(packet));
        TEST_ASSERT_EQUAL(ESP_OK,
            oratio_packet_decode(expected, sizeof(expected), &packet));
        TEST_ASSERT_EQUAL_UINT8(types[i], packet.type);
        TEST_ASSERT_EQUAL_UINT16(0, packet.node_id);
        TEST_ASSERT_EQUAL_UINT16(0, packet.sequence);
        TEST_ASSERT_EQUAL_UINT16(0, packet.length);
    }
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    UNITY_BEGIN();
    RUN_TEST(test_encode_heartbeat);
    RUN_TEST(test_decode_heartbeat);
    RUN_TEST(test_encode_telemetry_payload);
    RUN_TEST(test_decode_telemetry_payload);
    RUN_TEST(test_decode_invalid_packet_type);
    RUN_TEST(test_decode_invalid_length);
    RUN_TEST(test_decode_heartbeat_with_payload);
    RUN_TEST(test_decode_short_header);
    RUN_TEST(test_encode_invalid_type);
    RUN_TEST(test_encode_oversized_payload);
    RUN_TEST(test_encode_heartbeat_with_payload);
    RUN_TEST(test_encode_insufficient_capacity);
    RUN_TEST(test_encode_null_arguments);
    RUN_TEST(test_decode_null_arguments);
    RUN_TEST(test_decode_oversized_payload);
    RUN_TEST(test_decode_trailing_bytes);
    RUN_TEST(test_encode_maximum_audio_payload);
    RUN_TEST(test_decode_maximum_audio_payload);
    RUN_TEST(test_zero_header_values_and_empty_payloads);
    UNITY_END();
}