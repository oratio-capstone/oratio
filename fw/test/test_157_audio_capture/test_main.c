// AB#157 capture test: plays the network task's role and prints a live level bar.
// Run: pio run -e audio_capture_test -t upload -t monitor, then talk or clap near the mic.
#include <stdint.h>
#include <string.h>

#include "audio_capture.h"
#include "audio_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"

#define BUFFER_FRAMES 4 // 80 ms of audio

static const char *TAG = "TEST_157";

static audio_frame_t s_frame;

static void log_level(void)
{
    int16_t min = INT16_MAX;
    int16_t max = INT16_MIN;
    for (size_t i = 0; i < s_frame.length; i++)
    {
        min = (s_frame.samples[i] < min) ? s_frame.samples[i] : min;
        max = (s_frame.samples[i] > max) ? s_frame.samples[i] : max;
    }

    int level = max - min; // peak-to-peak: how loud this frame was
    int len = (level / 200 > 40) ? 40 : level / 200;
    char bar[41];
    memset(bar, '#', (size_t)len);
    bar[len] = '\0';
    ESP_LOGI(TAG, "seq %5u level %6d |%s", (unsigned)s_frame.sequence, level, bar);
}

void app_main(void)
{
    // Each message also stores its length (size_t) inside the buffer.
    MessageBufferHandle_t buffer = xMessageBufferCreate(BUFFER_FRAMES * (sizeof(audio_frame_t) + sizeof(size_t)));
    if (buffer == NULL || audio_capture_open() != ESP_OK || audio_capture_task_start(buffer) != ESP_OK)
    {
        ESP_LOGE(TAG, "Setup failed");
        return;
    }

    uint16_t expected = 0;
    uint32_t frames = 0;
    uint32_t missing = 0;
    while (true)
    {
        if (xMessageBufferReceive(buffer, &s_frame, sizeof(s_frame), portMAX_DELAY) != sizeof(s_frame))
        {
            continue;
        }

        missing += (uint16_t)(s_frame.sequence - expected); // gaps mean frames were dropped
        expected = (uint16_t)(s_frame.sequence + 1);
        frames++;

        if (frames % 5 == 0) // 10 lines per second
        {
            log_level();
        }
        if (frames % 250 == 0) // every 5 s
        {
            ESP_LOGI(TAG, "received=%lu missing=%lu", (unsigned long)frames, (unsigned long)missing);
        }
    }
}
