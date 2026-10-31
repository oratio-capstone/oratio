#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/* Minimal host double of the ESP-IDF v5 standard-mode I2S API used by audio_capture.c. */
typedef struct i2s_channel_obj_t *i2s_chan_handle_t;

typedef enum
{
    I2S_NUM_0 = 0
} i2s_port_t;

typedef enum
{
    I2S_ROLE_MASTER = 0,
    I2S_ROLE_SLAVE = 1
} i2s_role_t;

typedef enum
{
    I2S_DATA_BIT_WIDTH_16BIT = 16,
    I2S_DATA_BIT_WIDTH_32BIT = 32
} i2s_data_bit_width_t;

typedef enum
{
    I2S_SLOT_MODE_MONO = 1,
    I2S_SLOT_MODE_STEREO = 2
} i2s_slot_mode_t;

typedef enum
{
    I2S_STD_SLOT_LEFT = 1,
    I2S_STD_SLOT_RIGHT = 2,
    I2S_STD_SLOT_BOTH = 3
} i2s_std_slot_mask_t;

typedef struct
{
    i2s_port_t id;
    i2s_role_t role;
    uint32_t dma_desc_num;
    uint32_t dma_frame_num;
    bool auto_clear;
} i2s_chan_config_t;

typedef struct
{
    uint32_t sample_rate_hz;
} i2s_std_clk_config_t;

typedef struct
{
    i2s_data_bit_width_t data_bit_width;
    i2s_slot_mode_t slot_mode;
    i2s_std_slot_mask_t slot_mask;
} i2s_std_slot_config_t;

typedef struct
{
    gpio_num_t mclk;
    gpio_num_t bclk;
    gpio_num_t ws;
    gpio_num_t dout;
    gpio_num_t din;
    struct
    {
        uint32_t mclk_inv : 1;
        uint32_t bclk_inv : 1;
        uint32_t ws_inv : 1;
    } invert_flags;
} i2s_std_gpio_config_t;

typedef struct
{
    i2s_std_clk_config_t clk_cfg;
    i2s_std_slot_config_t slot_cfg;
    i2s_std_gpio_config_t gpio_cfg;
} i2s_std_config_t;

typedef struct
{
    void *data;
    size_t size;
} i2s_event_data_t;

typedef bool (*i2s_isr_callback_t)(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx);

typedef struct
{
    i2s_isr_callback_t on_recv;
    i2s_isr_callback_t on_recv_q_ovf;
    i2s_isr_callback_t on_sent;
    i2s_isr_callback_t on_send_q_ovf;
} i2s_event_callbacks_t;

#define I2S_GPIO_UNUSED GPIO_NUM_NC

#define I2S_CHANNEL_DEFAULT_CONFIG(i2s_num, i2s_role)                                                              \
    {                                                                                                              \
        .id = (i2s_num), .role = (i2s_role), .dma_desc_num = 6, .dma_frame_num = 240, .auto_clear = false          \
    }
#define I2S_STD_CLK_DEFAULT_CONFIG(rate)                                                                           \
    {                                                                                                              \
        .sample_rate_hz = (rate)                                                                                   \
    }
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode)                                                            \
    {                                                                                                              \
        .data_bit_width = (bits), .slot_mode = (mode),                                                             \
        .slot_mask = ((mode) == I2S_SLOT_MODE_MONO) ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_BOTH                        \
    }

esp_err_t i2s_new_channel(const i2s_chan_config_t *chan_cfg, i2s_chan_handle_t *ret_tx_handle,
                          i2s_chan_handle_t *ret_rx_handle);
esp_err_t i2s_del_channel(i2s_chan_handle_t handle);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *std_cfg);
esp_err_t i2s_channel_register_event_callback(i2s_chan_handle_t handle, const i2s_event_callbacks_t *callbacks,
                                              void *user_data);
esp_err_t i2s_channel_enable(i2s_chan_handle_t handle);
esp_err_t i2s_channel_disable(i2s_chan_handle_t handle);
esp_err_t i2s_channel_read(i2s_chan_handle_t handle, void *dest, size_t size, size_t *bytes_read,
                           uint32_t timeout_ms);
