#include "audio_capture.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "AUDIO_CAPTURE";

static const uint32_t TASK_READ_TIMEOUT_MS = 100; // five frames of slack before a read counts as a timeout
static const uint32_t READ_ERROR_BACKOFF_MS = 20; // avoid spinning if the driver keeps failing
static const uint32_t SUMMARY_INTERVAL_MS = 2000; // style guide: rolling summary every 1-2 s
static const uint32_t STOP_POLL_MS = 10;
static const int STOP_POLL_ATTEMPTS = 30; // 300 ms > one task read timeout
static const uint32_t TASK_STACK_BYTES = 4096;
static const UBaseType_t TASK_PRIORITY = 5; // above the network task so capture never falls behind

enum
{
    DMA_DESC_NUM = 4 // 4 x 20 ms of hardware buffering
};

/* ---- Driver state ---- */

// INMP441 sends 24-bit samples left-justified in 32-bit I2S slots.
static int32_t s_raw_slots[AUDIO_SAMPLES_PER_FRAME];

static i2s_chan_handle_t s_rx_channel = NULL;
static audio_capture_config_t s_config;
static bool s_is_initialized = false;
static bool s_is_running = false;
static volatile uint32_t s_dma_overflows = 0;

/* ---- Task state ---- */

static TaskHandle_t s_task_handle = NULL;
static MessageBufferHandle_t s_output_buffer = NULL;
static volatile bool s_task_should_run = false; // written by the controller, read by the task
static volatile bool s_task_active = false;     // cleared by the task just before it exits

static int16_t s_task_samples[AUDIO_SAMPLES_PER_FRAME];
static audio_capture_frame_t s_task_frame;
static esp_err_t s_last_read_result = ESP_OK;

static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_capture_stats_t s_stats;
static uint16_t s_next_sequence = 0;

/* ===========================================================================
 * Helpers
 * ======================================================================== */

static bool IRAM_ATTR on_receive_overflow(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx)
{
    (void)handle;
    (void)event;
    (void)user_ctx;
    s_dma_overflows++;
    return false;
}

static bool is_valid_config(const audio_capture_config_t *config)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(config->bclk_gpio) || !GPIO_IS_VALID_OUTPUT_GPIO(config->ws_gpio) ||
        !GPIO_IS_VALID_GPIO(config->din_gpio))
    {
        return false;
    }
    if (config->bclk_gpio == config->ws_gpio || config->bclk_gpio == config->din_gpio ||
        config->ws_gpio == config->din_gpio)
    {
        return false;
    }
    if (config->gain_shift > AUDIO_CAPTURE_MAX_GAIN_SHIFT)
    {
        return false;
    }
    if (config->status_led_gpio == GPIO_NUM_NC)
    {
        return true;
    }
    return GPIO_IS_VALID_OUTPUT_GPIO(config->status_led_gpio) && config->status_led_gpio != config->bclk_gpio &&
           config->status_led_gpio != config->ws_gpio && config->status_led_gpio != config->din_gpio;
}

// Keep the top 16 bits of the 32-bit slot, optionally boosted by gain_shift x 6 dB with saturation.
static int16_t slot_to_pcm16(int32_t slot, uint8_t gain_shift)
{
    const int32_t value = slot >> (16 - gain_shift);
    if (value > INT16_MAX)
    {
        return INT16_MAX;
    }
    if (value < INT16_MIN)
    {
        return INT16_MIN;
    }
    return (int16_t)value;
}

static esp_err_t configure_channel(const audio_capture_config_t *config)
{
    i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = config->bclk_gpio,
                .ws = config->ws_gpio,
                .dout = I2S_GPIO_UNUSED,
                .din = config->din_gpio,
                .invert_flags = {0},
            },
    };
    std_config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT; // INMP441 L/R pin tied to GND

    esp_err_t result = i2s_channel_init_std_mode(s_rx_channel, &std_config);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "I2S configuration failed: %s", esp_err_to_name(result));
        return result;
    }

    const i2s_event_callbacks_t callbacks = {.on_recv_q_ovf = on_receive_overflow};
    result = i2s_channel_register_event_callback(s_rx_channel, &callbacks, NULL);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "I2S callback registration failed: %s", esp_err_to_name(result));
    }
    return result;
}

