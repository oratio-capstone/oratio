#include "audio_output.h"
#include "audio_common.h"

#include "driver/i2s_std.h"

static i2s_chan_handle_t s_tx;
static bool s_running;

esp_err_t audio_output_open(void)
{
    if (s_tx != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const gpio_config_t amp_config =
    {
        .pin_bit_mask = 1ULL << AUDIO_OUTPUT_SD_MODE_GPIO,
        .mode = GPIO_MODE_OUTPUT
    };
    esp_err_t result = gpio_config(&amp_config);
    if (result != ESP_OK)
    {
        return result;
    }
    result = gpio_set_level(AUDIO_OUTPUT_SD_MODE_GPIO, 0);
    if (result != ESP_OK)
    {
        return result;
    }

    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_OUTPUT_I2S_PORT, I2S_ROLE_MASTER);
    channel.dma_desc_num = 4;
    channel.dma_frame_num = 80;
    channel.auto_clear = true;
    result = i2s_new_channel(&channel, &s_tx, NULL);
    if (result != ESP_OK)
    {
        return result;
    }

    const i2s_std_config_t standard =
    {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
        {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_OUTPUT_BCLK_GPIO,
            .ws = AUDIO_OUTPUT_WS_GPIO,
            .dout = AUDIO_OUTPUT_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED
        }
    };
    result = i2s_channel_init_std_mode(s_tx, &standard);
    if (result == ESP_OK)
    {
        result = i2s_channel_enable(s_tx);
        s_running = result == ESP_OK;
    }
    if (result == ESP_OK)
    {
        result = audio_output_set_enabled(true);
    }
    if (result != ESP_OK)
    {
        // Retain the handle if cleanup fails so close can be retried.
        (void)audio_output_close();
    }
    return result;
}

esp_err_t audio_output_write(const int16_t *samples, size_t sample_count)
{
    if (samples == NULL || sample_count == 0 || sample_count > AUDIO_SAMPLES_PER_FRAME)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    size_t written = 0;
    const size_t bytes = sample_count * sizeof(*samples);
    const esp_err_t result = i2s_channel_write(s_tx, samples, bytes, &written, 1000);
    return result != ESP_OK ? result : (written == bytes ? ESP_OK : ESP_FAIL);
}

esp_err_t audio_output_set_enabled(bool enabled)
{
    if (!s_running)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return gpio_set_level(AUDIO_OUTPUT_SD_MODE_GPIO, enabled);
}

esp_err_t audio_output_close(void)
{
    if (s_tx == NULL)
    {
        return ESP_OK;
    }
    esp_err_t result = gpio_set_level(AUDIO_OUTPUT_SD_MODE_GPIO, 0);
    if (result != ESP_OK)
    {
        return result;
    }
    if (s_running)
    {
        result = i2s_channel_disable(s_tx);
        if (result != ESP_OK)
        {
            return result;
        }
        s_running = false;
    }
    result = i2s_del_channel(s_tx);
    if (result == ESP_OK)
    {
        s_tx = NULL;
    }
    return result;
}
