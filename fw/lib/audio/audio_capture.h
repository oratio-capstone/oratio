#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_common.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"

/*
 * Capture -> network frame (agreed in the Oct 29 meeting).
 *
 * These are the last three fields of the Oratio packet: sequence, length and
 * audio payload. The network task adds the type ('A') and node ID, then
 * serializes the header big-endian (see oratio_packet_encode).
 *
 * The capture task owns the sequence number: it increments once per captured
 * frame, including frames dropped because the message buffer was full, so the
 * receiver sees a gap whenever audio is missing.
 *
 * payload holds `length` bytes of signed 16-bit mono PCM, each sample stored
 * low byte first (little-endian). Byte order is a proposal: confirm it with
 * playback (Mohamed) and network (Fayza).
 *
 * Named audio_capture_frame_t because audio_common.h already uses
 * audio_frame_t for a plain PCM sample frame.
 */
typedef struct
{
    uint16_t sequence;
    uint16_t length;
    uint8_t payload[AUDIO_FRAME_BYTES];
} audio_capture_frame_t;

enum
{
    AUDIO_CAPTURE_FRAME_HEADER_BYTES = 2 * sizeof(uint16_t),
    AUDIO_CAPTURE_MAX_GAIN_SHIFT = 8,
    AUDIO_CAPTURE_MAX_READ_TIMEOUT_MS = 1000
};

_Static_assert(offsetof(audio_capture_frame_t, payload) == AUDIO_CAPTURE_FRAME_HEADER_BYTES,
               "Frame header must be exactly sequence + length");
_Static_assert(sizeof(audio_capture_frame_t) == AUDIO_CAPTURE_FRAME_HEADER_BYTES + AUDIO_FRAME_BYTES,
               "Unexpected capture frame layout");

/* Message buffer size that holds `frames` full capture frames, including the
 * length word FreeRTOS stores in front of every message. */
#define AUDIO_CAPTURE_MESSAGE_BUFFER_BYTES(frames) ((frames) * (sizeof(audio_capture_frame_t) + sizeof(size_t)))

typedef struct
{
    gpio_num_t bclk_gpio;       // INMP441 SCK
    gpio_num_t ws_gpio;         // INMP441 WS
    gpio_num_t din_gpio;        // INMP441 SD
    gpio_num_t status_led_gpio; // high while capturing; GPIO_NUM_NC to disable
    uint8_t gain_shift;         // digital gain: 0 = none, each step = +6 dB, max 8 (saturates)
} audio_capture_config_t;

typedef struct
{
    bool is_capturing;
    uint32_t frames_captured; // frames read from the microphone
    uint32_t frames_sent;     // frames accepted by the message buffer
    uint32_t frames_dropped;  // message buffer full; frame discarded
    uint32_t read_timeouts;   // full frame did not arrive in time
    uint32_t read_errors;     // other I2S read failures
    uint32_t dma_overflows;   // I2S receive DMA overwrote unread audio
    uint16_t next_sequence;
    size_t stack_free_bytes; // capture task stack high-water mark
} audio_capture_stats_t;

/*
 * Driver layer: direct control of the INMP441 over I2S.
 * Singleton, task context only. One owner calls init/start/stop/deinit.
 */

/* Validate pins and configure a stopped I2S RX channel (32-bit Philips slots, mono left). */
esp_err_t audio_capture_init(const audio_capture_config_t *config);

/* Start or stop the microphone clocks. Already started/stopped returns ESP_OK. */
esp_err_t audio_capture_start(void);
esp_err_t audio_capture_stop(void);

/*
 * Read one 20 ms frame and convert it to signed 16-bit PCM.
 * samples must hold AUDIO_SAMPLES_PER_FRAME values. timeout_ms is 1..1000.
 * samples_read always reports how many samples were filled in, even when the
 * call returns ESP_ERR_TIMEOUT with a partial frame.
 */
esp_err_t audio_capture_read_frame(int16_t *samples, size_t *samples_read, uint32_t timeout_ms);

/* Stop the task (if running) and the microphone, then release I2S. Idempotent. */
esp_err_t audio_capture_deinit(void);

/*
 * Task layer: a FreeRTOS task that captures continuously.
 * Every 20 ms it reads a frame, fills an audio_capture_frame_t and sends
 * AUDIO_CAPTURE_FRAME_HEADER_BYTES + length bytes to output_buffer without
 * waiting. If the buffer is full the frame is dropped and counted.
 * output_buffer is created and owned by the caller (the network side).
 */
esp_err_t audio_capture_task_start(MessageBufferHandle_t output_buffer);

/* Ask the task to finish its current frame and exit, then stop the microphone. */
esp_err_t audio_capture_task_stop(void);

bool audio_capture_is_capturing(void);
esp_err_t audio_capture_get_stats(audio_capture_stats_t *stats);

/* PCM byte order helpers shared with consumers of audio_capture_frame_t. */
void audio_capture_encode_pcm(const int16_t *samples, size_t sample_count, uint8_t *dest);
void audio_capture_decode_pcm(const uint8_t *src, size_t sample_count, int16_t *samples);
