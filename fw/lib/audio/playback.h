#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"

// Start the playback task pinned to core 1. audio_output_open must have succeeded.
// The task loops forever:
//   1. Block on xMessageBufferReceive for one audio_frame_t from `input`
//      (the message buffer is created by the network task).
//   2. audio_output_write(frame.samples, frame.length).
// No stop function, no locks. Rate-limited summary log only (style guide).
esp_err_t playback_task_start(MessageBufferHandle_t input);
