#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio_common.h"
#include "driver/gpio.h"
#include "esp_err.h"

typedef struct
{
    gpio_num_t bclk_gpio;
    gpio_num_t ws_gpio;
    gpio_num_t dout_gpio;
} audio_output_config_t;

enum
{
    AUDIO_OUTPUT_MAX_WRITE_TIMEOUT_MS = 1000
};

/*
 * Singleton, task-context API. One owner must serialize ALL calls, including
 * lifecycle operations. Not callable from an ISR. No queue or volume processing.
 * init validates output-capable, distinct GPIOs, not board-level pin availability.
 * init leaves the channel stopped. Duplicate init/start/stop return INVALID_STATE.
 */
esp_err_t audio_output_init(const audio_output_config_t *config);
esp_err_t audio_output_start(void);

/*
 * Copy 1..AUDIO_SAMPLES_PER_FRAME signed mono samples to I2S, duplicating them
 * into left/right 16-bit slots. The caller may reuse its buffer on return.
 * timeout_ms must be 1..AUDIO_OUTPUT_MAX_WRITE_TIMEOUT_MS; it is one
 * overall I2S wait budget, subject to driver/RTOS timer resolution. A timeout
 * can return early because each chunk reserves time for up to two DMA waits.
 * samples_written is required and reports complete mono samples accepted even
 * on timeout/error. Retry ONLY samples + *samples_written, never the full input.
 * ESP_OK means accepted into DMA, not audibly drained. No steady-state allocation.
 * An unexpected non-timeout driver failure faults the module: deinit before reuse.
 */
esp_err_t audio_output_write(const int16_t *samples, size_t sample_count, size_t *samples_written,
                             uint32_t timeout_ms);

/* Immediate stop, not drain: pending audio can be discarded. Deinit is idempotent. */
esp_err_t audio_output_stop(void);
esp_err_t audio_output_deinit(void);