static esp_err_t configure_status_led(gpio_num_t led_gpio)
{
    if (led_gpio == GPIO_NUM_NC)
    {
        return ESP_OK;
    }

    esp_err_t result = gpio_reset_pin(led_gpio);
    if (result == ESP_OK)
    {
        result = gpio_set_direction(led_gpio, GPIO_MODE_OUTPUT);
    }
    if (result == ESP_OK)
    {
        result = gpio_set_level(led_gpio, 0);
    }
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Status LED setup failed: %s", esp_err_to_name(result));
    }
    return result;
}

static void set_status_led(bool on)
{
    if (s_config.status_led_gpio != GPIO_NUM_NC)
    {
        gpio_set_level(s_config.status_led_gpio, on ? 1 : 0);
    }
}

// Count read failures; log only the first occurrence of each new error and the recovery.
static void record_read_result(esp_err_t result)
{
    if (result != s_last_read_result)
    {
        if (result == ESP_OK)
        {
            ESP_LOGI(TAG, "Reads recovered");
        }
        else
        {
            ESP_LOGW(TAG, "Read failed: %s (repeats are counted, not logged)", esp_err_to_name(result));
        }
        s_last_read_result = result;
    }

    if (result == ESP_OK)
    {
        return;
    }

    portENTER_CRITICAL(&s_stats_lock);
    if (result == ESP_ERR_TIMEOUT)
    {
        s_stats.read_timeouts++;
    }
    else
    {
        s_stats.read_errors++;
    }
    portEXIT_CRITICAL(&s_stats_lock);
}

// One iteration of the capture task: read 20 ms, fill the frame, hand it to the network side.
static void capture_one_frame(void)
{
    size_t samples_read = 0;
    const esp_err_t result = audio_capture_read_frame(s_task_samples, &samples_read, TASK_READ_TIMEOUT_MS);
    record_read_result(result);

    if (samples_read == 0)
    {
        if (result != ESP_OK && result != ESP_ERR_TIMEOUT)
        {
            vTaskDelay(pdMS_TO_TICKS(READ_ERROR_BACKOFF_MS));
        }
        return;
    }

    s_task_frame.sequence = s_next_sequence;
    s_task_frame.length = (uint16_t)(samples_read * sizeof(int16_t));
    audio_capture_encode_pcm(s_task_samples, samples_read, s_task_frame.payload);

    // Timeout 0: never block the microphone. A full buffer means this frame is dropped.
    const size_t frame_bytes = AUDIO_CAPTURE_FRAME_HEADER_BYTES + s_task_frame.length;
    const size_t sent_bytes = xMessageBufferSend(s_output_buffer, &s_task_frame, frame_bytes, 0);

    portENTER_CRITICAL(&s_stats_lock);
    s_next_sequence++; // advances even on a drop so the receiver sees the gap
    s_stats.next_sequence = s_next_sequence;
    s_stats.frames_captured++;
    if (sent_bytes == frame_bytes)
    {
        s_stats.frames_sent++;
    }
    else
    {
        s_stats.frames_dropped++;
    }
    portEXIT_CRITICAL(&s_stats_lock);
}

static void log_summary(void)
{
    const size_t stack_free_bytes = (size_t)uxTaskGetStackHighWaterMark(NULL); // no RTOS calls inside the lock
    audio_capture_stats_t stats;
    portENTER_CRITICAL(&s_stats_lock);
    s_stats.stack_free_bytes = stack_free_bytes;
    stats = s_stats;
    portEXIT_CRITICAL(&s_stats_lock);

    ESP_LOGI(TAG, "frames=%lu sent=%lu dropped=%lu timeouts=%lu errors=%lu dma_overflows=%lu stack_free=%u",
             (unsigned long)stats.frames_captured, (unsigned long)stats.frames_sent,
             (unsigned long)stats.frames_dropped, (unsigned long)stats.read_timeouts, (unsigned long)stats.read_errors,
             (unsigned long)s_dma_overflows, (unsigned)stats.stack_free_bytes);
}

