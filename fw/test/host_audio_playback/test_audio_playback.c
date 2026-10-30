#include "host_runtime.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "HOST_PLAYBACK_TEST";
static const audio_output_config_t CONFIG = {GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_7};

static audio_playback_stats_t stats(void)
{
    audio_playback_stats_t result;
    assert(audio_playback_get_stats(&result) == ESP_OK);
    return result;
}

static void start(void)
{
    host_reset();
    assert(audio_playback_init(&CONFIG) == ESP_OK);
    assert(audio_playback_start() == ESP_OK);
    host_wait_write();
}

static void submit_value(int16_t value)
{
    audio_frame_t frame;
    for (size_t index = 0; index < AUDIO_SAMPLES_PER_FRAME; index++)
    {
        frame.samples[index] = value;
    }
    assert(audio_playback_submit(frame.samples, AUDIO_SAMPLES_PER_FRAME) == ESP_OK);
    /* Returning destroys this stack frame: output must use a copied value. */
    memset(&frame, 0, sizeof(frame));
}

static void test_arguments_and_allocation_rollback(void)
{
    host_reset();
    const int16_t sample = 0;
    assert(audio_playback_deinit() == ESP_OK);
    assert(audio_playback_start() == ESP_ERR_INVALID_STATE);
    assert(audio_playback_stop() == ESP_ERR_INVALID_STATE);
    assert(audio_playback_submit(&sample, AUDIO_SAMPLES_PER_FRAME) == ESP_ERR_INVALID_STATE);
    assert(audio_playback_submit(NULL, 320) == ESP_ERR_INVALID_ARG);
    assert(audio_playback_submit(&sample, 1) == ESP_ERR_INVALID_ARG);
    assert(audio_playback_get_stats(NULL) == ESP_ERR_INVALID_ARG);
    assert(audio_playback_set_volume(101) == ESP_ERR_INVALID_ARG);
    assert(audio_playback_set_volume(50) == ESP_ERR_INVALID_STATE);
    assert(audio_playback_init(NULL) == ESP_ERR_INVALID_ARG);
    audio_output_config_t invalid = CONFIG;
    invalid.dout_gpio = invalid.ws_gpio;
    assert(audio_playback_init(&invalid) == ESP_ERR_INVALID_ARG);
    for (unsigned int allocation = 1; allocation <= 3; allocation++)
    {
        host_fail_allocation = allocation;
        assert(audio_playback_init(&CONFIG) == ESP_ERR_NO_MEM);
        assert(host_live_resources == 0);
        assert(stats().state == AUDIO_PLAYBACK_UNINITIALIZED);
    }
    assert(audio_playback_init(&CONFIG) == ESP_OK);
    assert(audio_playback_init(&CONFIG) == ESP_ERR_INVALID_STATE);
    assert(audio_playback_stop() == ESP_OK);
    host_finish();
}

static void test_copy_order_overflow_and_silence(void)
{
    start();
    assert(audio_playback_start() == ESP_ERR_INVALID_STATE);
    for (int16_t value = 1; value <= 5; value++)
    {
        submit_value(value);
    }
    assert(stats().queue_depth == 4 && stats().queue_high_water == 4);
    assert(stats().submitted_frames == 5 && stats().dropped_frames == 1);
    host_release_write(320, ESP_OK, 20000); /* Initial silence. */
    host_wait_write();
    for (int16_t value = 2; value <= 5; value++)
    {
        const size_t offset = host_sample_count;
        host_release_write(320, ESP_OK, 20000);
        host_wait_write();
        for (size_t index = 0; index < 320; index++)
        {
            assert(host_samples[offset + index] == value);
        }
    }
    host_release_write(320, ESP_OK, 20000); /* Queue empty again. */
    host_wait_write();
    for (size_t index = 0; index < 320; index++)
    {
        assert(host_samples[index] == 0 && host_samples[1600 + index] == 0);
    }
    assert(stats().output_frames == 4 && stats().silence_frames == 2);
    assert(stats().underflows >= 2);
    host_finish();
}

