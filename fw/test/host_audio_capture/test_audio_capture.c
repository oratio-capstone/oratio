/*
 * Host contract tests for lib/audio/audio_capture.c.
 *
 * The real driver source is compiled on the PC against small fake I2S, GPIO and
 * FreeRTOS doubles (stubs/). It is #included so tests can drive the private
 * capture_one_frame() step directly. These tests check software behaviour
 * (validation, state, conversion, framing, sequencing, error counting), not the
 * electrical microphone path: that is test/test_157_audio_capture on hardware.
 *
 * Run: python tools/test_audio_capture.py
 */
#include "audio_capture.c"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- Tiny test framework ---- */

static int s_failures = 0;
static int s_checks = 0;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        s_checks++;                                                                                                    \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            s_failures++;                                                                                              \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                              \
        }                                                                                                              \
    } while (0)

/* ---- Fakes ---- */

struct i2s_channel_obj_t
{
    int unused;
};
struct tskTaskControlBlock
{
    int unused;
};
struct MessageBufferDef_t
{
    int unused;
};

static struct i2s_channel_obj_t s_fake_channel;
static struct tskTaskControlBlock s_fake_task;
static struct MessageBufferDef_t s_fake_buffer_obj;
static MessageBufferHandle_t const FAKE_BUFFER = &s_fake_buffer_obj;

static esp_err_t s_fake_new_result, s_fake_init_result, s_fake_callback_result, s_fake_enable_result;
static esp_err_t s_fake_disable_result, s_fake_del_result, s_fake_read_result;
static int s_fake_new_calls, s_fake_del_calls, s_fake_enable_calls, s_fake_disable_calls, s_fake_read_calls;
static i2s_chan_config_t s_fake_chan_cfg;
static i2s_std_config_t s_fake_std_cfg;
static i2s_isr_callback_t s_fake_overflow_cb;
static uint32_t s_fake_read_timeout_ms;
static int32_t s_fake_slots[AUDIO_SAMPLES_PER_FRAME];
static size_t s_fake_read_bytes;

static int s_fake_gpio_level[GPIO_NUM_MAX];
static int s_fake_gpio_reset_calls;

static size_t s_fake_buffer_space;
static uint8_t s_fake_sent[sizeof(audio_capture_frame_t)];
static size_t s_fake_sent_len;
static int s_fake_send_calls;

static BaseType_t s_fake_task_create_result;
static int s_fake_task_create_calls;
static configSTACK_DEPTH_TYPE s_fake_task_stack;
static UBaseType_t s_fake_task_priority;
static bool s_fake_task_exits_on_delay;
static int s_fake_delay_calls;
static TickType_t s_fake_last_delay;

const char *esp_err_to_name(esp_err_t code)
{
    (void)code;
    return "ERR";
}

void host_log(const char *tag, const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    (void)tag;
}

esp_err_t gpio_reset_pin(gpio_num_t gpio_num)
{
    s_fake_gpio_reset_calls++;
    s_fake_gpio_level[gpio_num] = -1;
    return ESP_OK;
}

esp_err_t gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode)
{
    (void)gpio_num;
    (void)mode;
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    s_fake_gpio_level[gpio_num] = (int)level;
    return ESP_OK;
}

esp_err_t i2s_new_channel(const i2s_chan_config_t *chan_cfg, i2s_chan_handle_t *ret_tx_handle,
                          i2s_chan_handle_t *ret_rx_handle)
{
    s_fake_new_calls++;
    s_fake_chan_cfg = *chan_cfg;
    if (ret_tx_handle != NULL)
    {
        return ESP_ERR_INVALID_ARG; // capture must only ask for RX
    }
    if (s_fake_new_result == ESP_OK)
    {
        *ret_rx_handle = &s_fake_channel;
    }
    return s_fake_new_result;
}

esp_err_t i2s_del_channel(i2s_chan_handle_t handle)
{
    (void)handle;
    s_fake_del_calls++;
    return s_fake_del_result;
}

esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *std_cfg)
{
    (void)handle;
    s_fake_std_cfg = *std_cfg;
    return s_fake_init_result;
}