static void capture_task(void *arg)
{
    (void)arg;
    TickType_t last_summary = xTaskGetTickCount();

    while (s_task_should_run)
    {
        capture_one_frame();

        const TickType_t now = xTaskGetTickCount();
        if (now - last_summary >= pdMS_TO_TICKS(SUMMARY_INTERVAL_MS))
        {
            log_summary();
            last_summary = now;
        }
    }

    s_task_active = false;
    vTaskDelete(NULL);
}

/* ===========================================================================
 * Driver layer
 * ======================================================================== */

esp_err_t audio_capture_init(const audio_capture_config_t *config)
{
    if (config == NULL || !is_valid_config(config))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_is_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // ESP32 is master: it drives SCK and WS, the microphone answers on SD.
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = DMA_DESC_NUM;
    channel_config.dma_frame_num = AUDIO_SAMPLES_PER_FRAME; // one 20 ms frame per DMA descriptor

    esp_err_t result = i2s_new_channel(&channel_config, NULL, &s_rx_channel);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "I2S channel allocation failed: %s", esp_err_to_name(result));
        s_rx_channel = NULL;
        return result;
    }

    result = configure_channel(config);
    if (result == ESP_OK)
    {
        result = configure_status_led(config->status_led_gpio);
    }
    if (result != ESP_OK)
    {
        i2s_del_channel(s_rx_channel);
        s_rx_channel = NULL;
        return result;
    }

    s_config = *config;
    s_dma_overflows = 0;
    s_next_sequence = 0;
    s_last_read_result = ESP_OK;
    memset(&s_stats, 0, sizeof(s_stats));
    s_is_running = false;
    s_is_initialized = true;

    ESP_LOGI(TAG, "Initialized: %d Hz, %d samples/frame, SCK=%d WS=%d SD=%d, gain +%d dB", AUDIO_SAMPLE_RATE_HZ,
             AUDIO_SAMPLES_PER_FRAME, config->bclk_gpio, config->ws_gpio, config->din_gpio, 6 * config->gain_shift);
    return ESP_OK;
}

esp_err_t audio_capture_start(void)
{
    if (!s_is_initialized)
    {
        ESP_LOGE(TAG, "Driver not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_is_running)
    {
        return ESP_OK;
    }

    const esp_err_t result = i2s_channel_enable(s_rx_channel);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "I2S start failed: %s", esp_err_to_name(result));
        return result;
    }

    s_is_running = true;
    return ESP_OK;
}

esp_err_t audio_capture_stop(void)
{
    if (!s_is_initialized || s_task_active)
    {
        return ESP_ERR_INVALID_STATE; // the capture task must be stopped first
    }
    if (!s_is_running)
    {
        return ESP_OK;
    }

    const esp_err_t result = i2s_channel_disable(s_rx_channel);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "I2S stop failed: %s", esp_err_to_name(result));
        return result;
    }

    s_is_running = false;
    return ESP_OK;
}

esp_err_t audio_capture_read_frame(int16_t *samples, size_t *samples_read, uint32_t timeout_ms)
{
    if (samples_read != NULL)
    {
        *samples_read = 0;
    }
    if (samples == NULL || samples_read == NULL || timeout_ms == 0 || timeout_ms > AUDIO_CAPTURE_MAX_READ_TIMEOUT_MS)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_is_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes_read = 0;
    // Note: i2s_channel_read takes milliseconds, not FreeRTOS ticks.
    const esp_err_t result = i2s_channel_read(s_rx_channel, s_raw_slots, sizeof(s_raw_slots), &bytes_read, timeout_ms);

    const size_t count = bytes_read / sizeof(s_raw_slots[0]);
    for (size_t i = 0; i < count; i++)
    {
        samples[i] = slot_to_pcm16(s_raw_slots[i], s_config.gain_shift);
    }

    *samples_read = count;
    return result;
}

