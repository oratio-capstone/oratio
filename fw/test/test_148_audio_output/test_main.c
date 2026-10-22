#include "audio_common.h"
#include "audio_output.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"

#if !defined(ORATIO_AUDIO_TEST_BCLK_GPIO) || !defined(ORATIO_AUDIO_TEST_WS_GPIO) || \
    !defined(ORATIO_AUDIO_TEST_DOUT_GPIO)
#error "Select the standalone test GPIOs in platformio.ini before building"
#endif

static const char *TAG = "TEST_148";
static const float TWO_PI = 6.2831853071795864769f;
static const float TONE_FREQUENCY_HZ = 440.0f;
static const int16_t TONE_AMPLITUDE = 512;
static const uint32_t WRITE_TIMEOUT_MS = 100;
static const int64_t SUMMARY_INTERVAL_US = 1000000;

static int16_t s_tone_samples[AUDIO_SAMPLES_PER_FRAME];

static void output_cleanup(void)
{
    const esp_err_t result = audio_output_deinit();
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Output cleanup failed: %s", esp_err_to_name(result));
    }
}

void app_main(void)
{
    const audio_output_config_t config =
    {
        .bclk_gpio = (gpio_num_t)ORATIO_AUDIO_TEST_BCLK_GPIO,
        .ws_gpio = (gpio_num_t)ORATIO_AUDIO_TEST_WS_GPIO,
        .dout_gpio = (gpio_num_t)ORATIO_AUDIO_TEST_DOUT_GPIO
    };

    ESP_LOGW(TAG, "Standalone tone test: confirm board, pins, speaker and power wiring before use");
    esp_err_t result = audio_output_init(&config);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Output init failed: %s", esp_err_to_name(result));
        output_cleanup();
        return;
    }

    result = audio_output_start();
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Output start failed: %s", esp_err_to_name(result));
        output_cleanup();
        return;
    }

    float phase = 0.0f;
    const float phase_step = TWO_PI * TONE_FREQUENCY_HZ / AUDIO_SAMPLE_RATE_HZ;
    uint32_t completed_frames = 0;
    int64_t last_summary_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Continuous 440 Hz tone, amplitude 512/32768; check at least 30 seconds, reset to end");

    while (true)
    {
        for (size_t index = 0; index < AUDIO_SAMPLES_PER_FRAME; index++)
        {
            s_tone_samples[index] = (int16_t)(sinf(phase) * TONE_AMPLITUDE);
            phase += phase_step;
            if (phase >= TWO_PI)
            {
                phase -= TWO_PI;
            }
        }

        size_t samples_written = 0;
        result = audio_output_write(s_tone_samples, AUDIO_SAMPLES_PER_FRAME, &samples_written, WRITE_TIMEOUT_MS);
        if (result != ESP_OK)
        {
            ESP_LOGE(TAG, "Tone stopped: %s after %zu samples of this frame", esp_err_to_name(result), samples_written);
            output_cleanup();
            return;
        }

        completed_frames++;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_summary_us >= SUMMARY_INTERVAL_US)
        {
            ESP_LOGI(TAG, "Frames accepted: %lu; I2S writes pace output", (unsigned long)completed_frames);
            last_summary_us = now_us;
        }
    }
}