static void test_partial_writes_and_volume(void)
{
    start();
    audio_frame_t frame;
    for (size_t index = 0; index < 320; index++)
    {
        frame.samples[index] = (int16_t)((int32_t)index - 160);
    }
    frame.samples[0] = INT16_MIN;
    frame.samples[1] = INT16_MAX;
    assert(audio_playback_set_volume(50) == ESP_OK);
    assert(audio_playback_submit(frame.samples, 320) == ESP_OK);
    host_release_write(320, ESP_OK, 20000);
    host_wait_write();
    host_release_write(100, ESP_ERR_TIMEOUT, 20000);
    host_wait_write();
    host_release_write(220, ESP_OK, 20000);
    host_wait_write();
    for (size_t index = 0; index < 320; index++)
    {
        assert(host_samples[320 + index] == (int16_t)((int32_t)frame.samples[index] * 50 / 100));
    }
    assert(stats().output_frames == 1 && stats().write_timeouts == 1);
    assert(audio_playback_set_volume(0) == ESP_OK);
    submit_value(INT16_MAX);
    host_release_write(320, ESP_OK, 20000); /* Silence already in progress. */
    host_wait_write();
    const size_t offset = host_sample_count;
    host_release_write(320, ESP_ERR_TIMEOUT, 20000); /* All accepted despite timeout: no duplicate retry. */
    host_wait_write();
    for (size_t index = 0; index < 320; index++)
    {
        assert(host_samples[offset + index] == 0);
    }
    assert(stats().output_frames == 2 && stats().write_timeouts == 2);
    assert(audio_playback_set_volume(100) == ESP_OK);
    submit_value(INT16_MIN);
    host_release_write(320, ESP_OK, 20000);
    host_wait_write();
    const size_t full_volume_offset = host_sample_count;
    host_release_write(320, ESP_OK, 20000);
    host_wait_write();
    assert(host_samples[full_volume_offset] == INT16_MIN);
    host_finish();
}

static void test_timeouts_faults_and_cleanup_retry(void)
{
    start();
    host_release_write(0, ESP_ERR_TIMEOUT, 100000);
    host_wait_idle();
    assert(stats().state == AUDIO_PLAYBACK_FAULTED && stats().last_error == ESP_ERR_TIMEOUT);
    assert(stats().write_errors == 1);
    assert(audio_playback_start() == ESP_ERR_INVALID_STATE);
    host_finish();

    start();
    for (size_t attempt = 0; attempt < 16; attempt++)
    {
        host_release_write(0, ESP_ERR_TIMEOUT, 0);
    }
    host_wait_idle();
    assert(stats().write_timeouts == 16 && stats().state == AUDIO_PLAYBACK_FAULTED);
    host_finish();

    start();
    host_release_write(0, ESP_OK, 0);
    host_wait_idle();
    assert(stats().last_error == ESP_FAIL);
    host_finish();

    start();
    host_cleanup_result = ESP_FAIL;
    host_release_write(10, ESP_FAIL, 1000);
    host_wait_idle();
    assert(stats().write_errors == 1 && stats().output_frames == 0);
    assert(audio_playback_deinit() == ESP_FAIL);
    assert(host_live_resources == 3);
    host_cleanup_result = ESP_OK;
    assert(audio_playback_stop() == ESP_OK);
    assert(stats().state == AUDIO_PLAYBACK_STOPPED);
    assert(audio_playback_start() == ESP_OK);
    host_wait_write();
    host_finish();
}

