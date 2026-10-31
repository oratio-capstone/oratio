/*
 * AB#157 audio capture validation app.
 *
 * Plays the role of the network task: creates the message buffer, starts the
 * capture library, consumes audio_capture_frame_t messages and measures them.
 * Prints a 1 s rolling summary, then a PASS/FAIL line for every check.
 *
 * Build/flash:  pio run -e audio_capture_test -t upload -t monitor
 * WAV capture:  pio run -e audio_capture_record -t upload, then tools/record_capture_wav.py
 */
#include "audio_capture.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/task.h"

#if !defined(ORATIO_CAPTURE_TEST_BCLK_GPIO) || !defined(ORATIO_CAPTURE_TEST_WS_GPIO) ||                                \
    !defined(ORATIO_CAPTURE_TEST_DIN_GPIO)
#error "Select the microphone GPIOs in platformio.ini before building"
#endif
#ifndef ORATIO_CAPTURE_TEST_LED_GPIO
#define ORATIO_CAPTURE_TEST_LED_GPIO -1
#endif
#ifndef ORATIO_CAPTURE_TEST_GAIN_SHIFT
#define ORATIO_CAPTURE_TEST_GAIN_SHIFT 0
#endif
#ifndef ORATIO_CAPTURE_TEST_RECORD
#define ORATIO_CAPTURE_TEST_RECORD 0
#endif
#ifndef ORATIO_CAPTURE_TEST_SECONDS
#define ORATIO_CAPTURE_TEST_SECONDS 10
#endif

// Recording logs every frame, so it gets a deeper buffer to absorb console delays.
#define CAPTURE_BUFFER_FRAMES (ORATIO_CAPTURE_TEST_RECORD ? 16 : 4)
#define BASE64_LINE_BYTES (4 * ((AUDIO_FRAME_BYTES + 2) / 3) + 1)

static const char *TAG = "TEST_157";

static const uint32_t TEST_DURATION_MS = ORATIO_CAPTURE_TEST_SECONDS * 1000;
static const uint32_t RESTART_DURATION_MS = 1000;
static const uint32_t RECEIVE_TIMEOUT_MS = 200;
static const int64_t WINDOW_US = 1000000;
static const uint32_t FRAMES_PER_SECOND = 1000 / AUDIO_FRAME_DURATION_MS; // 50
static const float MIN_FRAME_RATE = 49.0f;
static const float MAX_FRAME_RATE = 51.0f;
static const int32_t MIN_SIGNAL_P2P = 20; // below this the microphone is silent or disconnected
static const float FULL_SCALE = 32768.0f;

typedef struct
{
    uint32_t frames;
    uint32_t malformed;
    uint32_t short_frames;
    uint32_t missing; // sequence numbers skipped
    uint32_t out_of_order;
    bool has_sequence;
    uint16_t expected_sequence;
    uint16_t first_sequence;
    int64_t first_frame_us;
    int64_t last_frame_us;
    int16_t min_sample;
    int16_t max_sample;
} run_stats_t;

typedef struct
{
    uint32_t frames;
    int16_t min_sample;
    int16_t max_sample;
    double sum;
    double sum_squares;
    uint32_t samples;
} window_stats_t;

static audio_capture_frame_t s_frame;
static int16_t s_samples[AUDIO_SAMPLES_PER_FRAME];
static char s_base64[BASE64_LINE_BYTES];

static void reset_run(run_stats_t *run)
{
    memset(run, 0, sizeof(*run));
    run->min_sample = INT16_MAX;
    run->max_sample = INT16_MIN;
}

static void reset_window(window_stats_t *window)
{
    memset(window, 0, sizeof(*window));
    window->min_sample = INT16_MAX;
    window->max_sample = INT16_MIN;
}

