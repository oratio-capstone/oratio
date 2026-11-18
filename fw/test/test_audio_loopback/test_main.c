// AB#171 capture + playback loopback. Checks that the capture and playback tasks connect
// through one message buffer: hold the PTT button, talk, and hear yourself on the speaker.
// S3 only: the mic uses I2S_NUM_0 and the speaker I2S_NUM_1 (the C3 has one I2S controller).
// Run: pio test -e esp32-s3-n8r8 -f test_audio_loopback
//      then pio device monitor to keep listening; the tasks run until reset.
#include "audio_capture.h"
#include "audio_common.h"
#include "audio_output.h"
#include "playback.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/task.h"
#include "unity.h"

#define BUFFER_FRAMES 4 // 80 ms of audio, same as the playback test

static const char *TAG = "TEST_LOOPBACK";

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_capture_feeds_playback(void)
{
    // Both tasks live until reboot, so this buffer must also live until reboot.
    // Each message also stores its length inside the buffer.
    MessageBufferHandle_t buffer =
        xMessageBufferCreate(BUFFER_FRAMES * (sizeof(audio_frame_t) + sizeof(configMESSAGE_BUFFER_LENGTH_TYPE)));
    TEST_ASSERT_NOT_NULL(buffer);
    TEST_ASSERT_EQUAL(ESP_OK, audio_capture_open());
    TEST_ASSERT_EQUAL(ESP_OK, audio_output_open());
    TEST_ASSERT_EQUAL(ESP_OK, playback_task_start(buffer));      // consumer
    TEST_ASSERT_EQUAL(ESP_OK, audio_capture_task_start(buffer)); // producer
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(2000)); // give the serial monitor time to connect
    UNITY_BEGIN();
    RUN_TEST(test_capture_feeds_playback);
    UNITY_END();
    ESP_LOGI(TAG, "Loopback running: hold PTT (GPIO %d) and talk", AUDIO_CAPTURE_PTT_GPIO);
}
