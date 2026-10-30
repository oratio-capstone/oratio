#include "audio_output.h"
#include "audio_common.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "HOST_AUDIO_OUTPUT";
static const audio_output_config_t CONFIG = {GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_7};

static uint8_t s_channel_token;
static bool s_allocated;
static bool s_enabled;
static bool s_preloaded;
static esp_err_t s_new_result;
static esp_err_t s_init_result;
static esp_err_t s_preload_result;
static esp_err_t s_enable_result;
static esp_err_t s_disable_result;
static esp_err_t s_delete_result;
static esp_err_t s_write_result;
static size_t s_write_limit;
static size_t s_preload_limit;
static size_t s_new_calls;
static size_t s_write_calls;
static uint32_t s_last_timeout_ms;
static int64_t s_now_us;
static int64_t s_write_advance_us;
static uint8_t s_output[AUDIO_FRAME_BYTES * 2];
static size_t s_output_bytes;

static void reset_stub(void)
{
    assert(audio_output_deinit() == ESP_OK);
    assert(!s_allocated);
    s_new_result = ESP_OK;
    s_init_result = ESP_OK;
    s_preload_result = ESP_OK;
    s_enable_result = ESP_OK;
    s_disable_result = ESP_OK;
    s_delete_result = ESP_OK;
    s_write_result = ESP_OK;
    s_write_limit = SIZE_MAX;
    s_preload_limit = SIZE_MAX;
    s_new_calls = 0;
    s_write_calls = 0;
    s_last_timeout_ms = 0;
    s_now_us = 0;
    s_write_advance_us = 0;
    s_output_bytes = 0;
    s_preloaded = false;
    memset(s_output, 0, sizeof(s_output));
}

esp_err_t i2s_new_channel(const i2s_chan_config_t *config, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx)
{
    s_new_calls++;
    assert(!s_allocated && rx == NULL);
    assert(config->id == I2S_NUM_0 && config->role == I2S_ROLE_MASTER);
    assert(config->auto_clear);
    assert(config->dma_desc_num * config->dma_frame_num * 4 == sizeof(s_output));
    if (s_new_result == ESP_OK)
    {
        s_allocated = true;
        *tx = &s_channel_token;
    }
    return s_new_result;
}

esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t channel, const i2s_std_config_t *config)
{
    assert(channel == &s_channel_token && s_allocated);
    assert(config->clk_cfg.sample_rate_hz == 16000);
    assert(config->slot_cfg.data_bit_width == 16 && config->slot_cfg.slot_mode == I2S_SLOT_MODE_STEREO);
    assert(config->gpio_cfg.bclk == CONFIG.bclk_gpio && config->gpio_cfg.ws == CONFIG.ws_gpio);
    assert(config->gpio_cfg.dout == CONFIG.dout_gpio && config->gpio_cfg.din == GPIO_NUM_NC);
    return s_init_result;
}

esp_err_t i2s_channel_preload_data(i2s_chan_handle_t channel, const void *source, size_t length, size_t *loaded)
{
    assert(channel == &s_channel_token && !s_enabled);
    const uint8_t *bytes = source;
    for (size_t index = 0; index < length; index++)
    {
        assert(bytes[index] == 0);
    }
    *loaded = length < s_preload_limit ? length : s_preload_limit;
    s_preloaded = s_preload_result == ESP_OK && *loaded == sizeof(s_output);
    return s_preload_result;
}

esp_err_t i2s_channel_enable(i2s_chan_handle_t channel)
{
    assert(channel == &s_channel_token && s_preloaded && !s_enabled);
    if (s_enable_result == ESP_OK)
    {
        s_enabled = true;
    }
    return s_enable_result;
}

esp_err_t i2s_channel_disable(i2s_chan_handle_t channel)
{
    assert(channel == &s_channel_token && s_enabled);
    if (s_disable_result == ESP_OK)
    {
        s_enabled = false;
        s_preloaded = false;
    }
    return s_disable_result;
}

esp_err_t i2s_del_channel(i2s_chan_handle_t channel)
{
    assert(channel == &s_channel_token && s_allocated && !s_enabled);
    if (s_delete_result == ESP_OK)
    {
        s_allocated = false;
    }
    return s_delete_result;
}

