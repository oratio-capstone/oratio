#pragma once

#include "audio_playback.h"

/* Test controls used only while the worker is idle or blocked at the write gate. */
extern unsigned int host_fail_allocation;
extern unsigned int host_live_resources;
extern esp_err_t host_init_result;
extern esp_err_t host_start_result;
extern esp_err_t host_cleanup_result;
extern uint32_t host_init_delay_ms;
extern size_t host_output_calls;
extern size_t host_sample_count;
extern int16_t host_samples[32000];

void host_reset(void);
void host_wait_write(void);
void host_release_write(size_t count, esp_err_t result, int64_t elapsed_us);
void host_wait_idle(void);
void host_finish(void);