esp_err_t audio_capture_deinit(void)
{
    if (!s_is_initialized)
    {
        return ESP_OK;
    }

    esp_err_t result = audio_capture_task_stop();
    if (result != ESP_OK)
    {
        return result;
    }
    result = audio_capture_stop();
    if (result != ESP_OK)
    {
        return result;
    }

    result = i2s_del_channel(s_rx_channel);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "I2S release failed: %s", esp_err_to_name(result));
        return result; // handle kept so deinit can be retried
    }

    s_rx_channel = NULL;
    if (s_config.status_led_gpio != GPIO_NUM_NC)
    {
        gpio_set_level(s_config.status_led_gpio, 0);
        gpio_reset_pin(s_config.status_led_gpio);
    }
    s_is_initialized = false;
    return ESP_OK;
}

/* ===========================================================================
 * Task layer
 * ======================================================================== */

esp_err_t audio_capture_task_start(MessageBufferHandle_t output_buffer)
{
    if (output_buffer == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_is_initialized || s_task_active)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = audio_capture_start();
    if (result != ESP_OK)
    {
        return result;
    }

    s_output_buffer = output_buffer;
    s_last_read_result = ESP_OK;
    s_task_should_run = true;
    s_task_active = true;

    if (xTaskCreate(capture_task, "audio_capture", TASK_STACK_BYTES, NULL, TASK_PRIORITY, &s_task_handle) != pdPASS)
    {
        ESP_LOGE(TAG, "Capture task creation failed");
        s_task_should_run = false;
        s_task_active = false;
        s_task_handle = NULL;
        s_output_buffer = NULL;
        audio_capture_stop();
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_stats_lock);
    s_stats.is_capturing = true;
    portEXIT_CRITICAL(&s_stats_lock);
    set_status_led(true);
    ESP_LOGI(TAG, "Capture started (next sequence %u)", (unsigned)s_next_sequence);
    return ESP_OK;
}

esp_err_t audio_capture_task_stop(void)
{
    if (!s_task_active)
    {
        return ESP_OK;
    }

    s_task_should_run = false;
    for (int attempt = 0; attempt < STOP_POLL_ATTEMPTS && s_task_active; attempt++)
    {
        vTaskDelay(pdMS_TO_TICKS(STOP_POLL_MS));
    }
    if (s_task_active)
    {
        ESP_LOGE(TAG, "Capture task did not exit in time");
        return ESP_ERR_TIMEOUT;
    }

    s_task_handle = NULL;
    s_output_buffer = NULL;
    portENTER_CRITICAL(&s_stats_lock);
    s_stats.is_capturing = false;
    portEXIT_CRITICAL(&s_stats_lock);
    set_status_led(false);
    ESP_LOGI(TAG, "Capture stopped");
    return audio_capture_stop();
}

bool audio_capture_is_capturing(void)
{
    return s_task_active && s_task_should_run;
}

esp_err_t audio_capture_get_stats(audio_capture_stats_t *stats)
{
    if (stats == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_stats_lock);
    *stats = s_stats;
    portEXIT_CRITICAL(&s_stats_lock);
    stats->dma_overflows = s_dma_overflows;
    return ESP_OK;
}

/* ===========================================================================
 * PCM byte order (little-endian, low byte first)
 * ======================================================================== */

void audio_capture_encode_pcm(const int16_t *samples, size_t sample_count, uint8_t *dest)
{
    for (size_t i = 0; i < sample_count; i++)
    {
        const uint16_t bits = (uint16_t)samples[i];
        dest[2 * i] = (uint8_t)(bits & 0xFFu);
        dest[2 * i + 1] = (uint8_t)(bits >> 8);
    }
}

void audio_capture_decode_pcm(const uint8_t *src, size_t sample_count, int16_t *samples)
{
    for (size_t i = 0; i < sample_count; i++)
    {
        const uint16_t bits = (uint16_t)((uint16_t)src[2 * i] | (uint16_t)((uint16_t)src[2 * i + 1] << 8));
        samples[i] = (int16_t)bits;
    }
}
