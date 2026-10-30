#include "audio_playback.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "AUDIO_PLAYBACK";

enum
{
    PLAYBACK_STACK_BYTES = 4096,
    WRITE_SLICE_MS = 20,
    FRAME_WRITE_BUDGET_US = 100000,
    MAX_WRITE_ATTEMPTS = 16
};

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_changed;
static TaskHandle_t s_task;
static audio_output_config_t s_config;
static audio_playback_stats_t s_stats;
static audio_frame_t s_frame;
static audio_frame_t s_discarded_frame;
static bool s_idle;
static bool s_stop_requested;
static bool s_start_command;
static esp_err_t s_cleanup_error;

static bool stop_requested(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool stop = s_stop_requested;
    portEXIT_CRITICAL(&s_lock);
    return stop;
}

static esp_err_t write_frame(void)
{
    size_t offset = 0;
    const int64_t deadline = esp_timer_get_time() + FRAME_WRITE_BUDGET_US;
    for (size_t attempt = 0; attempt < MAX_WRITE_ATTEMPTS; attempt++)
    {
        if (stop_requested())
        {
            return ESP_ERR_INVALID_STATE;
        }
        const int64_t remaining_us = deadline - esp_timer_get_time();
        if (remaining_us < 1000)
        {
            return ESP_ERR_TIMEOUT;
        }
        const uint32_t remaining_ms = (uint32_t)(remaining_us / 1000);
        const uint32_t timeout_ms = remaining_ms < WRITE_SLICE_MS ? remaining_ms : WRITE_SLICE_MS;
        size_t written = 0;
        const esp_err_t result = audio_output_write(s_frame.samples + offset,
                                                    AUDIO_SAMPLES_PER_FRAME - offset, &written, timeout_ms);
        if (written > AUDIO_SAMPLES_PER_FRAME - offset || (result == ESP_OK && written == 0))
        {
            return ESP_FAIL;
        }
        offset += written;
        if (result != ESP_OK && result != ESP_ERR_TIMEOUT)
        {
            return result;
        }
        if (result == ESP_ERR_TIMEOUT)
        {
            portENTER_CRITICAL(&s_lock);
            s_stats.write_timeouts++;
            portEXIT_CRITICAL(&s_lock);
        }
        if (offset == AUDIO_SAMPLES_PER_FRAME)
        {
            return ESP_OK;
        }
        /* A zero-progress timeout must yield, not spin at playback priority. */
        if (written == 0)
        {
            vTaskDelay(1);
        }
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t run_frames(void)
{
    int64_t last_summary = esp_timer_get_time();
    while (!stop_requested())
    {
        portENTER_CRITICAL(&s_lock);
        const bool have_frame = xQueueReceive(s_queue, &s_frame, 0) == pdTRUE;
        const uint8_t volume = s_stats.volume_percent;
        if (!have_frame)
        {
            s_stats.underflows++;
        }
        portEXIT_CRITICAL(&s_lock);

        if (!have_frame)
        {
            memset(&s_frame, 0, sizeof(s_frame));
        }
        else
        {
            for (size_t index = 0; index < AUDIO_SAMPLES_PER_FRAME; index++)
            {
                s_frame.samples[index] = (int16_t)((int32_t)s_frame.samples[index] * volume / 100);
            }
        }

        const esp_err_t result = write_frame();
        if (stop_requested())
        {
            return ESP_OK;
        }
        if (result != ESP_OK)
        {
            portENTER_CRITICAL(&s_lock);
            s_stats.write_errors++;
            portEXIT_CRITICAL(&s_lock);
            return result;
        }
        portENTER_CRITICAL(&s_lock);
        if (have_frame)
        {
            s_stats.output_frames++;
        }
        else
        {
            s_stats.silence_frames++;
        }
        portEXIT_CRITICAL(&s_lock);

        if (esp_timer_get_time() - last_summary >= 2000000)
        {
            audio_playback_stats_t snapshot;
            (void)audio_playback_get_stats(&snapshot);
            ESP_LOGI(TAG, "Output=%lu silence=%lu dropped=%lu queued=%u",
                     (unsigned long)snapshot.output_frames, (unsigned long)snapshot.silence_frames,
                     (unsigned long)snapshot.dropped_frames, (unsigned int)snapshot.queue_depth);
            last_summary = esp_timer_get_time();
        }
    }
    return ESP_OK;
}

static void playback_task(void *argument)
{
    (void)argument;
    while (true)
    {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        portENTER_CRITICAL(&s_lock);
        const bool start = s_start_command;
        portEXIT_CRITICAL(&s_lock);

        esp_err_t result = ESP_OK;
        if (start)
        {
            result = audio_output_init(&s_config);
            if (result == ESP_OK)
            {
                result = audio_output_start();
            }
            portENTER_CRITICAL(&s_lock);
            if (result == ESP_OK && !s_stop_requested)
            {
                s_stats.state = AUDIO_PLAYBACK_RUNNING;
            }
            portEXIT_CRITICAL(&s_lock);
            (void)xSemaphoreGive(s_changed);
            if (result == ESP_OK)
            {
                result = run_frames();
            }
        }

        /* Only this worker touches the output, even when cleanup must be retried. */
        const esp_err_t cleanup = audio_output_deinit();
        if (result != ESP_OK || cleanup != ESP_OK)
        {
            ESP_LOGE(TAG, "Worker stopped: %s; cleanup: %s", esp_err_to_name(result), esp_err_to_name(cleanup));
        }
        portENTER_CRITICAL(&s_lock);
        s_cleanup_error = cleanup;
        s_stats.dropped_frames += (uint32_t)uxQueueMessagesWaiting(s_queue);
        (void)xQueueReset(s_queue);
        s_stats.last_error = cleanup != ESP_OK ? cleanup : result;
        s_stats.state = cleanup == ESP_OK && (s_stop_requested || !start)
                            ? AUDIO_PLAYBACK_STOPPED : AUDIO_PLAYBACK_FAULTED;
        /* Publish idle and signal together: deinit must not delete the semaphore
         * before this final give. After unlocking, the worker only waits again. */
        s_idle = true;
        (void)xSemaphoreGive(s_changed);
        portEXIT_CRITICAL(&s_lock);
    }
}

static esp_err_t await_state(bool starting)
{
    const TickType_t begin = xTaskGetTickCount();
    const TickType_t budget = pdMS_TO_TICKS(AUDIO_PLAYBACK_CONTROL_TIMEOUT_MS);
    while (true)
    {
        portENTER_CRITICAL(&s_lock);
        const bool idle = s_idle;
        const audio_playback_state_t state = s_stats.state;
        const esp_err_t error = s_stats.last_error;
        const esp_err_t cleanup = s_cleanup_error;
        portEXIT_CRITICAL(&s_lock);
        if (starting && state == AUDIO_PLAYBACK_RUNNING)
        {
            return ESP_OK;
        }
        if (idle)
        {
            return starting ? (error == ESP_OK ? ESP_FAIL : error) : cleanup;
        }
        const TickType_t elapsed = xTaskGetTickCount() - begin;
        if (elapsed >= budget)
        {
            portENTER_CRITICAL(&s_lock);
            s_stop_requested = true;
            s_stats.state = AUDIO_PLAYBACK_STOPPING;
            portEXIT_CRITICAL(&s_lock);
            return ESP_ERR_TIMEOUT;
        }
        (void)xSemaphoreTake(s_changed, budget - elapsed);
    }
}

esp_err_t audio_playback_init(const audio_output_config_t *config)
{
    if (config == NULL || !GPIO_IS_VALID_OUTPUT_GPIO(config->bclk_gpio) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config->ws_gpio) || !GPIO_IS_VALID_OUTPUT_GPIO(config->dout_gpio) ||
        config->bclk_gpio == config->ws_gpio || config->bclk_gpio == config->dout_gpio ||
        config->ws_gpio == config->dout_gpio)
    {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    const bool initialized = s_stats.state != AUDIO_PLAYBACK_UNINITIALIZED;
    portEXIT_CRITICAL(&s_lock);
    if (initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    QueueHandle_t queue = xQueueCreate(AUDIO_PLAYBACK_QUEUE_FRAMES, sizeof(audio_frame_t));
    SemaphoreHandle_t changed = xSemaphoreCreateBinary();
    if (queue == NULL || changed == NULL)
    {
        if (queue != NULL)
        {
            vQueueDelete(queue);
        }
        if (changed != NULL)
        {
            vSemaphoreDelete(changed);
        }
        return ESP_ERR_NO_MEM;
    }
    /* No commands are sent until task creation has succeeded. Public access is
     * still rejected while state remains UNINITIALIZED. */
    s_queue = queue;
    s_changed = changed;
    s_config = *config;
    if (xTaskCreate(playback_task, "audio_playback", PLAYBACK_STACK_BYTES, NULL,
                    tskIDLE_PRIORITY + 2, &s_task) != pdPASS)
    {
        vQueueDelete(queue);
        vSemaphoreDelete(changed);
        s_queue = NULL;
        s_changed = NULL;
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_lock);
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.volume_percent = 100;
    s_stats.state = AUDIO_PLAYBACK_STOPPED;
    s_idle = true;
    s_cleanup_error = ESP_OK;
    s_stop_requested = false;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t audio_playback_start(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_stats.state != AUDIO_PLAYBACK_STOPPED || !s_idle)
    {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_stats.state = AUDIO_PLAYBACK_STARTING;
    s_stats.last_error = ESP_OK;
    s_stop_requested = false;
    s_start_command = true;
    s_idle = false;
    portEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(s_task);
    return await_state(true);
}

esp_err_t audio_playback_submit(const int16_t *samples, size_t sample_count)
{
    if (samples == NULL || sample_count != AUDIO_SAMPLES_PER_FRAME)
    {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    if (s_stats.state != AUDIO_PLAYBACK_RUNNING)
    {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (uxQueueMessagesWaiting(s_queue) == AUDIO_PLAYBACK_QUEUE_FRAMES)
    {
        (void)xQueueReceive(s_queue, &s_discarded_frame, 0);
        s_stats.dropped_frames++;
    }
    const BaseType_t result = xQueueSend(s_queue, samples, 0);
    if (result == pdTRUE)
    {
        s_stats.submitted_frames++;
        const size_t depth = (size_t)uxQueueMessagesWaiting(s_queue);
        if (depth > s_stats.queue_high_water)
        {
            s_stats.queue_high_water = depth;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return result == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_playback_set_volume(uint8_t percent)
{
    if (percent > 100)
    {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    if (s_stats.state == AUDIO_PLAYBACK_UNINITIALIZED)
    {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_stats.volume_percent = percent;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t audio_playback_get_stats(audio_playback_stats_t *stats)
{
    if (stats == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    *stats = s_stats;
    stats->queue_depth = s_stats.state == AUDIO_PLAYBACK_UNINITIALIZED
                             ? 0 : (size_t)uxQueueMessagesWaiting(s_queue);
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t audio_playback_stop(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_stats.state == AUDIO_PLAYBACK_UNINITIALIZED)
    {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_stop_requested = true;
    s_stats.state = AUDIO_PLAYBACK_STOPPING;
    const bool retry_cleanup = s_idle && s_cleanup_error != ESP_OK;
    if (s_idle && !retry_cleanup)
    {
        s_stats.state = AUDIO_PLAYBACK_STOPPED;
    }
    if (retry_cleanup)
    {
        s_idle = false;
        s_start_command = false;
    }
    portEXIT_CRITICAL(&s_lock);
    if (retry_cleanup)
    {
        xTaskNotifyGive(s_task);
    }
    return await_state(false);
}

esp_err_t audio_playback_deinit(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool initialized = s_stats.state != AUDIO_PLAYBACK_UNINITIALIZED;
    portEXIT_CRITICAL(&s_lock);
    if (!initialized)
    {
        return ESP_OK;
    }
    const esp_err_t result = audio_playback_stop();
    if (result != ESP_OK)
    {
        return result;
    }
    portENTER_CRITICAL(&s_lock);
    s_stats.state = AUDIO_PLAYBACK_UNINITIALIZED;
    portEXIT_CRITICAL(&s_lock);
    /* Worker acknowledged cleanup and cannot touch queue/output again without a
     * supervisor notification. No supervisor may overlap this lifecycle call. */
    vTaskDelete(s_task);
    vQueueDelete(s_queue);
    vSemaphoreDelete(s_changed);
    s_task = NULL;
    s_queue = NULL;
    s_changed = NULL;
    return ESP_OK;
}
