#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2s_types.h"
#include "esp_err.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3
#define AUDIO_OUTPUT_I2S_PORT I2S_NUM_1 // mic uses I2S_NUM_0
#define AUDIO_OUTPUT_BCLK_GPIO GPIO_NUM_42
#define AUDIO_OUTPUT_WS_GPIO GPIO_NUM_2
#define AUDIO_OUTPUT_DOUT_GPIO GPIO_NUM_1
#define AUDIO_OUTPUT_SD_MODE_GPIO GPIO_NUM_47
#else
#error "audio_output: unsupported target"
#endif

// Create and enable an I2S TX channel: standard Philips, 16-bit, mono,
// AUDIO_SAMPLE_RATE_HZ, 4 DMA descriptors x 80 frames, auto_clear on.
// Configures SD_MODE as a push-pull output and enables the amp.
esp_err_t audio_output_open(void);

// Blocking write of `sample_count` samples. One i2s_channel_write call.
esp_err_t audio_output_write(const int16_t *samples, size_t sample_count);

// SD_MODE high = amp on, low = amp shutdown.
esp_err_t audio_output_set_enabled(bool enabled);

// Disable the amp, then disable and delete the I2S channel.
esp_err_t audio_output_close(void);
