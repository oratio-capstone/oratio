#include "audio_capture.h"

#include "audio_common.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "AUDIO_CAPTURE";

static i2s_chan_handle_t s_rx_handle = NULL;

// The INMP441 sends each 24-bit sample left-justified in a 32-bit I2S slot.
static int32_t s_raw[AUDIO_SAMPLES_PER_FRAME];

esp_err_t audio_capture_open(void)
{
    if (s_rx_handle != NULL)
    {
        return ESP_ERR_INVALID_STATE; // already open
    }

    // The ESP32 is the I2S master: it drives SCK and WS, the mic answers on SD.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_CAPTURE_I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_handle); // NULL = no TX, receive only
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = AUDIO_CAPTURE_SCK_GPIO,
                .ws = AUDIO_CAPTURE_WS_GPIO,
                .dout = I2S_GPIO_UNUSED,
                .din = AUDIO_CAPTURE_SD_GPIO,
            },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT; // INMP441 L/R pin tied to GND

    err = i2s_channel_init_std_mode(s_rx_handle, &std_cfg);
    if (err == ESP_OK)
    {
        err = i2s_channel_enable(s_rx_handle);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "I2S setup failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
        return err;
    }

    ESP_LOGI(TAG, "Opened: %d Hz, SCK=%d WS=%d SD=%d", AUDIO_SAMPLE_RATE_HZ, AUDIO_CAPTURE_SCK_GPIO,
             AUDIO_CAPTURE_WS_GPIO, AUDIO_CAPTURE_SD_GPIO);
    return ESP_OK;
}

esp_err_t audio_capture_read(int16_t *samples, size_t sample_count)
{
    if (samples == NULL || sample_count == 0 || sample_count > AUDIO_SAMPLES_PER_FRAME)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_rx_handle == NULL)
    {
        return ESP_ERR_INVALID_STATE; // call audio_capture_open first
    }

    // Blocks until all samples have arrived (20 ms for a full frame).
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_rx_handle, s_raw, sample_count * sizeof(int32_t), &bytes_read, portMAX_DELAY);
    if (err != ESP_OK)
    {
        return err;
    }

    for (size_t i = 0; i < sample_count; i++)
    {
        samples[i] = (int16_t)(s_raw[i] >> 16); // keep the top 16 bits -> 16-bit PCM
    }
    return ESP_OK;
}

esp_err_t audio_capture_close(void)
{
    if (s_rx_handle == NULL)
    {
        return ESP_OK; // nothing to close
    }

    esp_err_t err = i2s_channel_disable(s_rx_handle);
    if (err == ESP_OK)
    {
        err = i2s_del_channel(s_rx_handle);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "I2S close failed: %s", esp_err_to_name(err));
        return err;
    }

    s_rx_handle = NULL;
    return ESP_OK;
}