esp_err_t i2s_channel_write(i2s_chan_handle_t channel, const void *source, size_t length, size_t *written,
                            uint32_t timeout_ms)
{
    assert(channel == &s_channel_token && s_enabled);
    assert(length <= 320); /* At most two DMA waits, including a partially filled descriptor. */
    s_write_calls++;
    s_last_timeout_ms = timeout_ms;
    if (timeout_ms == 0 && s_write_advance_us > 0)
    {
        *written = 0;
        return ESP_ERR_TIMEOUT;
    }
    *written = length < s_write_limit ? length : s_write_limit;
    assert(s_output_bytes + *written <= sizeof(s_output));
    memcpy(s_output + s_output_bytes, source, *written);
    s_output_bytes += *written;
    s_now_us += s_write_advance_us;
    return s_write_result;
}

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

static void start_output(void)
{
    assert(audio_output_init(&CONFIG) == ESP_OK);
    assert(audio_output_start() == ESP_OK);
}

static void test_arguments_and_states(void)
{
    reset_stub();
    size_t written = 99;
    int16_t sample = 1;
    audio_output_config_t invalid = CONFIG;
    invalid.dout_gpio = invalid.ws_gpio;
    assert(audio_output_init(NULL) == ESP_ERR_INVALID_ARG);
    assert(audio_output_init(&invalid) == ESP_ERR_INVALID_ARG);
    invalid.dout_gpio = GPIO_NUM_NC;
    assert(audio_output_init(&invalid) == ESP_ERR_INVALID_ARG);
    assert(s_new_calls == 0);
    assert(audio_output_start() == ESP_ERR_INVALID_STATE);
    assert(audio_output_stop() == ESP_ERR_INVALID_STATE);
    assert(audio_output_write(&sample, 1, &written, 100) == ESP_ERR_INVALID_STATE && written == 0);
    start_output();
    assert(audio_output_init(&CONFIG) == ESP_ERR_INVALID_STATE);
    assert(audio_output_start() == ESP_ERR_INVALID_STATE);
    assert(audio_output_write(NULL, 1, &written, 100) == ESP_ERR_INVALID_ARG && written == 0);
    assert(audio_output_write(&sample, 0, &written, 100) == ESP_ERR_INVALID_ARG);
    assert(audio_output_write(&sample, AUDIO_SAMPLES_PER_FRAME + 1, &written, 100) == ESP_ERR_INVALID_ARG);
    assert(audio_output_write(&sample, 1, NULL, 100) == ESP_ERR_INVALID_ARG);
    assert(audio_output_write(&sample, 1, &written, 0) == ESP_ERR_INVALID_ARG);
    assert(audio_output_write(&sample, 1, &written, AUDIO_OUTPUT_MAX_WRITE_TIMEOUT_MS + 1) == ESP_ERR_INVALID_ARG);
    assert(audio_output_write(&sample, 1, &written, UINT32_MAX) == ESP_ERR_INVALID_ARG);
    assert(s_write_calls == 0);
}

static void test_packing_partial_writes_and_deadline(void)
{
    reset_stub();
    start_output();
    const int16_t samples[] = {INT16_MIN, -1, 0, INT16_MAX};
    const int16_t expected[] = {INT16_MIN, INT16_MIN, -1, -1, 0, 0, INT16_MAX, INT16_MAX};
    size_t written = 0;
    s_write_limit = 4;
    s_write_advance_us = 2000;
    assert(audio_output_write(samples, 4, &written, 10) == ESP_OK && written == 4);
    assert(s_write_calls == 4 && s_last_timeout_ms == 2);
    assert(memcmp(s_output, expected, sizeof(expected)) == 0);

    s_output_bytes = 0;
    s_write_calls = 0;
    assert(audio_output_write(samples, 4, &written, 3) == ESP_ERR_TIMEOUT && written == 1);
    assert(s_write_calls == 2 && s_last_timeout_ms == 0);

    reset_stub();
    start_output();
    const int16_t frame[AUDIO_SAMPLES_PER_FRAME] = {0};
    s_write_advance_us = 2000;
    assert(audio_output_write(frame, AUDIO_SAMPLES_PER_FRAME, &written, 10) == ESP_OK);
    assert(written == AUDIO_SAMPLES_PER_FRAME && s_write_calls == 4 && s_last_timeout_ms == 2);
}

