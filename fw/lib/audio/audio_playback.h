#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio_common.h"
#include "audio_output.h"

enum
{
    AUDIO_PLAYBACK_QUEUE_FRAMES = 4,
    AUDIO_PLAYBACK_CONTROL_TIMEOUT_MS = 1000
};

typedef enum
{
    AUDIO_PLAYBACK_UNINITIALIZED,
    AUDIO_PLAYBACK_STOPPED,
    AUDIO_PLAYBACK_STARTING,
    AUDIO_PLAYBACK_RUNNING,
    AUDIO_PLAYBACK_STOPPING,
    AUDIO_PLAYBACK_FAULTED
} audio_playback_state_t;

typedef struct
{
    audio_playback_state_t state;
    esp_err_t last_error;
    uint32_t submitted_frames;
    uint32_t output_frames;
    uint32_t silence_frames;
    uint32_t dropped_frames;
    uint32_t underflows;
    uint32_t write_timeouts;
    uint32_t write_errors;
    size_t queue_depth;
    size_t queue_high_water;
    uint8_t volume_percent;
} audio_playback_stats_t;

/* Singleton, task-context only. One supervisor serializes init/start/stop/deinit.
 * Producers may submit/read stats/set volume concurrently with that supervisor.
 * The service exclusively owns audio_output until successful deinit; no direct
 * output calls or other I2S owners are permitted during this interval.
 * Init allocates the queue/task once. No allocation during steady playback.
 */
esp_err_t audio_playback_init(const audio_output_config_t *config);
esp_err_t audio_playback_start(void);

/* Copy exactly one PCM frame. No waiting for queue space. Full queue discards
 * its oldest queued frame, then accepts this one. ESP_OK means queued, not played.
 * Buffer must remain valid/unchanged during the call and may be reused on return.
 * Short internal critical sections serialize queue and lifecycle access.
 */
esp_err_t audio_playback_submit(const int16_t *samples, size_t sample_count);
esp_err_t audio_playback_set_volume(uint8_t percent);
esp_err_t audio_playback_get_stats(audio_playback_stats_t *stats);

/* Stop rejects new frames immediately, waits for worker cleanup, then discards
 * queued audio. It is not a drain. Already stopped is OK. Control waits are
 * bounded to 1000 ms (subject to scheduling); timeout retains all resources.
 * Retry stop/deinit after timeout or cleanup failure. Deinit is idempotent.
 * Fault recovery requires successful stop before start, or deinit/init.
 */
esp_err_t audio_playback_stop(void);
esp_err_t audio_playback_deinit(void);