esp_err_t i2s_channel_register_event_callback(i2s_chan_handle_t handle, const i2s_event_callbacks_t *callbacks,
                                              void *user_data)
{
    (void)handle;
    (void)user_data;
    s_fake_overflow_cb = callbacks->on_recv_q_ovf;
    return s_fake_callback_result;
}

esp_err_t i2s_channel_enable(i2s_chan_handle_t handle)
{
    (void)handle;
    s_fake_enable_calls++;
    return s_fake_enable_result;
}

esp_err_t i2s_channel_disable(i2s_chan_handle_t handle)
{
    (void)handle;
    s_fake_disable_calls++;
    return s_fake_disable_result;
}

esp_err_t i2s_channel_read(i2s_chan_handle_t handle, void *dest, size_t size, size_t *bytes_read, uint32_t timeout_ms)
{
    (void)handle;
    s_fake_read_calls++;
    s_fake_read_timeout_ms = timeout_ms;
    const size_t count = (s_fake_read_bytes < size) ? s_fake_read_bytes : size;
    memcpy(dest, s_fake_slots, count);
    *bytes_read = count;
    return s_fake_read_result;
}

size_t xMessageBufferSend(MessageBufferHandle_t buffer, const void *data, size_t length, TickType_t ticks_to_wait)
{
    (void)buffer;
    s_fake_send_calls++;
    if (ticks_to_wait != 0 || length > s_fake_buffer_space || length > sizeof(s_fake_sent))
    {
        return 0; // capture must never wait; a full buffer accepts nothing
    }
    memcpy(s_fake_sent, data, length);
    s_fake_sent_len = length;
    return length;
}

BaseType_t xTaskCreate(TaskFunction_t task, const char *name, configSTACK_DEPTH_TYPE stack_depth, void *arg,
                       UBaseType_t priority, TaskHandle_t *created_task)
{
    (void)task;
    (void)name;
    (void)arg;
    s_fake_task_create_calls++;
    s_fake_task_stack = stack_depth;
    s_fake_task_priority = priority;
    if (s_fake_task_create_result == pdPASS)
    {
        *created_task = &s_fake_task;
    }
    return s_fake_task_create_result;
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
}

void vTaskDelay(TickType_t ticks)
{
    s_fake_delay_calls++;
    s_fake_last_delay = ticks;
    if (s_fake_task_exits_on_delay && !s_task_should_run)
    {
        s_task_active = false; // simulate the capture task noticing the stop request and exiting
    }
}

TickType_t xTaskGetTickCount(void)
{
    return 0;
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    (void)task;
    return 1234;
}

/* ---- Helpers ---- */

static const audio_capture_config_t VALID_CONFIG = {
    .bclk_gpio = (gpio_num_t)0,
    .ws_gpio = (gpio_num_t)1,
    .din_gpio = (gpio_num_t)6,
    .status_led_gpio = (gpio_num_t)3,
    .gain_shift = 0,
};

static void reset_all(void)
{
    // Module state (visible because the source is included)
    s_rx_channel = NULL;
    memset(&s_config, 0, sizeof(s_config));
    s_is_initialized = false;
    s_is_running = false;
    s_dma_overflows = 0;
    s_task_handle = NULL;
    s_output_buffer = NULL;
    s_task_should_run = false;
    s_task_active = false;
    s_last_read_result = ESP_OK;
    memset(&s_stats, 0, sizeof(s_stats));
    s_next_sequence = 0;

    // Fakes
    s_fake_new_result = s_fake_init_result = s_fake_callback_result = ESP_OK;
    s_fake_enable_result = s_fake_disable_result = s_fake_del_result = s_fake_read_result = ESP_OK;
    s_fake_new_calls = s_fake_del_calls = s_fake_enable_calls = s_fake_disable_calls = s_fake_read_calls = 0;
    memset(&s_fake_chan_cfg, 0, sizeof(s_fake_chan_cfg));
    memset(&s_fake_std_cfg, 0, sizeof(s_fake_std_cfg));
    s_fake_overflow_cb = NULL;
    s_fake_read_timeout_ms = 0;
    memset(s_fake_slots, 0, sizeof(s_fake_slots));
    s_fake_read_bytes = sizeof(s_fake_slots);
    memset(s_fake_gpio_level, 0, sizeof(s_fake_gpio_level));
    s_fake_gpio_reset_calls = 0;
    s_fake_buffer_space = sizeof(audio_capture_frame_t);
    memset(s_fake_sent, 0, sizeof(s_fake_sent));
    s_fake_sent_len = 0;
    s_fake_send_calls = 0;
    s_fake_task_create_result = pdPASS;
    s_fake_task_create_calls = 0;
    s_fake_task_stack = 0;
    s_fake_task_priority = 0;
    s_fake_task_exits_on_delay = true;
    s_fake_delay_calls = 0;
    s_fake_last_delay = 0;
}

