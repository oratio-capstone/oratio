// AB#157 audio capture test. Reads the INMP441 for 5 s, prints a live level bar, then passes or fails.
// Run: pio test -e esp32-s3-n8r8 -f test_157_audio_capture   (talk or clap near the mic while it runs)
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "audio_capture.h"
#include "audio_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"

#define TEST_FRAMES 250 // 250 frames x 20 ms = 5 s

static const char *TAG = "TEST_157";

static int16_t s_samples[AUDIO_SAMPLES_PER_FRAME];

void setUp(void)
{
}

void tearDown(void)
{
}

// Peak-to-peak of the last frame read: how loud it was.
static int frame_level(void)
{
    int16_t min = INT16_MAX;
    int16_t max = INT16_MIN;
    for (size_t i = 0; i < AUDIO_SAMPLES_PER_FRAME; i++)
    {
        min = (s_samples[i] < min) ? s_samples[i] : min;
        max = (s_samples[i] > max) ? s_samples[i] : max;
    }
    return max - min;
}

static void log_level(uint32_t frame, int level)
{
    int len = (level / 200 > 40) ? 40 : level / 200;
    char bar[41];
    memset(bar, '#', (size_t)len);
    bar[len] = '\0';
    ESP_LOGI(TAG, "frame %3lu level %6d |%s", (unsigned long)frame, level, bar);
}

static void test_open_read_close_contract(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, audio_capture_read(s_samples, AUDIO_SAMPLES_PER_FRAME)); // not open
    TEST_ASSERT_EQUAL(ESP_OK, audio_capture_open());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, audio_capture_open()); // already open
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, audio_capture_read(NULL, AUDIO_SAMPLES_PER_FRAME));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, audio_capture_read(s_samples, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, audio_capture_read(s_samples, AUDIO_SAMPLES_PER_FRAME + 1));
    TEST_ASSERT_EQUAL(ESP_OK, audio_capture_close());
    TEST_ASSERT_EQUAL(ESP_OK, audio_capture_close()); // closing twice is safe
}

static void test_mic_reads_for_5_seconds(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, audio_capture_open());

    uint32_t failed_reads = 0;
    bool any_nonzero = false;
    int loudest = 0;
    for (uint32_t frame = 0; frame < TEST_FRAMES; frame++)
    {
        if (audio_capture_read(s_samples, AUDIO_SAMPLES_PER_FRAME) != ESP_OK)
        {
            failed_reads++;
            continue;
        }

        for (size_t i = 0; i < AUDIO_SAMPLES_PER_FRAME && !any_nonzero; i++)
        {
            any_nonzero = (s_samples[i] != 0);
        }
        const int level = frame_level();
        loudest = (level > loudest) ? level : loudest;
        if (frame % 5 == 0) // 10 lines per second
        {
            log_level(frame, level);
        }
    }

    ESP_LOGI(TAG, "%d frames, %lu failed reads, loudest level %d", TEST_FRAMES, (unsigned long)failed_reads, loudest);
    TEST_ASSERT_EQUAL(ESP_OK, audio_capture_close());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, failed_reads, "I2S reads failed: check SCK and WS wiring");
    TEST_ASSERT_TRUE_MESSAGE(any_nonzero, "Every sample was zero: check SD wiring and L/R to GND");
    TEST_ASSERT_TRUE_MESSAGE(loudest > 0, "Signal never changed: the microphone is not producing audio");
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(2000)); // give the serial monitor time to connect
    UNITY_BEGIN();
    RUN_TEST(test_open_read_close_contract);
    RUN_TEST(test_mic_reads_for_5_seconds);
    UNITY_END();
}
