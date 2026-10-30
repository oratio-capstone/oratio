#include <assert.h>

#include "host_runtime.h"
#include "freertos/task.h"

void vTaskDelayUntil(TickType_t *wake, TickType_t interval);

/* Exercise the actual application's private snapshot check, not a copied rule.
 * app_main and the hardware-only stages are compiled but are never invoked. */
#include "../test_162_audio_playback/test_main.c"

void vTaskDelayUntil(TickType_t *wake, TickType_t interval)
{
    (void)wake;
    (void)interval;
    assert(false);
}

bool heap_caps_check_integrity_all(bool print_errors)
{
    (void)print_errors;
    assert(false);
    return false;
}

size_t heap_caps_get_free_size(uint32_t capabilities)
{
    (void)capabilities;
    assert(false);
    return 0;
}

size_t heap_caps_get_largest_free_block(uint32_t capabilities)
{
    (void)capabilities;
    assert(false);
    return 0;
}

int main(void)
{
    host_reset();
    assert(audio_playback_init(&CONFIG) == ESP_OK);
    assert(audio_playback_start() == ESP_OK);
    host_wait_write();
    audio_playback_stats_t stats;

    /* Startup can encounter a zero-progress timeout before DMA is writable. */
    host_release_write(0, ESP_ERR_TIMEOUT, 10000);
    host_wait_write();
    assert(snapshot(&stats) == ESP_OK);
    assert(stats.write_timeouts == 1 && stats.write_errors == 0);

    /* Also accept partial progress followed by successful suffix completion. */
    host_release_write(100, ESP_ERR_TIMEOUT, 10000);
    host_wait_write();
    assert(snapshot(&stats) == ESP_OK);
    host_release_write(220, ESP_OK, 10000);
    host_wait_write();
    assert(snapshot(&stats) == ESP_OK && stats.silence_frames == 1);
    assert(stats.write_timeouts == 2 && stats.write_errors == 0);

    /* A frame exhausting its deadline must still fail the application check. */
    host_release_write(0, ESP_ERR_TIMEOUT, 100000);
    host_wait_idle();
    assert(snapshot(&stats) == ESP_FAIL);
    assert(stats.state == AUDIO_PLAYBACK_FAULTED && stats.write_errors == 1);
    host_finish();
    return 0;
}