static void test_timeout_and_resume(void)
{
    reset_stub();
    start_output();
    const int16_t samples[] = {-123, 456, 789};
    const int16_t expected[] = {-123, -123, 456, 456, 789, 789};
    size_t written = 0;
    s_write_limit = 4;
    s_write_result = ESP_ERR_TIMEOUT;
    assert(audio_output_write(samples, 3, &written, 20) == ESP_ERR_TIMEOUT && written == 1);
    s_write_result = ESP_OK;
    s_write_limit = SIZE_MAX;
    assert(audio_output_write(samples + written, 3 - written, &written, 20) == ESP_OK && written == 2);
    assert(memcmp(s_output, expected, sizeof(expected)) == 0);
}

static void test_write_faults(void)
{
    const int16_t sample = 10;
    size_t written = 0;
    reset_stub();
    start_output();
    s_write_limit = 0;
    assert(audio_output_write(&sample, 1, &written, 100) == ESP_FAIL && written == 0);
    assert(audio_output_write(&sample, 1, &written, 100) == ESP_ERR_INVALID_STATE);
    reset_stub();
    start_output();
    s_write_limit = 2;
    assert(audio_output_write(&sample, 1, &written, 100) == ESP_FAIL && written == 0);
    reset_stub();
    start_output();
    s_write_result = ESP_FAIL;
    assert(audio_output_write(&sample, 1, &written, 100) == ESP_FAIL && written == 1);
    assert(audio_output_write(&sample, 1, &written, 100) == ESP_ERR_INVALID_STATE);
}

static void test_initialization_and_cleanup_failures(void)
{
    reset_stub();
    s_new_result = ESP_ERR_NO_MEM;
    assert(audio_output_init(&CONFIG) == ESP_ERR_NO_MEM && !s_allocated);
    s_new_result = ESP_OK;
    s_init_result = ESP_FAIL;
    assert(audio_output_init(&CONFIG) == ESP_FAIL && !s_allocated);
    s_delete_result = ESP_FAIL;
    assert(audio_output_init(&CONFIG) == ESP_FAIL && s_allocated);
    assert(audio_output_init(&CONFIG) == ESP_ERR_INVALID_STATE);
    assert(audio_output_start() == ESP_ERR_INVALID_STATE);
    s_delete_result = ESP_OK;
    assert(audio_output_deinit() == ESP_OK && !s_allocated);

    reset_stub();
    assert(audio_output_init(&CONFIG) == ESP_OK);
    s_preload_limit = 4;
    assert(audio_output_start() == ESP_FAIL && !s_enabled);
    assert(audio_output_start() == ESP_ERR_INVALID_STATE);
    reset_stub();
    assert(audio_output_init(&CONFIG) == ESP_OK);
    s_preload_result = ESP_FAIL;
    assert(audio_output_start() == ESP_FAIL && !s_enabled);
    reset_stub();
    assert(audio_output_init(&CONFIG) == ESP_OK);
    s_enable_result = ESP_FAIL;
    assert(audio_output_start() == ESP_FAIL && !s_enabled);
    assert(audio_output_start() == ESP_ERR_INVALID_STATE);
}

static void test_stop_restart_and_retry_cleanup(void)
{
    reset_stub();
    start_output();
    size_t written = 0;
    const int16_t sample = 100;
    assert(audio_output_write(&sample, 1, &written, 100) == ESP_OK);
    assert(audio_output_stop() == ESP_OK);
    assert(audio_output_write(&sample, 1, &written, 100) == ESP_ERR_INVALID_STATE);
    assert(audio_output_stop() == ESP_ERR_INVALID_STATE);
    assert(audio_output_start() == ESP_OK); /* Stub checks that preload contains only silence. */
    s_disable_result = ESP_FAIL;
    assert(audio_output_deinit() == ESP_FAIL && s_allocated && s_enabled);
    s_disable_result = ESP_OK;
    s_delete_result = ESP_FAIL;
    assert(audio_output_deinit() == ESP_FAIL && s_allocated && !s_enabled);
    s_delete_result = ESP_OK;
    assert(audio_output_deinit() == ESP_OK && !s_allocated);
    assert(audio_output_deinit() == ESP_OK);
}

int main(void)
{
    ESP_LOGI(TAG, "Running host driver contract checks");
    test_arguments_and_states();
    test_packing_partial_writes_and_deadline();
    test_timeout_and_resume();
    test_write_faults();
    test_initialization_and_cleanup_failures();
    test_stop_restart_and_retry_cleanup();
    return 0;
}