static void init_and_start(void)
{
    CHECK(audio_capture_init(&VALID_CONFIG) == ESP_OK);
    CHECK(audio_capture_start() == ESP_OK);
}

static audio_capture_frame_t last_sent_frame(void)
{
    audio_capture_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    memcpy(&frame, s_fake_sent, s_fake_sent_len);
    return frame;
}

/* ---- Tests ---- */

static void test_init_rejects_bad_config(void)
{
    reset_all();
    audio_capture_config_t config = VALID_CONFIG;
    CHECK(audio_capture_init(NULL) == ESP_ERR_INVALID_ARG);

    config.ws_gpio = config.bclk_gpio; // duplicate pins
    CHECK(audio_capture_init(&config) == ESP_ERR_INVALID_ARG);

    config = VALID_CONFIG;
    config.din_gpio = (gpio_num_t)30; // not a C3 GPIO
    CHECK(audio_capture_init(&config) == ESP_ERR_INVALID_ARG);

    config = VALID_CONFIG;
    config.gain_shift = AUDIO_CAPTURE_MAX_GAIN_SHIFT + 1;
    CHECK(audio_capture_init(&config) == ESP_ERR_INVALID_ARG);

    config = VALID_CONFIG;
    config.status_led_gpio = config.din_gpio; // LED on a microphone pin
    CHECK(audio_capture_init(&config) == ESP_ERR_INVALID_ARG);

    CHECK(s_fake_new_calls == 0); // nothing allocated for bad input
    CHECK(!s_is_initialized);
}

static void test_init_configures_i2s(void)
{
    reset_all();
    CHECK(audio_capture_init(&VALID_CONFIG) == ESP_OK);
    CHECK(s_fake_chan_cfg.id == I2S_NUM_0);
    CHECK(s_fake_chan_cfg.role == I2S_ROLE_MASTER);
    CHECK(s_fake_chan_cfg.dma_frame_num == AUDIO_SAMPLES_PER_FRAME);
    CHECK(s_fake_chan_cfg.dma_desc_num == 4);
    CHECK(s_fake_std_cfg.clk_cfg.sample_rate_hz == 16000);
    CHECK(s_fake_std_cfg.slot_cfg.data_bit_width == I2S_DATA_BIT_WIDTH_32BIT);
    CHECK(s_fake_std_cfg.slot_cfg.slot_mode == I2S_SLOT_MODE_MONO);
    CHECK(s_fake_std_cfg.slot_cfg.slot_mask == I2S_STD_SLOT_LEFT);
    CHECK(s_fake_std_cfg.gpio_cfg.bclk == VALID_CONFIG.bclk_gpio);
    CHECK(s_fake_std_cfg.gpio_cfg.ws == VALID_CONFIG.ws_gpio);
    CHECK(s_fake_std_cfg.gpio_cfg.din == VALID_CONFIG.din_gpio);
    CHECK(s_fake_std_cfg.gpio_cfg.mclk == I2S_GPIO_UNUSED);
    CHECK(s_fake_std_cfg.gpio_cfg.dout == I2S_GPIO_UNUSED);
    CHECK(s_fake_overflow_cb != NULL);
    CHECK(s_fake_gpio_level[3] == 0); // status LED starts off
    CHECK(s_fake_enable_calls == 0);  // init leaves the microphone stopped
}

