#include "audio_common.h"
#include "audio_output.h"
#include "playback.h"
#include "freertos/task.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

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

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    UNITY_BEGIN();
    RUN_TEST(test_output_contract);
    RUN_TEST(test_direct_tone);
    UNITY_END();
}