static void base64_encode(const uint8_t *src, size_t length, char *dest)
{
    static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out = 0;
    for (size_t i = 0; i < length; i += 3)
    {
        const uint32_t b0 = src[i];
        const uint32_t b1 = (i + 1 < length) ? src[i + 1] : 0;
        const uint32_t b2 = (i + 2 < length) ? src[i + 2] : 0;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        dest[out++] = ALPHABET[(triple >> 18) & 0x3F];
        dest[out++] = ALPHABET[(triple >> 12) & 0x3F];
        dest[out++] = (i + 1 < length) ? ALPHABET[(triple >> 6) & 0x3F] : '=';
        dest[out++] = (i + 2 < length) ? ALPHABET[triple & 0x3F] : '=';
    }
    dest[out] = '\0';
}

static void track_sequence(run_stats_t *run, uint16_t sequence)
{
    if (!run->has_sequence)
    {
        run->has_sequence = true;
        run->first_sequence = sequence;
    }
    else if (sequence != run->expected_sequence)
    {
        const uint16_t distance = (uint16_t)(sequence - run->expected_sequence);
        if (distance < 0x8000u)
        {
            run->missing += distance;
        }
        else
        {
            run->out_of_order++;
        }
    }
    run->expected_sequence = (uint16_t)(sequence + 1);
}

static void accumulate_level(run_stats_t *run, window_stats_t *window, size_t sample_count)
{
    for (size_t i = 0; i < sample_count; i++)
    {
        const int16_t sample = s_samples[i];
        window->min_sample = (sample < window->min_sample) ? sample : window->min_sample;
        window->max_sample = (sample > window->max_sample) ? sample : window->max_sample;
        window->sum += sample;
        window->sum_squares += (double)sample * sample;
    }
    window->samples += (uint32_t)sample_count;
    run->min_sample = (window->min_sample < run->min_sample) ? window->min_sample : run->min_sample;
    run->max_sample = (window->max_sample > run->max_sample) ? window->max_sample : run->max_sample;
}

// Validate one message exactly as the network task would, then measure it.
static void process_frame(size_t received_bytes, run_stats_t *run, window_stats_t *window, bool export_pcm)
{
    const size_t length = s_frame.length;
    if (received_bytes < AUDIO_CAPTURE_FRAME_HEADER_BYTES || length > AUDIO_FRAME_BYTES || (length % 2) != 0 ||
        received_bytes != AUDIO_CAPTURE_FRAME_HEADER_BYTES + length)
    {
        run->malformed++;
        return;
    }
    if (length != AUDIO_FRAME_BYTES)
    {
        run->short_frames++;
    }

    const int64_t now_us = esp_timer_get_time();
    if (run->frames == 0)
    {
        run->first_frame_us = now_us;
    }
    run->last_frame_us = now_us;
    run->frames++;
    window->frames++;
    track_sequence(run, s_frame.sequence);

    const size_t sample_count = length / sizeof(int16_t);
    audio_capture_decode_pcm(s_frame.payload, sample_count, s_samples);
    accumulate_level(run, window, sample_count);

    if (export_pcm)
    {
        base64_encode(s_frame.payload, length, s_base64);
        ESP_LOGI(TAG, "PCM %u %s", (unsigned)s_frame.sequence, s_base64); // test-only data export
    }
}

static void log_window(uint32_t second, const run_stats_t *run, const window_stats_t *window)
{
    if (window->samples == 0)
    {
        ESP_LOGW(TAG, "t=%2lus no frames received", (unsigned long)second);
        return;
    }
    const double mean = window->sum / window->samples;
    const double rms = sqrt(window->sum_squares / window->samples);
    const double rms_dbfs = (rms > 0.0) ? 20.0 * log10(rms / FULL_SCALE) : -120.0;
    ESP_LOGI(TAG, "t=%2lus frames=%2lu missing=%lu p2p=%5d rms=%6.1f dBFS dc=%6.0f", (unsigned long)second,
             (unsigned long)window->frames, (unsigned long)run->missing, (int)(window->max_sample - window->min_sample),
             rms_dbfs, mean);
}