static void test_lifecycle_states(void)
{
    reset_all();
    CHECK(audio_capture_start() == ESP_ERR_INVALID_STATE);
    CHECK(audio_capture_stop() == ESP_ERR_INVALID_STATE);
    CHECK(audio_capture_deinit() == ESP_OK); // idempotent when never initialized

    CHECK(audio_capture_init(&VALID_CONFIG) == ESP_OK);
    CHECK(audio_capture_init(&VALID_CONFIG) == ESP_ERR_INVALID_STATE);

    int16_t samples[AUDIO_SAMPLES_PER_FRAME];
    size_t samples_read = 99;
    CHECK(audio_capture_read_frame(samples, &samples_read, 100) == ESP_ERR_INVALID_STATE);
    CHECK(samples_read == 0);

    CHECK(audio_capture_start() == ESP_OK);
    CHECK(audio_capture_start() == ESP_OK);
    CHECK(s_fake_enable_calls == 1);
    CHECK(audio_capture_stop() == ESP_OK);
    CHECK(audio_capture_stop() == ESP_OK);
    CHECK(s_fake_disable_calls == 1);

    CHECK(audio_capture_start() == ESP_OK);
    CHECK(audio_capture_deinit() == ESP_OK);
    CHECK(s_fake_disable_calls == 2);
    CHECK(s_fake_del_calls == 1);
    CHECK(s_fake_gpio_reset_calls == 2); // LED claimed at init, released at deinit
    CHECK(!s_is_initialized);
    CHECK(audio_capture_deinit() == ESP_OK);
    CHECK(s_fake_del_calls == 1);
}

static void test_init_failure_cleans_up(void)
{
    reset_all();
    s_fake_init_result = ESP_FAIL;
    CHECK(audio_capture_init(&VALID_CONFIG) == ESP_FAIL);
    CHECK(s_fake_del_calls == 1);
    CHECK(s_rx_channel == NULL);
    CHECK(!s_is_initialized);

    s_fake_init_result = ESP_OK;
    s_fake_callback_result = ESP_FAIL;
    CHECK(audio_capture_init(&VALID_CONFIG) == ESP_FAIL);
    CHECK(s_fake_del_calls == 2);

    s_fake_callback_result = ESP_OK;
    CHECK(audio_capture_init(&VALID_CONFIG) == ESP_OK); // recovers after a failed attempt
}

static void test_read_rejects_bad_arguments(void)
{
    reset_all();
    init_and_start();
    int16_t samples[AUDIO_SAMPLES_PER_FRAME];
    size_t samples_read = 99;
    CHECK(audio_capture_read_frame(NULL, &samples_read, 100) == ESP_ERR_INVALID_ARG);
    CHECK(samples_read == 0);
    CHECK(audio_capture_read_frame(samples, NULL, 100) == ESP_ERR_INVALID_ARG);
    CHECK(audio_capture_read_frame(samples, &samples_read, 0) == ESP_ERR_INVALID_ARG);
    CHECK(audio_capture_read_frame(samples, &samples_read, AUDIO_CAPTURE_MAX_READ_TIMEOUT_MS + 1) ==
          ESP_ERR_INVALID_ARG);
    CHECK(s_fake_read_calls == 0);
}

static void test_read_converts_slots_to_pcm16(void)
{
    reset_all();
    init_and_start();
    s_fake_slots[0] = 0x7FFF0000;          // loudest positive
    s_fake_slots[1] = (int32_t)0x80000000; // loudest negative
    s_fake_slots[2] = 0x00010000;          // +1
    s_fake_slots[3] = (int32_t)0xFFFF0000; // -1
    s_fake_slots[4] = 0x0000FFFF;          // below 16-bit resolution
    s_fake_slots[5] = 0x12345678;

    int16_t samples[AUDIO_SAMPLES_PER_FRAME];
    size_t samples_read = 0;
    CHECK(audio_capture_read_frame(samples, &samples_read, 100) == ESP_OK);
    CHECK(samples_read == AUDIO_SAMPLES_PER_FRAME);
    CHECK(s_fake_read_timeout_ms == 100); // passed through as milliseconds
    CHECK(samples[0] == 32767);
    CHECK(samples[1] == -32768);
    CHECK(samples[2] == 1);
    CHECK(samples[3] == -1);
    CHECK(samples[4] == 0);
    CHECK(samples[5] == 0x1234);
}

