#include "playback.h"
#include "audio_common.h"
#include "audio_output.h"

#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "PLAYBACK";
// Startup calls task_start once, after opening the driver. The caller retains
// the input buffer for this task's lifetime and supplies exactly one producer.
static TaskHandle_t s_task;

static void playback_task(void *argument)
{
    MessageBufferHandle_t input = (MessageBufferHandle_t)argument;
    static audio_frame_t frame;
    uint32_t errors = 0;
    TickType_t last_log = xTaskGetTickCount();
    while (true)
    {
        const size_t received = xMessageBufferReceive(input, &frame, sizeof(frame), portMAX_DELAY);
        configASSERT(received != 0); // Producer must send messages that fit audio_frame_t.
        // Messages contain the whole struct; length counts valid samples, not bytes.
        // Sequence is intentionally unused until packet reordering is introduced.
        if (received != sizeof(frame) || frame.length == 0 || frame.length > AUDIO_SAMPLES_PER_FRAME)
        {
            errors++;
        }
        else if (audio_output_write(frame.samples, frame.length) != ESP_OK)
        {
            errors++;
        }
        const TickType_t now = xTaskGetTickCount();
        if (errors != 0 && now - last_log >= pdMS_TO_TICKS(2000))
        {
            ESP_LOGW(TAG, "Rejected frames/write failures: %lu", (unsigned long)errors);
            errors = 0;
            last_log = now;
        }
    }
}

esp_err_t playback_task_start(MessageBufferHandle_t input)
{
    if (input == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }
    // The agreed core-1 affinity applies to S3; C3 has only core 0.
#if CONFIG_FREERTOS_UNICORE
    const BaseType_t core = 0;
#else
    const BaseType_t core = 1;
#endif
    return xTaskCreatePinnedToCore(playback_task, "playback", 3072, input,
                                  tskIDLE_PRIORITY + 2, &s_task, core) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}