// Consume frames for duration_ms, logging a summary every second.
static void run_phase(MessageBufferHandle_t buffer, uint32_t duration_ms, run_stats_t *run, bool export_pcm)
{
    window_stats_t window;
    reset_window(&window);
    const int64_t start_us = esp_timer_get_time();
    int64_t window_start_us = start_us;
    uint32_t second = 0;

    while (esp_timer_get_time() - start_us < (int64_t)duration_ms * 1000)
    {
        const size_t received =
            xMessageBufferReceive(buffer, &s_frame, sizeof(s_frame), pdMS_TO_TICKS(RECEIVE_TIMEOUT_MS));
        if (received > 0)
        {
            process_frame(received, run, &window, export_pcm);
        }

        if (esp_timer_get_time() - window_start_us >= WINDOW_US)
        {
            second++;
            log_window(second, run, &window);
            reset_window(&window);
            window_start_us += WINDOW_US;
        }
    }
}

static bool check(bool passed, const char *name, const char *detail)
{
    ESP_LOGI(TAG, "[%s] %s: %s", passed ? "PASS" : "FAIL", name, detail);
    return passed;
}

static bool evaluate(const run_stats_t *main_run, const run_stats_t *restart_run, const audio_capture_stats_t *phase,
                     const audio_capture_stats_t *final_stats)
{
    char detail[96];
    bool ok = true;

    const uint32_t expected_frames = FRAMES_PER_SECOND * ORATIO_CAPTURE_TEST_SECONDS;
    snprintf(detail, sizeof(detail), "%lu of ~%lu frames", (unsigned long)main_run->frames,
             (unsigned long)expected_frames);
    ok &= check(main_run->frames >= expected_frames * 98 / 100, "frames received", detail);

    const double elapsed_s = (double)(main_run->last_frame_us - main_run->first_frame_us) / 1e6;
    const double rate = (elapsed_s > 0.0) ? (main_run->frames - 1) / elapsed_s : 0.0;
    snprintf(detail, sizeof(detail), "%.2f frames/s (expect 50)", rate);
    ok &= check(rate >= MIN_FRAME_RATE && rate <= MAX_FRAME_RATE, "real-time rate", detail);

    snprintf(detail, sizeof(detail), "missing=%lu out_of_order=%lu", (unsigned long)main_run->missing,
             (unsigned long)main_run->out_of_order);
    ok &= check(main_run->missing == 0 && main_run->out_of_order == 0, "sequence continuity", detail);

    snprintf(detail, sizeof(detail), "malformed=%lu short=%lu (all frames 640 bytes)",
             (unsigned long)main_run->malformed, (unsigned long)main_run->short_frames);
    ok &= check(main_run->malformed == 0 && main_run->short_frames == 0, "frame format", detail);

    snprintf(detail, sizeof(detail), "dropped=%lu timeouts=%lu errors=%lu dma_overflows=%lu",
             (unsigned long)phase->frames_dropped, (unsigned long)phase->read_timeouts,
             (unsigned long)phase->read_errors, (unsigned long)phase->dma_overflows);
    ok &= check(phase->frames_dropped == 0 && phase->read_timeouts == 0 && phase->read_errors == 0 &&
                    phase->dma_overflows == 0,
                "capture health", detail);

    const int32_t p2p = (int32_t)main_run->max_sample - main_run->min_sample;
    snprintf(detail, sizeof(detail), "peak-to-peak %ld (min %d, max %d)", (long)p2p, main_run->min_sample,
             main_run->max_sample);
    ok &= check(p2p >= MIN_SIGNAL_P2P, "microphone signal", detail);

    snprintf(detail, sizeof(detail), "%lu frames after restart, first sequence %u (expected %u)",
             (unsigned long)restart_run->frames, (unsigned)restart_run->first_sequence,
             (unsigned)main_run->expected_sequence);
    ok &= check(restart_run->frames >= FRAMES_PER_SECOND * 9 / 10 &&
                    restart_run->first_sequence == main_run->expected_sequence,
                "stop/start", detail);

    snprintf(detail, sizeof(detail), "%u bytes free at peak", (unsigned)final_stats->stack_free_bytes);
    ok &= check(final_stats->stack_free_bytes >= 512, "task stack", detail);
    return ok;
}