static void test_gain_boosts_and_saturates(void)
{
    reset_all();
    audio_capture_config_t config = VALID_CONFIG;
    config.gain_shift = 2; // +12 dB
    CHECK(audio_capture_init(&config) == ESP_OK);
    CHECK(audio_capture_start() == ESP_OK);
    s_fake_slots[0] = 0x00010000;          // 1 -> 4
    s_fake_slots[1] = 0x10000000;          // 4096 -> 16384
    s_fake_slots[2] = 0x7FFF0000;          // saturates high
    s_fake_slots[3] = (int32_t)0x80000000; // saturates low
    s_fake_slots[4] = (int32_t)0xF0000000; // -4096 -> -16384

    int16_t samples[AUDIO_SAMPLES_PER_FRAME];
    size_t samples_read = 0;
    CHECK(audio_capture_read_frame(samples, &samples_read, 100) == ESP_OK);
    CHECK(samples[0] == 4);
    CHECK(samples[1] == 16384);
    CHECK(samples[2] == INT16_MAX);
    CHECK(samples[3] == INT16_MIN);
    CHECK(samples[4] == -16384);
}

static void test_read_timeout_returns_partial_frame(void)
{
    reset_all();
    init_and_start();
    s_fake_read_bytes = 160 * sizeof(int32_t);
    s_fake_read_result = ESP_ERR_TIMEOUT;
    int16_t samples[AUDIO_SAMPLES_PER_FRAME];
    size_t samples_read = 0;
    CHECK(audio_capture_read_frame(samples, &samples_read, 100) == ESP_ERR_TIMEOUT);
    CHECK(samples_read == 160);
}

static void test_pcm_byte_order(void)
{
    const int16_t samples[] = {0x1234, -2, INT16_MIN, INT16_MAX};
    uint8_t bytes[sizeof(samples)];
    audio_capture_encode_pcm(samples, 4, bytes);
    const uint8_t expected[] = {0x34, 0x12, 0xFE, 0xFF, 0x00, 0x80, 0xFF, 0x7F}; // low byte first
    CHECK(memcmp(bytes, expected, sizeof(expected)) == 0);

    int16_t decoded[4];
    audio_capture_decode_pcm(bytes, 4, decoded);
    CHECK(memcmp(decoded, samples, sizeof(samples)) == 0);
}

static void test_frame_layout_matches_protocol(void)
{
    // sequence, length, payload: the last three Oratio packet fields
    CHECK(offsetof(audio_capture_frame_t, sequence) == 0);
    CHECK(offsetof(audio_capture_frame_t, length) == 2);
    CHECK(offsetof(audio_capture_frame_t, payload) == 4);
    CHECK(sizeof(audio_capture_frame_t) == 644);
    CHECK(AUDIO_CAPTURE_MESSAGE_BUFFER_BYTES(4) == 4 * (644 + sizeof(size_t)));
}

static void test_capture_sends_full_frame(void)
{
    reset_all();
    init_and_start();
    s_output_buffer = FAKE_BUFFER;
    for (int i = 0; i < AUDIO_SAMPLES_PER_FRAME; i++)
    {
        s_fake_slots[i] = (int32_t)((uint32_t)(i - 160) << 16); // samples -160..159
    }

    capture_one_frame();
    CHECK(s_fake_sent_len == AUDIO_CAPTURE_FRAME_HEADER_BYTES + AUDIO_FRAME_BYTES); // 644 bytes
    audio_capture_frame_t frame = last_sent_frame();
    CHECK(frame.sequence == 0);
    CHECK(frame.length == AUDIO_FRAME_BYTES);

    int16_t decoded[AUDIO_SAMPLES_PER_FRAME];
    audio_capture_decode_pcm(frame.payload, AUDIO_SAMPLES_PER_FRAME, decoded);
    CHECK(decoded[0] == -160);
    CHECK(decoded[160] == 0);
    CHECK(decoded[319] == 159);

    capture_one_frame();
    CHECK(last_sent_frame().sequence == 1);

    audio_capture_stats_t stats;
    CHECK(audio_capture_get_stats(&stats) == ESP_OK);
    CHECK(stats.frames_captured == 2);
    CHECK(stats.frames_sent == 2);
    CHECK(stats.frames_dropped == 0);
    CHECK(stats.next_sequence == 2);
    CHECK(s_fake_read_timeout_ms == 100);
    CHECK(s_stats_lock.locked == 0); // every critical section was exited
}

