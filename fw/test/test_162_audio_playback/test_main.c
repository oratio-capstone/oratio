#include "audio_playback.h"
#include "speech_fixture.h"

#include <stdbool.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "TEST_162";
static audio_frame_t s_frame;
static const audio_output_config_t CONFIG =
{
    .bclk_gpio = (gpio_num_t)ORATIO_AUDIO_TEST_BCLK_GPIO,
    .ws_gpio = (gpio_num_t)ORATIO_AUDIO_TEST_WS_GPIO,
    .dout_gpio = (gpio_num_t)ORATIO_AUDIO_TEST_DOUT_GPIO
};

enum
{
    DRAIN_TIMEOUT_MS = 3000,
    DMA_SETTLE_MS = 100,
    PRODUCER_PAUSE_MS = 300,
    BURST_FRAMES = 64,
    LIFECYCLE_CYCLES = 5
};

_Static_assert((uint32_t)SPEECH_FIXTURE_RATE_HZ == (uint32_t)AUDIO_SAMPLE_RATE_HZ,
               "Speech fixture sample rate mismatch");
_Static_assert(AUDIO_SAMPLES_PER_FRAME == 320, "Review frame pacing for the changed format");

static esp_err_t snapshot(audio_playback_stats_t *stats)
{
    const esp_err_t result = audio_playback_get_stats(stats);
    if (result != ESP_OK)
    {
        return result;
    }
    /* Per-attempt timeouts can recover through bounded suffix retries. A failed
     * frame is represented by write_errors/state, not the timeout counter alone. */
    if (stats->state != AUDIO_PLAYBACK_RUNNING || stats->write_errors != 0)
    {
        ESP_LOGE(TAG, "Unexpected state=%d errors=%lu timeouts=%lu last=%s", stats->state,
                 (unsigned long)stats->write_errors, (unsigned long)stats->write_timeouts,
                 esp_err_to_name(stats->last_error));
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t wait_for_audio(void)
{
    const TickType_t begin = xTaskGetTickCount();
    while (xTaskGetTickCount() - begin < pdMS_TO_TICKS(DRAIN_TIMEOUT_MS))
    {
        audio_playback_stats_t stats;
        const esp_err_t result = snapshot(&stats);
        if (result != ESP_OK)
        {
            return result;
        }
        if (stats.queue_depth == 0 && stats.output_frames == stats.submitted_frames - stats.dropped_frames)
        {
            /* Acceptance is not a hardware drain. Allow more than the driver's
             * nominal 20 ms DMA capacity while the worker continues silence. */
            vTaskDelay(pdMS_TO_TICKS(DMA_SETTLE_MS));
            return snapshot(&stats);
        }
        vTaskDelay(1);
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t feed_speech(bool pause)
{
    const size_t frames = (SPEECH_FIXTURE_SAMPLES + AUDIO_SAMPLES_PER_FRAME - 1) / AUDIO_SAMPLES_PER_FRAME;
    TickType_t wake = xTaskGetTickCount();
    for (size_t frame = 0; frame < frames; frame++)
    {
        if (pause && frame == frames / 2)
        {
            audio_playback_stats_t before;
            audio_playback_stats_t after;
            (void)audio_playback_get_stats(&before);
            ESP_LOGI(TAG, "Producer pause: expect a brief silent gap, then resumed speech");
            vTaskDelay(pdMS_TO_TICKS(PRODUCER_PAUSE_MS));
            const esp_err_t result = snapshot(&after);
            if (result != ESP_OK || after.silence_frames <= before.silence_frames || after.queue_depth != 0)
            {
                return ESP_FAIL;
            }
            /* Resume at the current time; do not burst to catch up after the intentional gap. */
            wake = xTaskGetTickCount();
        }
        const size_t offset = frame * AUDIO_SAMPLES_PER_FRAME;
        const size_t remaining = SPEECH_FIXTURE_SAMPLES - offset;
        const size_t count = remaining < AUDIO_SAMPLES_PER_FRAME ? remaining : AUDIO_SAMPLES_PER_FRAME;
        memset(&s_frame, 0, sizeof(s_frame));
        memcpy(s_frame.samples, speech_fixture_samples + offset, count * sizeof(int16_t));
        const esp_err_t result = audio_playback_submit(s_frame.samples, AUDIO_SAMPLES_PER_FRAME);
        if (result != ESP_OK)
        {
            return result;
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(AUDIO_FRAME_DURATION_MS));
    }
    return wait_for_audio();
}

static esp_err_t speech_stage(const char *label, uint8_t volume, bool pause)
{
    ESP_LOGI(TAG, "%s: volume=%u%%; LISTENING RESULT PENDING", label, volume);
    esp_err_t result = audio_playback_set_volume(volume);
    audio_playback_stats_t before;
    audio_playback_stats_t after;
    if (result == ESP_OK)
    {
        result = snapshot(&before);
    }
    if (result != ESP_OK)
    {
        return result;
    }
    result = feed_speech(pause);
    if (result != ESP_OK)
    {
        return result;
    }
    result = snapshot(&after);
    if (result != ESP_OK || after.output_frames <= before.output_frames ||
        after.dropped_frames != before.dropped_frames || after.volume_percent != volume)
    {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s: software PASS; accepted frames=%lu write-timeout attempts=%lu", label,
             (unsigned long)(after.output_frames - before.output_frames),
             (unsigned long)(after.write_timeouts - before.write_timeouts));
    return ESP_OK;
}

static esp_err_t submit_burst(void)
{
    /* Non-silent section of the fixture; this stage may sound choppy by design. */
    memcpy(s_frame.samples, speech_fixture_samples + 4320, sizeof(s_frame.samples));
    for (size_t frame = 0; frame < BURST_FRAMES; frame++)
    {
        const esp_err_t result = audio_playback_submit(s_frame.samples, AUDIO_SAMPLES_PER_FRAME);
        if (result != ESP_OK)
        {
            return result;
        }
    }
    return ESP_OK;
}

static esp_err_t overflow_stage(void)
{
    ESP_LOGI(TAG, "OVERFLOW: 64 frames without producer pacing; discontinuities expected");
    audio_playback_stats_t before;
    audio_playback_stats_t after;
    esp_err_t result = snapshot(&before);
    if (result == ESP_OK)
    {
        result = submit_burst();
    }
    if (result == ESP_OK)
    {
        result = wait_for_audio();
    }
    if (result != ESP_OK)
    {
        return result;
    }
    result = snapshot(&after);
    if (result != ESP_OK || after.dropped_frames <= before.dropped_frames ||
        after.queue_high_water > AUDIO_PLAYBACK_QUEUE_FRAMES || after.queue_depth != 0)
    {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OVERFLOW: software PASS; dropped=%lu high-water=%u",
             (unsigned long)(after.dropped_frames - before.dropped_frames), (unsigned int)after.queue_high_water);
    return ESP_OK;
}

static esp_err_t restart_stage(void)
{
    ESP_LOGI(TAG, "STOP/RESTART: interrupt a burst, then expect silence before fresh speech");
    esp_err_t result = submit_burst();
    if (result == ESP_OK)
    {
        result = audio_playback_stop();
    }
    if (result != ESP_OK)
    {
        return result;
    }
    audio_playback_stats_t before;
    audio_playback_stats_t after;
    (void)audio_playback_get_stats(&before);
    if (before.state != AUDIO_PLAYBACK_STOPPED || before.queue_depth != 0 ||
        audio_playback_submit(s_frame.samples, AUDIO_SAMPLES_PER_FRAME) != ESP_ERR_INVALID_STATE)
    {
        return ESP_FAIL;
    }
    result = audio_playback_start();
    if (result != ESP_OK)
    {
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(PRODUCER_PAUSE_MS));
    result = snapshot(&after);
    if (result != ESP_OK || after.output_frames != before.output_frames ||
        after.silence_frames <= before.silence_frames || after.queue_depth != 0)
    {
        return ESP_FAIL;
    }
    /* Stop can discard an in-flight frame, which is intentionally not counted as
     * a queued drop. Reinitialize counters before using acceptance-based waits. */
    result = audio_playback_deinit();
    if (result == ESP_OK)
    {
        result = audio_playback_init(&CONFIG);
    }
    if (result == ESP_OK)
    {
        result = audio_playback_start();
    }
    return result == ESP_OK ? speech_stage("FRESH SPEECH", 100, false) : result;
}

static esp_err_t lifecycle_stage(void)
{
    ESP_LOGI(TAG, "LIFECYCLE: five complete init/start/speech/stop/deinit cycles");
    for (size_t cycle = 0; cycle < LIFECYCLE_CYCLES; cycle++)
    {
        esp_err_t result = audio_playback_init(&CONFIG);
        if (result == ESP_OK)
        {
            result = audio_playback_start();
        }
        if (result == ESP_OK)
        {
            result = feed_speech(false);
        }
        const esp_err_t cleanup = audio_playback_deinit();
        if (result != ESP_OK || cleanup != ESP_OK)
        {
            return result != ESP_OK ? result : cleanup;
        }
        /* Deleted-task allocations can be reclaimed by the idle task asynchronously. */
        vTaskDelay(pdMS_TO_TICKS(100));
        audio_playback_stats_t stats;
        (void)audio_playback_get_stats(&stats);
        if (stats.state != AUDIO_PLAYBACK_UNINITIALIZED || stats.queue_depth != 0 ||
            !heap_caps_check_integrity_all(true))
        {
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Cycle=%u PASS; free internal heap=%u largest=%u; inspect trend for leaks",
                 (unsigned int)(cycle + 1), (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
    return ESP_OK;
}

static esp_err_t run_sequence(void)
{
    esp_err_t result = audio_playback_init(&CONFIG);
    if (result == ESP_OK)
    {
        result = audio_playback_start();
    }
    if (result == ESP_OK)
    {
        result = speech_stage("NORMAL SPEECH", 100, false);
    }
    if (result == ESP_OK)
    {
        result = speech_stage("UNDERFLOW/RECOVERY", 100, true);
    }
    if (result == ESP_OK)
    {
        result = speech_stage("QUIETER SPEECH", 25, false);
    }
    if (result == ESP_OK)
    {
        result = speech_stage("MUTE", 0, false);
    }
    if (result == ESP_OK)
    {
        result = audio_playback_set_volume(100);
    }
    if (result == ESP_OK)
    {
        result = overflow_stage();
    }
    if (result == ESP_OK)
    {
        result = restart_stage();
    }
    const esp_err_t cleanup = audio_playback_deinit();
    if (result != ESP_OK || cleanup != ESP_OK)
    {
        return result != ESP_OK ? result : cleanup;
    }
    return lifecycle_stage();
}

void app_main(void)
{
    ESP_LOGW(TAG, "Task 162: BCLK=%d WS=%d DOUT=%d; native USB console; test starts in 10 seconds",
             CONFIG.bclk_gpio, CONFIG.ws_gpio, CONFIG.dout_gpio);
    speech_fixture_log_info();
    for (int seconds = 10; seconds > 0; seconds--)
    {
        ESP_LOGI(TAG, "Speech test starts in %d seconds; expect one, two, three", seconds);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    const esp_err_t result = run_sequence();
    const esp_err_t cleanup = audio_playback_deinit();
    ESP_LOGI(TAG, "SOFTWARE SEQUENCE %s; result=%s cleanup=%s; listening and heap trend remain manual",
             result == ESP_OK && cleanup == ESP_OK ? "PASS" : "FAIL",
             esp_err_to_name(result), esp_err_to_name(cleanup));
    /* Late USB monitors still receive a result. Reset starts a fresh sequence. */
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "Test finished: %s; result=%s cleanup=%s; reset to repeat",
                 result == ESP_OK && cleanup == ESP_OK ? "PASS" : "FAIL",
                 esp_err_to_name(result), esp_err_to_name(cleanup));
    }
}
