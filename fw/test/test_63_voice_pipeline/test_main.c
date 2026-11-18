/* AB#63: real microphone -> node -> root -> gateway -> root -> speaker node.
 * This is a manual hardware integration test, not a Unity assertion suite.
 */
#include "audio_capture.h"
#include "audio_output.h"
#include "esp_log.h"
#include "network.h"
#include "playback.h"
#include "storage.h"

#ifndef APP_IS_ROOT
#error "Select a root_test or node_test environment"
#endif
#ifndef TEST_IS_PRODUCER
#error "Define TEST_IS_PRODUCER as 0 (speaker) or 1 (microphone)"
#endif
#if (TEST_IS_PRODUCER != 0) && (TEST_IS_PRODUCER != 1)
#error "TEST_IS_PRODUCER must be 0 or 1"
#endif

static const char *TAG = "TEST_63";
static MessageBufferHandle_t s_capture_buffer;
static MessageBufferHandle_t s_playback_buffer;

/* Allocate only the selected endpoint's buffer and open its existing driver.
 * A root has no audio endpoint: both NULL handles select heartbeat-only service.
 */
static esp_err_t voice_endpoint_open(void)
{
#if APP_IS_ROOT
    return ESP_OK;
#elif TEST_IS_PRODUCER
    s_capture_buffer = xMessageBufferCreate(NETWORK_CAPTURE_BUFFER_BYTES);
    if (s_capture_buffer == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    return audio_capture_open();
#else
    s_playback_buffer = xMessageBufferCreate(NETWORK_PLAYBACK_BUFFER_BYTES);
    if (s_playback_buffer == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    return audio_output_open();
#endif
}

/* Only call before the network task owns the buffers; workers have no stop API. */
static void voice_endpoint_close(void)
{
#if !APP_IS_ROOT && TEST_IS_PRODUCER
    (void)audio_capture_close();
#elif !APP_IS_ROOT
    (void)audio_output_close();
#endif
    if (s_capture_buffer != NULL)
    {
        vMessageBufferDelete(s_capture_buffer);
        s_capture_buffer = NULL;
    }
    if (s_playback_buffer != NULL)
    {
        vMessageBufferDelete(s_playback_buffer);
        s_playback_buffer = NULL;
    }
}

/* Reuse the production workers: no synthetic frames or extra buffer readers.
 * Capture supplies sequence and sample length; networking serializes the frame.
 */
static esp_err_t voice_endpoint_start(void)
{
#if APP_IS_ROOT
    ESP_LOGI(TAG, "ROOT: mesh relay and gateway connection; no local audio");
    return ESP_OK;
#elif TEST_IS_PRODUCER
    const esp_err_t result = audio_capture_task_start(s_capture_buffer);
    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "MICROPHONE: wait for both nodes in the gateway registry, then hold PTT to speak");
    }
    return result;
#else
    const esp_err_t result = playback_task_start(s_playback_buffer);
    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "SPEAKER: playback worker started; waiting for forwarded audio");
    }
    return result;
#endif
}

void app_main(void)
{
    /* Initialize shared persistence before Wi-Fi, then prepare the audio endpoint. */
    esp_err_t result = storage_init();
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Storage initialization failed: %s", esp_err_to_name(result));
        return;
    }
    result = voice_endpoint_open();
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Audio setup failed: %s", esp_err_to_name(result));
        voice_endpoint_close();
        return;
    }

    /* Mesh startup is asynchronous. Connection logs and the gateway registry,
     * rather than this return value, establish readiness for a manual voice run.
     */
    result = mesh_network_init();
    if (result == ESP_OK)
    {
        result = network_task_start(s_capture_buffer, s_playback_buffer);
    }
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Network startup failed: %s", esp_err_to_name(result));
        voice_endpoint_close();
        return;
    }

    /* From here the network owns access to these buffers for its lifetime.
     * Keep them alive even if an audio worker cannot start; reset to retry.
     */
    result = voice_endpoint_start();
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Audio worker failed: %s; reset after correcting the fault", esp_err_to_name(result));
        return;
    }
    ESP_LOGI(TAG, "Pipeline started: %u Hz, %u samples/frame, %u PCM bytes/frame", (unsigned int)AUDIO_SAMPLE_RATE_HZ,
             (unsigned int)AUDIO_SAMPLES_PER_FRAME, (unsigned int)(AUDIO_SAMPLES_PER_FRAME * sizeof(int16_t)));
    /* app_main may return: capture, network and playback continue in their tasks. */
}