static void test_full_buffer_drops_but_sequence_advances(void)
{
    reset_all();
    init_and_start();
    s_output_buffer = FAKE_BUFFER;
    s_fake_buffer_space = 0; // network side not reading

    capture_one_frame(); // sequence 0 is dropped
    audio_capture_stats_t stats;
    audio_capture_get_stats(&stats);
    CHECK(stats.frames_dropped == 1);
    CHECK(stats.frames_sent == 0);
    CHECK(s_fake_send_calls == 1); // tried once, did not wait or retry

    s_fake_buffer_space = sizeof(audio_capture_frame_t);
    capture_one_frame();
    CHECK(last_sent_frame().sequence == 1); // receiver sees the gap at 0
}

static void test_timeout_and_error_counting(void)
{
    reset_all();
    init_and_start();
    s_output_buffer = FAKE_BUFFER;

    s_fake_read_result = ESP_ERR_TIMEOUT;
    s_fake_read_bytes = 160 * sizeof(int32_t);
    capture_one_frame();
    CHECK(s_fake_sent_len == AUDIO_CAPTURE_FRAME_HEADER_BYTES + 320); // partial frame still sent
    CHECK(last_sent_frame().length == 320);

    s_fake_read_result = ESP_FAIL;
    s_fake_read_bytes = 0;
    const int sends_before = s_fake_send_calls;
    capture_one_frame();
    capture_one_frame();
    CHECK(s_fake_send_calls == sends_before); // nothing to send
    CHECK(s_fake_delay_calls == 2);           // backs off instead of spinning
    CHECK(s_fake_last_delay == pdMS_TO_TICKS(20));

    audio_capture_stats_t stats;
    audio_capture_get_stats(&stats);
    CHECK(stats.read_timeouts == 1);
    CHECK(stats.read_errors == 2);
    CHECK(s_last_read_result == ESP_FAIL);
}

static void test_sequence_wraps(void)
{
    reset_all();
    init_and_start();
    s_output_buffer = FAKE_BUFFER;
    s_next_sequence = 0xFFFF;
    capture_one_frame();
    CHECK(last_sent_frame().sequence == 0xFFFF);
    capture_one_frame();
    CHECK(last_sent_frame().sequence == 0x0000);
}

static void test_task_start_and_stop(void)
{
    reset_all();
    CHECK(audio_capture_task_start(FAKE_BUFFER) == ESP_ERR_INVALID_STATE); // not initialized
    CHECK(audio_capture_init(&VALID_CONFIG) == ESP_OK);
    CHECK(audio_capture_task_start(NULL) == ESP_ERR_INVALID_ARG);

    CHECK(audio_capture_task_start(FAKE_BUFFER) == ESP_OK);
    CHECK(s_fake_enable_calls == 1);
    CHECK(s_fake_task_create_calls == 1);
    CHECK(s_fake_task_stack == 4096);
    CHECK(s_fake_task_priority == 5);
    CHECK(s_fake_gpio_level[3] == 1); // status LED on
    CHECK(audio_capture_is_capturing());
    CHECK(audio_capture_task_start(FAKE_BUFFER) == ESP_ERR_INVALID_STATE); // already running
    CHECK(audio_capture_stop() == ESP_ERR_INVALID_STATE);                  // task owns the microphone

    CHECK(audio_capture_task_stop() == ESP_OK);
    CHECK(!audio_capture_is_capturing());
    CHECK(s_fake_gpio_level[3] == 0);
    CHECK(s_fake_disable_calls == 1);
    CHECK(audio_capture_task_stop() == ESP_OK); // already stopped

    audio_capture_stats_t stats;
    audio_capture_get_stats(&stats);
    CHECK(!stats.is_capturing);
}