static void test_start_failures(void)
{
    for (int failure = 0; failure < 2; failure++)
    {
        host_reset();
        if (failure == 0)
        {
            host_init_result = ESP_ERR_NO_MEM;
        }
        else
        {
            host_start_result = ESP_FAIL;
        }
        assert(audio_playback_init(&CONFIG) == ESP_OK);
        assert(audio_playback_start() == (failure == 0 ? ESP_ERR_NO_MEM : ESP_FAIL));
        assert(stats().state == AUDIO_PLAYBACK_FAULTED);
        host_finish();
    }
    host_reset();
    host_init_delay_ms = 1100;
    assert(audio_playback_init(&CONFIG) == ESP_OK);
    assert(audio_playback_start() == ESP_ERR_TIMEOUT);
    assert(host_live_resources == 3);
    host_wait_idle();
    assert(stats().state == AUDIO_PLAYBACK_STOPPED);
    host_finish();
}

static void test_stop_timeout_restart_and_resource_lifetime(void)
{
    start();
    submit_value(1234);
    assert(audio_playback_stop() == ESP_ERR_TIMEOUT); /* Fake driver deliberately holds the write. */
    assert(host_live_resources == 3 && stats().state == AUDIO_PLAYBACK_STOPPING);
    audio_frame_t frame = {0};
    assert(audio_playback_submit(frame.samples, 320) == ESP_ERR_INVALID_STATE);
    host_release_write(100, ESP_ERR_TIMEOUT, 20000);
    host_wait_idle();
    assert(audio_playback_stop() == ESP_OK);
    assert(stats().queue_depth == 0 && stats().dropped_frames == 1);
    assert(audio_playback_start() == ESP_OK);
    host_wait_write();
    const size_t offset = host_sample_count;
    host_release_write(320, ESP_OK, 20000);
    host_wait_write();
    for (size_t index = 0; index < 320; index++)
    {
        assert(host_samples[offset + index] == 0);
    }
    host_finish();
    assert(audio_playback_deinit() == ESP_OK);
}

static atomic_uint s_producers_ready;
static atomic_bool s_shutdown_complete;

static void *producer(void *argument)
{
    (void)argument;
    audio_frame_t frame = {0};
    assert(audio_playback_submit(frame.samples, 320) == ESP_OK);
    (void)atomic_fetch_add(&s_producers_ready, 1);
    unsigned int after_shutdown = 0;
    while (after_shutdown < 100)
    {
        const esp_err_t result = audio_playback_submit(frame.samples, 320);
        assert(result == ESP_OK || result == ESP_ERR_INVALID_STATE);
        audio_playback_stats_t snapshot;
        assert(audio_playback_get_stats(&snapshot) == ESP_OK);
        assert(snapshot.queue_depth <= 4);
        const esp_err_t volume_result = audio_playback_set_volume(25);
        assert(volume_result == ESP_OK || volume_result == ESP_ERR_INVALID_STATE);
        if (atomic_load(&s_shutdown_complete))
        {
            after_shutdown++;
        }
    }
    return NULL;
}

static void test_concurrent_producers_and_shutdown(void)
{
    start();
    atomic_store(&s_producers_ready, 0);
    atomic_store(&s_shutdown_complete, false);
    pthread_t first;
    pthread_t second;
    assert(pthread_create(&first, NULL, producer, NULL) == 0);
    assert(pthread_create(&second, NULL, producer, NULL) == 0);
    while (atomic_load(&s_producers_ready) != 2)
    {
        vTaskDelay(1);
    }
    host_finish();
    atomic_store(&s_shutdown_complete, true);
    assert(pthread_join(first, NULL) == 0);
    assert(pthread_join(second, NULL) == 0);
    assert(stats().state == AUDIO_PLAYBACK_UNINITIALIZED && stats().queue_depth == 0);
}

int main(void)
{
    ESP_LOGI(TAG, "Running threaded playback contract tests");
    test_arguments_and_allocation_rollback();
    test_copy_order_overflow_and_silence();
    test_partial_writes_and_volume();
    test_timeouts_faults_and_cleanup_retry();
    test_start_failures();
    test_stop_timeout_restart_and_resource_lifetime();
    test_concurrent_producers_and_shutdown();
    assert(host_live_resources == 0);
    return 0;
}
