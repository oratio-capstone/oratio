#include "audio_output.h"
#include "audio_common.h"

#include <stdbool.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "AUDIO_OUTPUT";

enum
{
    OUTPUT_SLOTS = 2,
    OUTPUT_BYTES_PER_SAMPLE = OUTPUT_SLOTS * sizeof(int16_t),
    DMA_DESCRIPTOR_COUNT = 4,
    DMA_FRAMES_PER_DESCRIPTOR = AUDIO_SAMPLES_PER_FRAME / DMA_DESCRIPTOR_COUNT,
    DMA_BYTES_PER_DESCRIPTOR = DMA_FRAMES_PER_DESCRIPTOR * OUTPUT_BYTES_PER_SAMPLE
};

/* IDF allocates its own DMA-capable buffers. This ordinary buffer is copied in. */
static int16_t s_i2s_samples[AUDIO_SAMPLES_PER_FRAME * OUTPUT_SLOTS];
static i2s_chan_handle_t s_tx_channel;
static bool s_running;
static bool s_faulted;

static bool config_is_valid(const audio_output_config_t *config)
{
    return config != NULL && GPIO_IS_VALID_OUTPUT_GPIO(config->bclk_gpio) &&
           GPIO_IS_VALID_OUTPUT_GPIO(config->ws_gpio) && GPIO_IS_VALID_OUTPUT_GPIO(config->dout_gpio) &&
           config->bclk_gpio != config->ws_gpio && config->bclk_gpio != config->dout_gpio &&
           config->ws_gpio != config->dout_gpio;
}

esp_err_t audio_output_init(const audio_output_config_t *config)
{
    if (!config_is_valid(config))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_tx_channel != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = DMA_DESCRIPTOR_COUNT;
    channel_config.dma_frame_num = DMA_FRAMES_PER_DESCRIPTOR;
    channel_config.auto_clear = true;

    esp_err_t result = i2s_new_channel(&channel_config, &s_tx_channel, NULL);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Channel allocation failed: %s", esp_err_to_name(result));
        return result;
    }

    const i2s_std_config_t standard_config =
    {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
        {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = config->bclk_gpio,
            .ws = config->ws_gpio,
            .dout = config->dout_gpio,
            .din = I2S_GPIO_UNUSED
        }
    };

    result = i2s_channel_init_std_mode(s_tx_channel, &standard_config);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Standard I2S initialization failed: %s", esp_err_to_name(result));
        s_faulted = true;
        const esp_err_t cleanup_result = audio_output_deinit();
        if (cleanup_result != ESP_OK)
        {
            ESP_LOGE(TAG, "Cleanup failed: %s; retry deinit", esp_err_to_name(cleanup_result));
        }
        return result;
    }

    ESP_LOGI(TAG, "Ready: %u Hz, stereo PCM16 slots, BCLK=%d WS=%d DOUT=%d",
             (unsigned int)AUDIO_SAMPLE_RATE_HZ, config->bclk_gpio, config->ws_gpio, config->dout_gpio);
    return ESP_OK;
}

esp_err_t audio_output_start(void)
{
    if (s_tx_channel == NULL || s_running || s_faulted)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* Clear every DMA descriptor before start/restart, avoiding old audio. */
    memset(s_i2s_samples, 0, sizeof(s_i2s_samples));
    size_t bytes_loaded = 0;
    esp_err_t result = i2s_channel_preload_data(s_tx_channel, s_i2s_samples, sizeof(s_i2s_samples), &bytes_loaded);
    if (result != ESP_OK || bytes_loaded != sizeof(s_i2s_samples))
    {
        s_faulted = true;
        return result == ESP_OK ? ESP_FAIL : result;
    }

    result = i2s_channel_enable(s_tx_channel);
    if (result == ESP_OK)
    {
        s_running = true;
        ESP_LOGI(TAG, "Started");
    }
    else
    {
        s_faulted = true;
    }
    return result;
}

esp_err_t audio_output_write(const int16_t *samples, size_t sample_count, size_t *samples_written,
                             uint32_t timeout_ms)
{
    if (samples_written == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *samples_written = 0;

    if (samples == NULL || sample_count == 0 || sample_count > AUDIO_SAMPLES_PER_FRAME || timeout_ms == 0 ||
        timeout_ms > AUDIO_OUTPUT_MAX_WRITE_TIMEOUT_MS)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_running || s_faulted)
    {
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t index = 0; index < sample_count; index++)
    {
        s_i2s_samples[index * OUTPUT_SLOTS] = samples[index];
        s_i2s_samples[index * OUTPUT_SLOTS + 1] = samples[index];
    }

    const size_t total_bytes = sample_count * OUTPUT_BYTES_PER_SAMPLE;
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t accepted_bytes = 0;

    while (accepted_bytes < total_bytes)
    {
        const int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0)
        {
            return ESP_ERR_TIMEOUT;
        }

        /* A descriptor-sized write may cross two descriptors when a previous
         * partial write (or starvation recovery) leaves IDF partway through one.
         * Budget for both waits. A sub-2 ms remainder permits a nonblocking try. */
        const uint32_t wait_timeout_ms = (uint32_t)(remaining_us / 2000);
        const size_t requested_bytes = total_bytes - accepted_bytes < DMA_BYTES_PER_DESCRIPTOR
                                           ? total_bytes - accepted_bytes
                                           : DMA_BYTES_PER_DESCRIPTOR;
        size_t bytes_written = 0;
        /* IDF applies its timeout to each DMA wait, not the entire write. */
        const esp_err_t result = i2s_channel_write(s_tx_channel, (const uint8_t *)s_i2s_samples + accepted_bytes,
                                                  requested_bytes, &bytes_written, wait_timeout_ms);

        /* An incomplete hardware sample has no safe sample-level retry boundary. */
        if (bytes_written > requested_bytes || bytes_written % OUTPUT_BYTES_PER_SAMPLE != 0)
        {
            s_faulted = true;
            return ESP_FAIL;
        }

        accepted_bytes += bytes_written;
        *samples_written = accepted_bytes / OUTPUT_BYTES_PER_SAMPLE;
        if (result != ESP_OK)
        {
            s_faulted = result != ESP_ERR_TIMEOUT;
            return result;
        }

        if (bytes_written == 0)
        {
            s_faulted = true;
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

esp_err_t audio_output_stop(void)
{
    if (s_tx_channel == NULL || !s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t result = i2s_channel_disable(s_tx_channel);
    if (result == ESP_OK)
    {
        s_running = false;
        ESP_LOGI(TAG, "Stopped (pending audio discarded)");
    }
    else
    {
        s_faulted = true;
    }
    return result;
}

esp_err_t audio_output_deinit(void)
{
    if (s_tx_channel == NULL)
    {
        return ESP_OK;
    }

    if (s_running)
    {
        const esp_err_t result = audio_output_stop();
        if (result != ESP_OK)
        {
            return result;
        }
    }

    const esp_err_t result = i2s_del_channel(s_tx_channel);
    if (result == ESP_OK)
    {
        s_tx_channel = NULL;
        s_faulted = false;
        ESP_LOGI(TAG, "Released");
    }
    else
    {
        s_faulted = true;
    }
    return result;
}