static void test_task_create_failure_rolls_back(void)
{
    reset_all();
    CHECK(audio_capture_init(&VALID_CONFIG) == ESP_OK);
    s_fake_task_create_result = pdFAIL;
    CHECK(audio_capture_task_start(FAKE_BUFFER) == ESP_ERR_NO_MEM);
    CHECK(!s_task_active);
    CHECK(!s_is_running);
    CHECK(s_fake_disable_calls == 1);
    CHECK(s_fake_gpio_level[3] == 0);
}

static void test_task_stop_timeout_keeps_resources(void)
{
    reset_all();
    CHECK(audio_capture_init(&VALID_CONFIG) == ESP_OK);
    CHECK(audio_capture_task_start(FAKE_BUFFER) == ESP_OK);
    s_fake_task_exits_on_delay = false; // task is stuck
    CHECK(audio_capture_task_stop() == ESP_ERR_TIMEOUT);
    CHECK(s_task_active);
    CHECK(s_fake_disable_calls == 0);
    CHECK(audio_capture_deinit() == ESP_ERR_TIMEOUT); // deinit refuses while the task may still run
    CHECK(s_fake_del_calls == 0);

    s_fake_task_exits_on_delay = true;
    CHECK(audio_capture_deinit() == ESP_OK);
    CHECK(s_fake_del_calls == 1);
}

static void test_overflow_callback_and_stats(void)
{
    reset_all();
    CHECK(audio_capture_get_stats(NULL) == ESP_ERR_INVALID_ARG);
    CHECK(audio_capture_init(&VALID_CONFIG) == ESP_OK);
    CHECK(s_fake_overflow_cb != NULL);
    s_fake_overflow_cb(&s_fake_channel, NULL, NULL);
    s_fake_overflow_cb(&s_fake_channel, NULL, NULL);
    audio_capture_stats_t stats;
    CHECK(audio_capture_get_stats(&stats) == ESP_OK);
    CHECK(stats.dma_overflows == 2);
}

static void test_led_optional(void)
{
    reset_all();
    audio_capture_config_t config = VALID_CONFIG;
    config.status_led_gpio = GPIO_NUM_NC;
    CHECK(audio_capture_init(&config) == ESP_OK);
    CHECK(audio_capture_task_start(FAKE_BUFFER) == ESP_OK);
    CHECK(s_fake_gpio_reset_calls == 0);
    CHECK(audio_capture_deinit() == ESP_OK);
    CHECK(s_fake_gpio_reset_calls == 0);
}

typedef struct
{
    const char *name;
    void (*run)(void);
} test_case_t;

int main(void)
{
    const test_case_t tests[] = {
        {"init rejects bad config", test_init_rejects_bad_config},
        {"init configures I2S for INMP441", test_init_configures_i2s},
        {"lifecycle states", test_lifecycle_states},
        {"init failure cleans up", test_init_failure_cleans_up},
        {"read rejects bad arguments", test_read_rejects_bad_arguments},
        {"32-bit slots -> PCM16", test_read_converts_slots_to_pcm16},
        {"gain boosts and saturates", test_gain_boosts_and_saturates},
        {"timeout returns partial frame", test_read_timeout_returns_partial_frame},
        {"PCM little-endian byte order", test_pcm_byte_order},
        {"frame layout matches protocol", test_frame_layout_matches_protocol},
        {"capture sends 644-byte frames", test_capture_sends_full_frame},
        {"full buffer drops, sequence advances", test_full_buffer_drops_but_sequence_advances},
        {"timeouts and errors are counted", test_timeout_and_error_counting},
        {"sequence wraps at 65535", test_sequence_wraps},
        {"task start/stop", test_task_start_and_stop},
        {"task create failure rolls back", test_task_create_failure_rolls_back},
        {"stuck task keeps resources", test_task_stop_timeout_keeps_resources},
        {"DMA overflow counter and stats", test_overflow_callback_and_stats},
        {"status LED optional", test_led_optional},
    };
    const size_t count = sizeof(tests) / sizeof(tests[0]);

    for (size_t i = 0; i < count; i++)
    {
        const int failures_before = s_failures;
        tests[i].run();
        printf("%s %s\n", (s_failures == failures_before) ? "PASS" : "FAIL", tests[i].name);
    }

    printf("\n%zu tests, %d checks, %d failures\n", count, s_checks, s_failures);
    return (s_failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
