/* AB#63: two-way voice through node -> root -> gateway -> root -> node.
 * Flash root_test to the relay and the same node_test build to both endpoints.
 * Each endpoint needs a microphone, PTT button, amplifier and speaker.
 * TEST_IS_PRODUCER is ignored: every non-root node captures and plays audio.
 * Wait for both nodes to register at the gateway, then take turns holding PTT.
 * Playback remains active during capture; no muting or echo cancellation is added.
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
#if (APP_IS_ROOT != 0) && (APP_IS_ROOT != 1)
#error "APP_IS_ROOT must be 0 or 1"
#endif

static const char *TAG = "TEST_63";
static MessageBufferHandle_t s_capture_buffer;
static MessageBufferHandle_t s_playback_buffer;

/* Workers have both audio paths. The root has neither: its NULL buffer handles
 * select heartbeat-only application traffic while Mesh-Lite forwards node traffic.
 */
static esp_err_t voice_endpoint_open(void)
{
#if APP_IS_ROOT
    return ESP_OK;
#else
    s_capture_buffer = xMessageBufferCreate(NETWORK_CAPTURE_BUFFER_BYTES);
    s_playback_buffer = xMessageBufferCreate(NETWORK_PLAYBACK_BUFFER_BYTES);
    if (s_capture_buffer == NULL || s_playback_buffer == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = audio_capture_open();
    if (result != ESP_OK)
    {
        return result;
    }
    return audio_output_open();
#endif
}

/* Only call before the network task owns the buffers; workers have no stop API. */
static void voice_endpoint_close(void)
{
#if !APP_IS_ROOT
    (void)audio_capture_close();
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

/* Start playback first so incoming frames can drain before capture begins.
 * Each buffer still has exactly one writer and one reader; no audio copies or
 * application-level producer/consumer switching are needed in this test.
 */
static esp_err_t voice_endpoint_start(void)
{
#if APP_IS_ROOT
    ESP_LOGI(TAG, "ROOT: mesh relay and gateway connection; no local audio");
    return ESP_OK;
#else
    esp_err_t result = playback_task_start(s_playback_buffer);
    if (result != ESP_OK)
    {
        return result;
    }

    result = audio_capture_task_start(s_capture_buffer);
    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "VOICE NODE: capture and playback started; hold PTT to send, incoming audio plays automatically");
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
     * Keep them alive if either audio worker fails to start: playback may already
     * be running when capture startup fails. There is no worker stop API; reset to retry.
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
