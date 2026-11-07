#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2s_types.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32C3
#define AUDIO_CAPTURE_I2S_PORT I2S_NUM_0 // C3 has only one I2S controller
#define AUDIO_CAPTURE_SCK_GPIO GPIO_NUM_4
#define AUDIO_CAPTURE_WS_GPIO GPIO_NUM_5
#define AUDIO_CAPTURE_SD_GPIO GPIO_NUM_6
#elif CONFIG_IDF_TARGET_ESP32S3
#define AUDIO_CAPTURE_I2S_PORT I2S_NUM_0   // speaker uses I2S_NUM_1
#define AUDIO_CAPTURE_SCK_GPIO GPIO_NUM_39 // PCB pin map (left side)
#define AUDIO_CAPTURE_WS_GPIO GPIO_NUM_40
#define AUDIO_CAPTURE_SD_GPIO GPIO_NUM_41
#else
#error "audio_capture: unsupported target"
#endif

// Create and enable an I2S RX channel: standard Philips, 32-bit slots, mono left
// (INMP441 L/R tied to GND), AUDIO_SAMPLE_RATE_HZ.
esp_err_t audio_capture_open(void);

// Blocking read of `sample_count` samples, converted in place from 32-bit slots
// to 16-bit PCM (top 16 bits).
esp_err_t audio_capture_read(int16_t *samples, size_t sample_count);

// Disable and delete the I2S channel.
esp_err_t audio_capture_close(void);

// Start the capture task pinned to core 1. audio_capture_open must have succeeded.
// The task loops forever:
//   1. audio_capture_read(frame.samples, AUDIO_SAMPLES_PER_FRAME).
//   2. Set frame.sequence and frame.length; sequence advances every frame.
//   3. xMessageBufferSend to `output` with timeout 0. If full, drop and count it.
// No stop function, no locks. Rate-limited summary log only (style guide).
esp_err_t audio_capture_task_start(MessageBufferHandle_t output);
