#include "audio_common.h"
#include "audio_output.h"
#include "playback.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "unity.h"

static const char *TAG = "AUDIO_TEST";

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_output_contract(void)
{
    const int16_t sample = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, audio_output_write(NULL, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, audio_output_write(&sample, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, audio_output_write(&sample, AUDIO_SAMPLES_PER_FRAME + 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, audio_output_write(&sample, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, audio_output_set_enabled(true));
    TEST_ASSERT_EQUAL(ESP_OK, audio_output_close());
}

static void test_direct_tone(void)
{
    int16_t samples[AUDIO_SAMPLES_PER_FRAME];
    for (size_t i = 0; i < AUDIO_SAMPLES_PER_FRAME; i++)
    {
        samples[i] = i % 32 < 16 ? 512 : -512; // Quiet 500 Hz square wave.
    }
    TEST_ASSERT_EQUAL(ESP_OK, audio_output_open());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, audio_output_open());
    for (size_t frame = 0; frame < 150; frame++)
    {
        TEST_ASSERT_EQUAL(ESP_OK, audio_output_write(samples, AUDIO_SAMPLES_PER_FRAME));
    }
    TEST_ASSERT_EQUAL(ESP_OK, audio_output_set_enabled(false));
    for (size_t frame = 0; frame < 50; frame++)
    {
        TEST_ASSERT_EQUAL(ESP_OK, audio_output_write(samples, AUDIO_SAMPLES_PER_FRAME));
    }
    TEST_ASSERT_EQUAL(ESP_OK, audio_output_set_enabled(true));
    TEST_ASSERT_EQUAL(ESP_OK, audio_output_write(samples, 16));
    TEST_ASSERT_EQUAL(ESP_OK, audio_output_close());
    TEST_ASSERT_EQUAL(ESP_OK, audio_output_close());
}

static void test_buffered_playback(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, playback_task_start(NULL));
    // The consumer lives until reboot, so this buffer must also live until reboot.
    MessageBufferHandle_t input =
        xMessageBufferCreate(4 * (sizeof(audio_frame_t) + sizeof(configMESSAGE_BUFFER_LENGTH_TYPE)));
    TEST_ASSERT_NOT_NULL(input);
    TEST_ASSERT_EQUAL(ESP_OK, audio_output_open());
    TEST_ASSERT_EQUAL(ESP_OK, playback_task_start(input));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, playback_task_start(input));

    audio_frame_t frame = { .length = AUDIO_SAMPLES_PER_FRAME };
    for (size_t i = 0; i < AUDIO_SAMPLES_PER_FRAME; i++)
    {
        frame.samples[i] = i % 32 < 16 ? 512 : -512;
    }
    for (uint16_t sequence = 0; sequence < 200; sequence++)
    {
        frame.sequence = sequence;
        frame.length = sequence < 100 ? AUDIO_SAMPLES_PER_FRAME : AUDIO_SAMPLES_PER_FRAME / 2;
        TEST_ASSERT_EQUAL(sizeof(frame), xMessageBufferSend(input, &frame, sizeof(frame), pdMS_TO_TICKS(1000)));
        if (sequence == 99)
        {
            vTaskDelay(pdMS_TO_TICKS(1000)); // Listener checks silence and later recovery.
        }
    }
    frame.length = AUDIO_SAMPLES_PER_FRAME + 1;
    TEST_ASSERT_EQUAL(sizeof(frame), xMessageBufferSend(input, &frame, sizeof(frame), pdMS_TO_TICKS(1000)));
    frame.length = 0;
    TEST_ASSERT_EQUAL(sizeof(frame), xMessageBufferSend(input, &frame, sizeof(frame), pdMS_TO_TICKS(1000)));
    TEST_ASSERT_EQUAL(2, xMessageBufferSend(input, &frame, 2, pdMS_TO_TICKS(1000)));
    frame.length = AUDIO_SAMPLES_PER_FRAME;
    TEST_ASSERT_EQUAL(sizeof(frame), xMessageBufferSend(input, &frame, sizeof(frame), pdMS_TO_TICKS(1000)));
    const TickType_t start = xTaskGetTickCount();
    while (!xMessageBufferIsEmpty(input) && xTaskGetTickCount() - start < pdMS_TO_TICKS(2000))
    {
        vTaskDelay(1);
    }
    TEST_ASSERT_TRUE(xMessageBufferIsEmpty(input));
    // Queue drain proves consumption, not audible output or successful DMA writes.
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "Listen for direct tone, amp shutdown, buffered tone, pause and recovery");
    UNITY_BEGIN();
    RUN_TEST(test_output_contract);
    RUN_TEST(test_direct_tone);
    RUN_TEST(test_buffered_playback);
    UNITY_END();
}