static void finish(bool passed, const char *reason)
{
    // Repeat the verdict so a monitor opened late still sees it. Reset to run again.
    while (true)
    {
        if (passed)
        {
            ESP_LOGI(TAG, "RESULT: PASS");
        }
        else
        {
            ESP_LOGE(TAG, "RESULT: FAIL (%s)", reason);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    const audio_capture_config_t config = {
        .bclk_gpio = (gpio_num_t)ORATIO_CAPTURE_TEST_BCLK_GPIO,
        .ws_gpio = (gpio_num_t)ORATIO_CAPTURE_TEST_WS_GPIO,
        .din_gpio = (gpio_num_t)ORATIO_CAPTURE_TEST_DIN_GPIO,
        .status_led_gpio = (gpio_num_t)ORATIO_CAPTURE_TEST_LED_GPIO,
        .gain_shift = ORATIO_CAPTURE_TEST_GAIN_SHIFT,
    };

    vTaskDelay(pdMS_TO_TICKS(2000)); // give the USB monitor time to reconnect after reset
    ESP_LOGI(TAG, "Audio capture test: %d s, buffer %d frames, record=%d", ORATIO_CAPTURE_TEST_SECONDS,
             CAPTURE_BUFFER_FRAMES, ORATIO_CAPTURE_TEST_RECORD);

    MessageBufferHandle_t buffer = xMessageBufferCreate(AUDIO_CAPTURE_MESSAGE_BUFFER_BYTES(CAPTURE_BUFFER_FRAMES));
    if (buffer == NULL)
    {
        finish(false, "message buffer allocation");
    }

    esp_err_t result = audio_capture_init(&config);
    if (result == ESP_OK)
    {
        result = audio_capture_task_start(buffer);
    }
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Capture start failed: %s", esp_err_to_name(result));
        audio_capture_deinit();
        finish(false, "capture start");
    }

    run_stats_t main_run;
    reset_run(&main_run);
    if (ORATIO_CAPTURE_TEST_RECORD)
    {
        ESP_LOGI(TAG, "PCM_BEGIN rate=%d bits=%d channels=%d", AUDIO_SAMPLE_RATE_HZ, AUDIO_SAMPLE_BITS, AUDIO_CHANNELS);
    }
    ESP_LOGI(TAG, "Speak, clap or tap near the microphone during the test");
    run_phase(buffer, TEST_DURATION_MS, &main_run, ORATIO_CAPTURE_TEST_RECORD);
    if (ORATIO_CAPTURE_TEST_RECORD)
    {
        ESP_LOGI(TAG, "PCM_END frames=%lu", (unsigned long)main_run.frames);
    }

    // Health counters cover the measurement window only, not the moments after the consumer stopped reading.
    audio_capture_stats_t phase_stats;
    audio_capture_get_stats(&phase_stats);
    result = audio_capture_task_stop();
    xMessageBufferReset(buffer); // discard anything queued after the measurement window

    // Restart: frames must flow again and the sequence must continue where it stopped.
    run_stats_t restart_run;
    reset_run(&restart_run);
    audio_capture_stats_t before_restart;
    audio_capture_get_stats(&before_restart);
    main_run.expected_sequence = before_restart.next_sequence;
    if (result == ESP_OK)
    {
        result = audio_capture_task_start(buffer);
    }
    if (result == ESP_OK)
    {
        run_phase(buffer, RESTART_DURATION_MS, &restart_run, false);
        result = audio_capture_task_stop();
    }

    audio_capture_stats_t stats;
    audio_capture_get_stats(&stats);
    const bool passed = (result == ESP_OK) && evaluate(&main_run, &restart_run, &phase_stats, &stats);
    audio_capture_deinit();
    vMessageBufferDelete(buffer);
    finish(passed, (result == ESP_OK) ? "see [FAIL] lines above" : esp_err_to_name(result));
}
