#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef void *i2s_chan_handle_t;

enum
{
    I2S_NUM_0 = 0,
    I2S_ROLE_MASTER = 1,
    I2S_DATA_BIT_WIDTH_16BIT = 16,
    I2S_SLOT_MODE_STEREO = 2,
    I2S_GPIO_UNUSED = -1
};

typedef struct
{
    uint32_t id;
    uint32_t role;
    uint32_t dma_desc_num;
    uint32_t dma_frame_num;
    bool auto_clear;
} i2s_chan_config_t;

typedef struct
{
    struct
    {
        uint32_t sample_rate_hz;
    } clk_cfg;
    struct
    {
        uint32_t data_bit_width;
        uint32_t slot_mode;
    } slot_cfg;
    struct
    {
        gpio_num_t mclk;
        gpio_num_t bclk;
        gpio_num_t ws;
        gpio_num_t dout;
        gpio_num_t din;
    } gpio_cfg;
} i2s_std_config_t;

#define I2S_CHANNEL_DEFAULT_CONFIG(port, mode) {.id = (port), .role = (mode)}
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) {.sample_rate_hz = (rate)}
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) {.data_bit_width = (bits), .slot_mode = (mode)}

esp_err_t i2s_new_channel(const i2s_chan_config_t *config, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t channel, const i2s_std_config_t *config);
esp_err_t i2s_channel_preload_data(i2s_chan_handle_t channel, const void *source, size_t length, size_t *loaded);
esp_err_t i2s_channel_enable(i2s_chan_handle_t channel);
esp_err_t i2s_channel_disable(i2s_chan_handle_t channel);
esp_err_t i2s_del_channel(i2s_chan_handle_t channel);
esp_err_t i2s_channel_write(i2s_chan_handle_t channel, const void *source, size_t length, size_t *written,
                            uint32_t timeout_ms);
