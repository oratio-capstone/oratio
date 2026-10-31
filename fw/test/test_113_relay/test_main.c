#include "network.h"
#include <stdbool.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/task.h"

/* ========================================================================== */
/* 1. Build switches: choose the test and this board's role.
 * ========================================================================== */

/* Set these flags in platformio.ini, with the same test mode on both endpoints:
 *   TEST_BENCHMARK=1: task 116, repeated finite runs with loss and RTT summaries.
 *   Otherwise TEST_CONTINUOUS_STREAM=1: task 115, continuous dummy audio.
 *   Otherwise: task 113, one finite numbered-packet delivery test.
 * TEST_IS_PRODUCER selects sending (1) or receiving (0), independently of mesh role.
 * A root or a leaf can be either role. The middle runs the separate heartbeat test.
 */
#ifndef TEST_CONTINUOUS_STREAM
#define TEST_CONTINUOUS_STREAM 0
#endif
#if (TEST_CONTINUOUS_STREAM != 0) && (TEST_CONTINUOUS_STREAM != 1)
#error "TEST_CONTINUOUS_STREAM must be 0 or 1"
#endif
#ifndef TEST_IS_PRODUCER
#error "Define TEST_IS_PRODUCER as 0 or 1 in the test environment"
#elif (TEST_IS_PRODUCER != 0) && (TEST_IS_PRODUCER != 1)
#error "TEST_IS_PRODUCER must be 0 or 1"
#endif

#ifndef TEST_BENCHMARK
#define TEST_BENCHMARK 0
#endif
#if (TEST_BENCHMARK != 0) && (TEST_BENCHMARK != 1)
#error "TEST_BENCHMARK must be 0 or 1"
#endif

/* ========================================================================== */
/* 2. Shared task settings: startup wait, CPU core, stack, and scheduling.
 * ========================================================================== */

static const char *TAG = "TEST_113_115_116";
static const uint32_t TEST_STARTUP_DELAY_MS = 60000;
static const BaseType_t TEST_WORKER_CORE = 1;
static const uint32_t TEST_WORKER_STACK_BYTES = 4096;
/* The consumer has higher priority than the network task so it can drain arrivals
 * promptly, then block again. The producer keeps the network task's priority.
 */
#if TEST_IS_PRODUCER
static const UBaseType_t TEST_WORKER_PRIORITY = tskIDLE_PRIORITY + 1;
#else
static const UBaseType_t TEST_WORKER_PRIORITY = tskIDLE_PRIORITY + 2;
#endif

/* ========================================================================== */
/* 3. Task 116: repeated full-size frame benchmark (compiled only in benchmark mode).
 * ========================================================================== */

#if TEST_BENCHMARK

/* Edit these settings on both endpoints before rebuilding:
 * RUN_COUNT = repetitions; FRAME_COUNT = frames generated in each repetition.
 * INTERVAL_MS controls sending speed (20 ms at the shared audio frame duration).
 * ECHO_STRIDE selects one frame in every 50 for a round-trip measurement.
 * These payloads are test bytes, not sound to play through a speaker.
 */
enum
{
    BENCHMARK_RUN_COUNT = 10,
    BENCHMARK_FRAME_COUNT = 1000,
    BENCHMARK_INTERVAL_MS = AUDIO_FRAME_DURATION_MS,
    BENCHMARK_ECHO_STRIDE = 50,
    BENCHMARK_PATTERN_OFFSET = 13,
    BENCHMARK_REQUEST = 'Q',
    BENCHMARK_REPLY = 'R'
};
/* GRACE allows late frames/replies after generation. RUN_GAP separates repetitions.
 * FIRST_TIMEOUT bounds waiting for a run that never arrives. REPORT changes only
 * how often progress is printed, not how often frames are sent.
 */
static const uint32_t BENCHMARK_GRACE_MS = 5000;
static const uint32_t BENCHMARK_RUN_GAP_MS = 10000;
static const uint32_t BENCHMARK_FIRST_TIMEOUT_MS = 300000;
static const uint32_t BENCHMARK_REPORT_MS = 10000;

/* Both handles are needed: even the consumer sends sampled echoes back.
 * capture: test -> network. playback: network -> test. Each message is one frame.
 */
typedef struct
{
    MessageBufferHandle_t capture;
    MessageBufferHandle_t playback;
} benchmark_buffers_t;

/* Receiver state for one run. seen[] counts each frame number once, so a late
 * frame fills a missing slot while a duplicate never increases the unique count.
 * first_us/last_us measure the arrival span on this device's own clock.
 */
typedef struct
{
    bool seen[BENCHMARK_FRAME_COUNT];
    bool started;
    uint32_t run_id;
    uint32_t unique;
    uint32_t duplicates;
    uint32_t late;
    uint32_t invalid;
    uint32_t foreign;
    uint32_t highest;
    uint32_t echo_failures;
    int64_t first_us;
    int64_t last_us;
} benchmark_counts_t;

/* Compact final record for one repetition. Producer fills send/RTT fields;
 * consumer fills receive/quality/rate fields. Each board prints its own summary.
 * Large reusable storage is static to keep it off the task's small stack.
 */
typedef struct
{
    uint32_t run_id;
    uint32_t received;
    uint32_t duplicates;
    uint32_t late;
    uint32_t invalid;
    uint32_t buffered;
    uint32_t local_drops;
    uint32_t echo_requests;
    uint32_t rtt_count;
    int64_t rtt_sum_us;
    int64_t rtt_min_us;
    int64_t rtt_max_us;
    double fps;
} benchmark_result_t;

static benchmark_result_t s_results[BENCHMARK_RUN_COUNT];

/* 3a. Combine saved runs. Loss uses all expected frames, including no-data runs.
 * RTT average weights every reply equally; it does not average the run averages.
 * mean-run-rate is an arithmetic average of per-run rates, excluding idle gaps.
 */
static void benchmark_report_batch(void)
{
    uint32_t received = 0;
    uint32_t duplicates = 0;
    uint32_t late = 0;
    uint32_t invalid = 0;
    uint32_t buffered = 0;
    uint32_t local_drops = 0;
    uint32_t requests = 0;
    uint32_t replies = 0;
    int64_t rtt_sum = 0;
    int64_t rtt_min = INT64_MAX;
    int64_t rtt_max = 0;
    double rate_sum = 0.0;
    double best_loss = 100.0;
    double worst_loss = 0.0;
    for (size_t i = 0; i < BENCHMARK_RUN_COUNT; i++)
    {
        const benchmark_result_t *result = &s_results[i];
        received += result->received;
        duplicates += result->duplicates;
        late += result->late;
        invalid += result->invalid;
        buffered += result->buffered;
        local_drops += result->local_drops;
        requests += result->echo_requests;
        replies += result->rtt_count;
        rtt_sum += result->rtt_sum_us;
        rate_sum += result->fps;
        double loss = 100.0 * (BENCHMARK_FRAME_COUNT - result->received) / BENCHMARK_FRAME_COUNT;
        if (loss < best_loss)
        {
            best_loss = loss;
        }
        if (loss > worst_loss)
        {
            worst_loss = loss;
        }
        if (result->rtt_count > 0 && result->rtt_min_us < rtt_min)
        {
            rtt_min = result->rtt_min_us;
        }
        if (result->rtt_max_us > rtt_max)
        {
            rtt_max = result->rtt_max_us;
        }
#if TEST_IS_PRODUCER
        ESP_LOGI(TAG, "Batch row %u: id=%08lx buffered=%lu local-drops=%lu echoes=%lu/%lu", (unsigned int)(i + 1),
                 (unsigned long)result->run_id, (unsigned long)result->buffered, (unsigned long)result->local_drops,
                 (unsigned long)result->rtt_count, (unsigned long)result->echo_requests);
#else
        ESP_LOGI(TAG, "Batch row %u: id=%08lx unique=%lu missing=%lu loss=%.2f%% rate=%.2f frames/s",
                 (unsigned int)(i + 1), (unsigned long)result->run_id, (unsigned long)result->received,
                 (unsigned long)(BENCHMARK_FRAME_COUNT - result->received), loss, result->fps);
#endif
    }
#if TEST_IS_PRODUCER
    ESP_LOGI(TAG, "Batch producer: runs=%u generated=%u buffered=%lu local-drops=%lu echoes=%lu/%lu",
             (unsigned int)BENCHMARK_RUN_COUNT, (unsigned int)(BENCHMARK_RUN_COUNT * BENCHMARK_FRAME_COUNT),
             (unsigned long)buffered, (unsigned long)local_drops, (unsigned long)replies, (unsigned long)requests);
    ESP_LOGI(TAG, "Batch RTT: samples=%lu missing=%lu min/weighted-avg/max=%.3f/%.3f/%.3f ms", (unsigned long)replies,
             (unsigned long)(requests - replies), replies ? (double)rtt_min / 1000.0 : 0.0,
             replies ? (double)rtt_sum / replies / 1000.0 : 0.0, (double)rtt_max / 1000.0);
#else
    ESP_LOGI(TAG, "Batch consumer: runs=%u unique=%lu missing=%lu loss=%.2f%% best/worst=%.2f/%.2f%%",
             (unsigned int)BENCHMARK_RUN_COUNT, (unsigned long)received,
             (unsigned long)(BENCHMARK_RUN_COUNT * BENCHMARK_FRAME_COUNT - received),
             100.0 * (BENCHMARK_RUN_COUNT * BENCHMARK_FRAME_COUNT - received) /
                 (BENCHMARK_RUN_COUNT * BENCHMARK_FRAME_COUNT),
             best_loss, worst_loss);
    ESP_LOGI(TAG, "Batch quality: duplicates=%lu out-of-order=%lu invalid=%lu mean-run-rate=%.2f frames/s",
             (unsigned long)duplicates, (unsigned long)late, (unsigned long)invalid, rate_sum / BENCHMARK_RUN_COUNT);
#endif
    ESP_LOGI(TAG, "Batch complete; save both endpoints' summaries. Reset endpoints to start a new batch.");
}

/* 3b. Test payload helpers. This is inside the normal Oratio audio payload:
 *   bytes 0..3   frame number, big endian (most significant byte first)
 *   byte 4       Q request or R reply
 *   bytes 5..8   random run ID, used to match replies to this repetition
 *   bytes 9..12  zero-based run index, used to move through the batch
 *   bytes 13..  predictable pattern: byte[i] = i modulo 256
 * The normal seven-byte network header is built separately by network.c.
 */
static uint32_t benchmark_read_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | bytes[3];
}

static void benchmark_write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

/* Reject malformed input before using a received number as an array index.
 * The network header sequence must match the diagnostic frame number.
 */
static esp_err_t benchmark_validate_frame(const audio_frame_t *frame, uint8_t direction)
{
    if (frame->length != AUDIO_FRAME_BYTES)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t *bytes = (const uint8_t *)frame->samples;
    uint32_t number = benchmark_read_u32(bytes);
    if (bytes[4] != direction || number >= BENCHMARK_FRAME_COUNT || frame->sequence != number ||
        benchmark_read_u32(bytes + 9) >= BENCHMARK_RUN_COUNT)
    {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = BENCHMARK_PATTERN_OFFSET; i < AUDIO_FRAME_BYTES; i++)
    {
        if (bytes[i] != (uint8_t)i)
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

#if TEST_IS_PRODUCER
/* 3c. Producer: generate requests and measure sampled return echoes.
 * Timestamps use only the producer's clock; no clock synchronization is needed.
 * RTT includes application buffers, gateway/consumer work, and network travel.
 * It is not one-way audio latency. Reused arrays are cleared before each run.
 */
static int64_t s_sent_us[BENCHMARK_FRAME_COUNT];
static bool s_echo_seen[BENCHMARK_FRAME_COUNT];
static uint32_t s_rtt_count;
static uint32_t s_echo_duplicates;
static uint32_t s_invalid_echoes;
static int64_t s_rtt_sum;
static int64_t s_rtt_min = INT64_MAX;
static int64_t s_rtt_max;

/* Drain available replies without blocking. Accept each matching sampled reply
 * once; reject other runs, unsent frames, and unexpected echo numbers.
 */
static void benchmark_receive_echoes(MessageBufferHandle_t playback, uint32_t run_id, uint32_t run_index)
{
    static audio_frame_t reply;
    while (xMessageBufferReceive(playback, &reply, sizeof(reply), 0) == sizeof(reply))
    {
        const uint8_t *bytes = (const uint8_t *)reply.samples;
        if (benchmark_validate_frame(&reply, BENCHMARK_REPLY) != ESP_OK || benchmark_read_u32(bytes + 5) != run_id ||
            benchmark_read_u32(bytes + 9) != run_index)
        {
            s_invalid_echoes++;
            continue;
        }
        uint32_t number = benchmark_read_u32(bytes);
        if (number % BENCHMARK_ECHO_STRIDE != 0 || s_sent_us[number] == 0)
        {
            s_invalid_echoes++;
            continue;
        }
        if (s_echo_seen[number])
        {
            s_echo_duplicates++;
            continue;
        }
        s_echo_seen[number] = true;
        int64_t rtt = esp_timer_get_time() - s_sent_us[number];
        s_rtt_count++;
        s_rtt_sum += rtt;
        if (rtt < s_rtt_min)
        {
            s_rtt_min = rtt;
        }
        if (rtt > s_rtt_max)
        {
            s_rtt_max = rtt;
        }
    }
}

/* Generate one finite run, collect late echoes, then save its summary record. */
static void benchmark_run_producer_once(benchmark_buffers_t *buffers, uint32_t run_index)
{
    static audio_frame_t frame = {.length = AUDIO_FRAME_BYTES};
    uint8_t *bytes = (uint8_t *)frame.samples;
    memset(s_sent_us, 0, sizeof(s_sent_us));
    memset(s_echo_seen, 0, sizeof(s_echo_seen));
    s_rtt_count = 0;
    s_echo_duplicates = 0;
    s_invalid_echoes = 0;
    s_rtt_sum = 0;
    s_rtt_min = INT64_MAX;
    s_rtt_max = 0;
    /* Index orders runs; the random ID distinguishes separate repetitions/batches. */
    uint32_t run_id = esp_random();
    uint32_t buffered = 0;
    uint32_t failures = 0;
    uint32_t echo_requests = 0;
    for (size_t i = BENCHMARK_PATTERN_OFFSET; i < AUDIO_FRAME_BYTES; i++)
    {
        bytes[i] = (uint8_t)i;
    }
    bytes[4] = BENCHMARK_REQUEST;
    benchmark_write_u32(bytes + 5, run_id);
    benchmark_write_u32(bytes + 9, run_index);
    ESP_LOGI(TAG, "Starting run %u/%u: id=%08lx frames=%u interval=%u ms", (unsigned int)(run_index + 1),
             (unsigned int)BENCHMARK_RUN_COUNT, (unsigned long)run_id, (unsigned int)BENCHMARK_FRAME_COUNT,
             (unsigned int)BENCHMARK_INTERVAL_MS);
    mesh_network_log_status();
    TickType_t wake = xTaskGetTickCount();
    TickType_t last_report = wake;
    int64_t start_us = esp_timer_get_time();
    /* Timestamp before enqueue: buffer acceptance is not proof of remote delivery.
     * Nonblocking sends count local drops instead of slowing the requested rate.
     */
    for (uint32_t number = 0; number < BENCHMARK_FRAME_COUNT; number++)
    {
        frame.sequence = (uint16_t)number;
        benchmark_write_u32(bytes, number);
        s_sent_us[number] = esp_timer_get_time();
        if (xMessageBufferSend(buffers->capture, &frame, sizeof(frame), 0) == sizeof(frame))
        {
            buffered++;
            if (number % BENCHMARK_ECHO_STRIDE == 0)
            {
                echo_requests++;
            }
        }
        else
        {
            failures++;
            s_sent_us[number] = 0;
        }
        benchmark_receive_echoes(buffers->playback, run_id, run_index);
        TickType_t now = xTaskGetTickCount();
        if ((TickType_t)(now - last_report) >= pdMS_TO_TICKS(BENCHMARK_REPORT_MS))
        {
            ESP_LOGI(TAG, "Benchmark progress: buffered=%lu local-drops=%lu echoes=%lu stack-free=%lu B",
                     (unsigned long)buffered, (unsigned long)failures, (unsigned long)s_rtt_count,
                     (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            last_report = now;
        }
        /* Target a regular schedule rather than adding processing time to every delay. */
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(BENCHMARK_INTERVAL_MS));
    }
    int64_t duration_us = esp_timer_get_time() - start_us;
    /* Stop generating, but keep reading so late echoes still contribute to RTT. */
    TickType_t drain_start = xTaskGetTickCount();
    while ((TickType_t)(xTaskGetTickCount() - drain_start) < pdMS_TO_TICKS(BENCHMARK_GRACE_MS))
    {
        benchmark_receive_echoes(buffers->playback, run_id, run_index);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "Producer final: generated=1000 buffered=%lu local-drops=%lu duration=%.3f s",
             (unsigned long)buffered, (unsigned long)failures, (double)duration_us / 1000000.0);
    ESP_LOGI(TAG, "RTT: replies=%lu missing=%lu duplicates=%lu invalid=%lu min/avg/max=%.3f/%.3f/%.3f ms",
             (unsigned long)s_rtt_count, (unsigned long)(echo_requests - s_rtt_count), (unsigned long)s_echo_duplicates,
             (unsigned long)s_invalid_echoes, s_rtt_count ? (double)s_rtt_min / 1000.0 : 0.0,
             s_rtt_count ? (double)s_rtt_sum / s_rtt_count / 1000.0 : 0.0, (double)s_rtt_max / 1000.0);
    /* Preserve this run before the working counters are reset for the next one. */
    benchmark_result_t *result = &s_results[run_index];
    result->run_id = run_id;
    result->buffered = buffered;
    result->local_drops = failures;
    result->echo_requests = echo_requests;
    result->rtt_count = s_rtt_count;
    result->rtt_sum_us = s_rtt_sum;
    result->rtt_min_us = s_rtt_min;
    result->rtt_max_us = s_rtt_max;
}

/* Wait once for startup, then repeat send/drain/gap without manual resets. */
static void benchmark_run(benchmark_buffers_t *buffers)
{
    ESP_LOGI(TAG, "Batch producer: %u runs; startup wait %u s", (unsigned int)BENCHMARK_RUN_COUNT,
             (unsigned int)(TEST_STARTUP_DELAY_MS / 1000));
    vTaskDelay(pdMS_TO_TICKS(TEST_STARTUP_DELAY_MS));
    for (uint32_t index = 0; index < BENCHMARK_RUN_COUNT; index++)
    {
        benchmark_run_producer_once(buffers, index);
        if (index + 1 < BENCHMARK_RUN_COUNT)
        {
            vTaskDelay(pdMS_TO_TICKS(BENCHMARK_RUN_GAP_MS));
        }
    }
    benchmark_report_batch();
}

#else
/* 3d. Consumer: validate arrivals, track delivery, and send selected frames back. */
static void benchmark_record(audio_frame_t *frame, benchmark_counts_t *counts, MessageBufferHandle_t capture)
{
    if (benchmark_validate_frame(frame, BENCHMARK_REQUEST) != ESP_OK)
    {
        counts->invalid++;
        return;
    }
    uint8_t *bytes = (uint8_t *)frame->samples;
    uint32_t number = benchmark_read_u32(bytes);
    uint32_t run_id = benchmark_read_u32(bytes + 5);
    if (counts->started && counts->run_id != run_id)
    {
        counts->foreign++;
        return;
    }
    if (!counts->started)
    {
        counts->started = true;
        counts->run_id = run_id;
        counts->first_us = esp_timer_get_time();
    }
    if (counts->seen[number])
    {
        counts->duplicates++;
        return;
    }
    /* A late unique frame still counts as received. highest is only for ordering. */
    counts->seen[number] = true;
    counts->unique++;
    counts->last_us = esp_timer_get_time();
    if (number < counts->highest)
    {
        counts->late++;
    }
    else
    {
        counts->highest = number;
    }
    if (number % BENCHMARK_ECHO_STRIDE != 0)
    {
        return;
    }
    /* Reuse the received frame as an echo; retain its frame number and run IDs.
     * network.c adds this consumer's own node ID when it sends the frame.
     */
    bytes[4] = BENCHMARK_REPLY;
    if (xMessageBufferSend(capture, frame, sizeof(*frame), 0) != sizeof(*frame))
    {
        counts->echo_failures++;
    }
}

/* Report arrival rate from first to last unique frame, excluding startup/idle time.
 * A progress snapshot's remaining count includes frames not yet generated.
 * Only the final snapshot describes missing frames at the measurement deadline.
 */
static void benchmark_report(const benchmark_counts_t *counts, bool final)
{
    int64_t span_us = counts->last_us - counts->first_us;
    double seconds = span_us > 0 ? (double)span_us / 1000000.0 : 0.0;
    double fps = seconds > 0.0 && counts->unique > 1 ? (counts->unique - 1) / seconds : 0.0;
    ESP_LOGI(TAG, "%s: run=%08lx unique=%lu remaining=%lu duplicates=%lu out-of-order=%lu invalid=%lu foreign=%lu",
             final ? "Benchmark final" : "Benchmark progress", (unsigned long)counts->run_id,
             (unsigned long)counts->unique, (unsigned long)(BENCHMARK_FRAME_COUNT - counts->unique),
             (unsigned long)counts->duplicates, (unsigned long)counts->late, (unsigned long)counts->invalid,
             (unsigned long)counts->foreign);
    ESP_LOGI(TAG, "%s=%.2f%% rate=%.2f frames/s payload=%.2f kbit/s echo-buffer-failures=%lu stack-free=%lu B",
             final ? "final-missing" : "not-yet-received",
             100.0 * (BENCHMARK_FRAME_COUNT - counts->unique) / BENCHMARK_FRAME_COUNT, fps,
             fps * AUDIO_FRAME_BYTES * 8 / 1000.0, (unsigned long)counts->echo_failures,
             (unsigned long)uxTaskGetStackHighWaterMark(NULL));
    /* In-progress missing/loss includes frames not yet generated; only final is loss. */
}

/* Save one completed or skipped run before its working counters are cleared.
 * A run with no arrivals remains zero received and counts as fully missing.
 */
static void benchmark_save_consumer(const benchmark_counts_t *counts, uint32_t index)
{
    benchmark_result_t *result = &s_results[index];
    result->run_id = counts->run_id;
    result->received = counts->unique;
    result->duplicates = counts->duplicates;
    result->late = counts->late;
    result->invalid = counts->invalid;
    int64_t span_us = counts->last_us - counts->first_us;
    if (span_us > 0 && counts->unique > 1)
    {
        result->fps = (double)(counts->unique - 1) * 1000000.0 / span_us;
    }
    ESP_LOGI(TAG, "Finished run %u/%u", (unsigned int)(index + 1), (unsigned int)BENCHMARK_RUN_COUNT);
    benchmark_report(counts, true);
}

/* Move through the expected run indexes. A future run can finish an overdue run;
 * its first frame is kept as pending so it is not lost during that transition.
 */
static void benchmark_run(benchmark_buffers_t *buffers)
{
    static audio_frame_t frame;
    static benchmark_counts_t counts;
    uint32_t index = 0;
    bool pending = false;
    TickType_t wait_start = xTaskGetTickCount();
    TickType_t last_report = wait_start;
    ESP_LOGI(TAG, "Batch consumer ready: %u runs; first-packet timeout %u s per run", (unsigned int)BENCHMARK_RUN_COUNT,
             (unsigned int)(BENCHMARK_FIRST_TIMEOUT_MS / 1000));
    mesh_network_log_status();
    while (index < BENCHMARK_RUN_COUNT)
    {
        /* pending means frame already contains a future run's first arrival.
         * Otherwise block briefly for data so other tasks can run while idle.
         */
        size_t received = pending ? sizeof(frame)
                                  : xMessageBufferReceive(buffers->playback, &frame, sizeof(frame), pdMS_TO_TICKS(100));
        pending = false;
        if (received == sizeof(frame) && benchmark_validate_frame(&frame, BENCHMARK_REQUEST) == ESP_OK)
        {
            uint32_t received_index = benchmark_read_u32((const uint8_t *)frame.samples + 9);
            if (received_index > index)
            {
                /* Preserve this frame while finalizing a skipped or overdue run. */
                benchmark_save_consumer(&counts, index);
                memset(&counts, 0, sizeof(counts));
                index++;
                pending = true;
                wait_start = xTaskGetTickCount();
                continue;
            }
            if (received_index == index)
            {
                benchmark_record(&frame, &counts, buffers->capture);
            }
            /* Packets from completed runs cannot seed the next run. */
        }
        else if (received != 0)
        {
            counts.invalid++;
        }
        TickType_t now = xTaskGetTickCount();
        /* Distinguish a run that never started from one whose receive window ended.
         * Wait through the full window even if every expected frame already arrived.
         */
        bool no_start = !counts.started && (TickType_t)(now - wait_start) >= pdMS_TO_TICKS(BENCHMARK_FIRST_TIMEOUT_MS);
        bool expired =
            counts.started && esp_timer_get_time() - counts.first_us >=
                                  (int64_t)(BENCHMARK_FRAME_COUNT * BENCHMARK_INTERVAL_MS + BENCHMARK_GRACE_MS) * 1000;
        if (no_start || expired)
        {
            benchmark_save_consumer(&counts, index);
            ESP_LOGI(TAG, "Run ended: %s", no_start ? "no valid frame arrived" : "receive deadline reached");
            memset(&counts, 0, sizeof(counts));
            index++;
            wait_start = now;
            last_report = now;
            continue;
        }
        if ((TickType_t)(now - last_report) >= pdMS_TO_TICKS(BENCHMARK_REPORT_MS))
        {
            benchmark_report(&counts, false);
            last_report = now;
        }
    }
    mesh_network_log_status();
    benchmark_report_batch();
}

#endif

/* End task 116; the next branches compile only when benchmark mode is disabled. */
#elif TEST_CONTINUOUS_STREAM
/* ========================================================================== */
/* 4. Task 115: continuous dummy stream and periodic reception summaries.
 * ========================================================================== */

static const uint32_t TEST_STREAM_INTERVAL_MS = 100;
static const uint32_t TEST_REPORT_INTERVAL_MS = 2000;

/* Synthetic payload: four-byte big-endian frame number, then byte[i] = i.
 * These are diagnostic bytes, not playable PCM audio.
 */
#if TEST_IS_PRODUCER
/* Generate full-size frames forever. The four-byte diagnostic number is separate
 * from the 16-bit header sequence, which naturally wraps after 65535.
 */
static void run_producer(MessageBufferHandle_t capture_buffer)
{
    static audio_frame_t stream_item = {.length = AUDIO_FRAME_BYTES};
    uint8_t *stream_payload = (uint8_t *)stream_item.samples;
    uint32_t frame_number = 0;
    uint32_t buffered_frames = 0;
    uint32_t buffer_failures = 0;

    /* Pattern bytes do not change between frames. */
    for (size_t i = 4; i < ORATIO_MAX_PAYLOAD_SIZE; i++)
    {
        stream_payload[i] = (uint8_t)i;
    }

    ESP_LOGI(TAG, "Producer: waiting %u s, then sending 640-byte frames every %u ms",
             (unsigned int)(TEST_STARTUP_DELAY_MS / 1000), (unsigned int)TEST_STREAM_INTERVAL_MS);
    vTaskDelay(pdMS_TO_TICKS(TEST_STARTUP_DELAY_MS));
    TickType_t last_report = xTaskGetTickCount();

    while (true)
    {
        stream_item.sequence = (uint16_t)frame_number;
        stream_payload[0] = (uint8_t)(frame_number >> 24);
        stream_payload[1] = (uint8_t)(frame_number >> 16);
        stream_payload[2] = (uint8_t)(frame_number >> 8);
        stream_payload[3] = (uint8_t)frame_number;

        if (xMessageBufferSend(capture_buffer, &stream_item, sizeof(stream_item), 0) == sizeof(stream_item))
        {
            buffered_frames++;
        }
        else
        {
            buffer_failures++;
        }
        /* Advance for every generated frame, including local message buffer drops. */
        frame_number++;

        TickType_t now = xTaskGetTickCount();
        if ((TickType_t)(now - last_report) >= pdMS_TO_TICKS(TEST_REPORT_INTERVAL_MS))
        {
            ESP_LOGI(TAG, "Generated=%lu buffered=%lu buffer failures=%lu stack-free=%lu B",
                     (unsigned long)(buffered_frames + buffer_failures), (unsigned long)buffered_frames,
                     (unsigned long)buffer_failures, (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            last_report = now;
        }
        /* Initial bring-up pacing; not yet an audio-rate benchmark. */
        vTaskDelay(pdMS_TO_TICKS(TEST_STREAM_INTERVAL_MS));
    }
}
#else
typedef struct
{
    uint32_t valid_frames;
    uint32_t corrupted_frames;
    uint32_t malformed_frames;
    bool have_previous_frame;
    uint32_t highest_frame;
    uint32_t observed_gaps;
    uint32_t repeated_highest;
    uint32_t late_or_duplicate;
} stream_counts_t;

/* Check frame size, deterministic payload, and sequence before updating counters.
 * This continuous mode tracks observed jumps, not exact permanent packet loss.
 */
static void record_stream_frame(const audio_frame_t *frame, stream_counts_t *counts)
{
    if (frame->length != AUDIO_FRAME_BYTES)
    {
        counts->malformed_frames++;
        return;
    }
    const uint8_t *payload = (const uint8_t *)frame->samples;
    for (size_t i = 4; i < AUDIO_FRAME_BYTES; i++)
    {
        if (payload[i] != (uint8_t)i)
        {
            counts->corrupted_frames++;
            return;
        }
    }
    uint32_t number = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) | ((uint32_t)payload[2] << 8) |
                      (uint32_t)payload[3];
    if (frame->sequence != (uint16_t)number)
    {
        counts->corrupted_frames++;
        return;
    }
    counts->valid_frames++;
    if (!counts->have_previous_frame)
    {
        counts->have_previous_frame = true;
        counts->highest_frame = number;
        return;
    }
    /* Gaps are observations; late arrivals do not establish permanent loss. */
    uint32_t difference = number - counts->highest_frame;
    if (difference == 0)
    {
        counts->repeated_highest++;
        return;
    }
    if (difference < 0x80000000UL)
    {
        counts->observed_gaps += difference - 1;
        counts->highest_frame = number;
        return;
    }
    counts->late_or_duplicate++;
}

static void report_stream_counts(const stream_counts_t *counts)
{
    ESP_LOGI(TAG,
             "Valid=%lu corrupted=%lu malformed=%lu gaps=%lu "
             "repeated-highest=%lu late-or-duplicate=%lu highest=%lu stack-free=%lu B",
             (unsigned long)counts->valid_frames, (unsigned long)counts->corrupted_frames,
             (unsigned long)counts->malformed_frames, (unsigned long)counts->observed_gaps,
             (unsigned long)counts->repeated_highest, (unsigned long)counts->late_or_duplicate,
             (unsigned long)counts->highest_frame, (unsigned long)uxTaskGetStackHighWaterMark(NULL));
}

/* Block for incoming frames, process them, and report at a fixed log interval.
 * Reports continue even when no traffic arrives, making a stopped stream visible.
 */
static void run_consumer(MessageBufferHandle_t playback_buffer)
{
    static audio_frame_t frame;
    static stream_counts_t counts = {0};
    TickType_t last_report = xTaskGetTickCount();
    ESP_LOGI(TAG, "Consumer ready: expecting 640-byte dummy audio frames");
    while (true)
    {
        size_t received = xMessageBufferReceive(playback_buffer, &frame, sizeof(frame), pdMS_TO_TICKS(1000));
        if (received == sizeof(frame))
        {
            record_stream_frame(&frame, &counts);
        }
        TickType_t now = xTaskGetTickCount();
        if ((TickType_t)(now - last_report) >= pdMS_TO_TICKS(TEST_REPORT_INTERVAL_MS))
        {
            report_stream_counts(&counts);
            last_report = now;
        }
    }
}

#endif

#else
/* ========================================================================== */
/* 5. Task 113: finite numbered delivery check with a four-byte payload.
 * ========================================================================== */

/* Unlike task 116, this simpler test numbers frames 1..1000 rather than 0..999.
 * Payload: two-byte big-endian number followed by fixed marker bytes AA 55.
 * enum also permits a compile-time-sized seen[] array without a logic macro.
 */
enum
{
    TEST_PACKET_COUNT = 1000,
    TEST_SEND_INTERVAL_MS = 100,
    TEST_RECEIVE_TIMEOUT_MS = TEST_PACKET_COUNT * TEST_SEND_INTERVAL_MS + 30000
};
#if !TEST_IS_PRODUCER
static const uint32_t TEST_FIRST_PACKET_TIMEOUT_MS = 300000;
#endif

/* Producer sends the numbered records once. Consumer deduplicates them and ends
 * on complete reception, its receive deadline, or a first-packet timeout.
 */
static void run_numbered_test(MessageBufferHandle_t buffer)
{
#if TEST_IS_PRODUCER
    MessageBufferHandle_t capture_buffer = buffer;
#else
    MessageBufferHandle_t playback_buffer = buffer;
#endif
#if TEST_IS_PRODUCER
    /* Start the consumer first and confirm gateway registration before this send. */
    audio_frame_t test_item = {.length = 4};
    uint8_t *test_payload = (uint8_t *)test_item.samples;

    ESP_LOGI(TAG, "Waiting %u seconds before starting the numbered packet run",
             (unsigned int)(TEST_STARTUP_DELAY_MS / 1000));
    /* The network task keeps sending heartbeats during this delay. */
    vTaskDelay(pdMS_TO_TICKS(TEST_STARTUP_DELAY_MS));
    ESP_LOGI(TAG, "Starting numbered packet run");

    uint32_t buffered_packets = 0;
    uint32_t buffer_failures = 0;

    for (uint16_t test_number = 1; // producer loop
         test_number <= TEST_PACKET_COUNT; test_number++)
    {
        test_item.sequence = test_number;
        test_payload[0] = (uint8_t)(test_number >> 8);
        test_payload[1] = (uint8_t)(test_number & 0xFF);
        test_payload[2] = 0xAA;
        test_payload[3] = 0x55;

        /* Send one complete record and check the returned byte count. */
        if (xMessageBufferSend(capture_buffer, &test_item, sizeof(test_item), pdMS_TO_TICKS(1000)) == sizeof(test_item))
        {
            buffered_packets++;
        }
        else
        {
            buffer_failures++;
            ESP_LOGD(TAG, "Capture message buffer full; test payload not buffered");
        }

        vTaskDelay(pdMS_TO_TICKS(TEST_SEND_INTERVAL_MS));
    }

    ESP_LOGI(TAG, "Producer finished: buffered=%lu, buffer failures=%lu", (unsigned long)buffered_packets,
             (unsigned long)buffer_failures);
    /* Networking keeps running. Its message buffers must remain alive. */
    return;
#else
    static audio_frame_t received_packet;
    const uint8_t *received_payload = (const uint8_t *)received_packet.samples;

    /* Persistent test storage avoids consuming the main task's small stack. */
    static bool seen[TEST_PACKET_COUNT + 1];
    uint32_t unique_received = 0;
    uint32_t duplicate_packets = 0;
    uint16_t highest_received = 0;
    uint32_t out_of_order_packets = 0;
    uint32_t malformed_packets = 0;
    bool run_started = false;
    TickType_t run_start_time = 0;
    TickType_t first_packet_wait_start = xTaskGetTickCount();

    /* Run with one producer; audio_frame_t intentionally has no source identity. */
    ESP_LOGI(TAG, "Consumer ready: expecting %u packets; first-packet timeout %u s", (unsigned int)TEST_PACKET_COUNT,
             (unsigned int)(TEST_FIRST_PACKET_TIMEOUT_MS / 1000));

    while (true) // Consumer loop
    {
        TickType_t now = xTaskGetTickCount();
        bool timed_out = run_started && (TickType_t)(now - run_start_time) >= pdMS_TO_TICKS(TEST_RECEIVE_TIMEOUT_MS);
        bool no_packet_timeout =
            !run_started && (TickType_t)(now - first_packet_wait_start) >= pdMS_TO_TICKS(TEST_FIRST_PACKET_TIMEOUT_MS);

        if (unique_received == TEST_PACKET_COUNT || timed_out || no_packet_timeout)
        {
            uint32_t missing_packets = TEST_PACKET_COUNT - unique_received;

            ESP_LOGI(TAG,
                     "Summary: unique=%lu missing=%lu duplicates=%lu "
                     "out-of-order=%lu malformed=%lu",
                     (unsigned long)unique_received, (unsigned long)missing_packets, (unsigned long)duplicate_packets,
                     (unsigned long)out_of_order_packets, (unsigned long)malformed_packets);

            for (uint16_t number = 1; number <= TEST_PACKET_COUNT; number++)
            {
                if (!seen[number])
                {
                    ESP_LOGW(TAG, "Missing test packet: %u", (unsigned int)number);
                }
            }

            const char *reason = "receive deadline reached";
            if (unique_received == TEST_PACKET_COUNT)
            {
                reason = "all expected packets received";
            }
            else if (no_packet_timeout)
            {
                reason = "no valid test packet arrived";
            }
            ESP_LOGI(TAG, "Run ended: %s", reason);
            ESP_LOGI(TAG, "Missing means not observed; compare with producer buffer failures.");
            /* Do not delete message buffers: the independent network task still uses them. */

            return;
        }

        if (xMessageBufferReceive(playback_buffer, &received_packet, sizeof(received_packet), pdMS_TO_TICKS(1000)) ==
            sizeof(received_packet))
        {
            if (received_packet.length != 4)
            {
                ESP_LOGD(TAG, "Received audio test frame with unexpected length: %u",
                         (unsigned int)received_packet.length);
                malformed_packets++;
                continue;
            }
            else if (received_payload[2] != 0xAA || received_payload[3] != 0x55)
            {
                ESP_LOGD(TAG, "Received audio test frame with unexpected payload pattern");
                malformed_packets++;
                continue;
            }

            uint16_t test_number = ((uint16_t)received_payload[0] << 8) | (uint16_t)received_payload[1];

            if (test_number < 1 || test_number > TEST_PACKET_COUNT || received_packet.sequence != test_number)
            {
                ESP_LOGD(TAG, "Received audio test frame with out-of-range test number: %u", (unsigned int)test_number);
                malformed_packets++;
                continue;
            }
            else
            {
                ESP_LOGD(TAG, "Received test packet %u", (unsigned int)test_number);
            }

            if (!run_started)
            {
                run_started = true;
                run_start_time = xTaskGetTickCount();
            }

            if (seen[test_number]) // duplicate packet check
            {
                duplicate_packets++;

                ESP_LOGD(TAG, "Duplicate test packet: %u", (unsigned int)test_number);
                continue;
            }

            seen[test_number] = true;
            unique_received++;

            if (test_number < highest_received) // out-of-order packet check
            {
                out_of_order_packets++;

                ESP_LOGD(TAG, "Out-of-order packet: %u, highest received: %u", (unsigned int)test_number,
                         (unsigned int)highest_received);
            }
            else
            {
                highest_received = test_number;
            }
        }
    }
#endif
}
#endif

/* ========================================================================== */
/* 6. Worker entry point: run the selected test on core 1.
 * ========================================================================== */

/* Build switches include only the chosen test path. Finite tests return here;
 * continuous streaming normally stays inside its worker until the board resets.
 */
static void relay_test_task(void *arg)
{
#if TEST_BENCHMARK
    benchmark_buffers_t *buffers = (benchmark_buffers_t *)arg;
#else
    MessageBufferHandle_t buffer = (MessageBufferHandle_t)arg;
#endif
    ESP_LOGI(TAG, "Relay/stream test worker running on core %d", (int)xPortGetCoreID());

#if TEST_BENCHMARK
    benchmark_run(buffers);
#elif TEST_CONTINUOUS_STREAM
#if TEST_IS_PRODUCER
    run_producer(buffer);
#else
    run_consumer(buffer);
#endif
#else
    run_numbered_test(buffer);
#endif

    ESP_LOGI(TAG, "Test worker minimum free stack: %lu B", (unsigned long)uxTaskGetStackHighWaterMark(NULL));

    /* Finite verification ends here; networking retains ownership of its buffers. */
    vTaskDelete(NULL);
}

/* ========================================================================== */
/* 7. Startup: allocate buffers, start networking, then create the test worker.
 * ========================================================================== */

/* Startup owns allocation and failure cleanup. Once networking starts, buffers
 * must stay alive even if the test worker finishes or cannot be created.
 */
void app_main(void)
{
    /* Test substitutes for audio capture/playback. FreeRTOS copies a complete
     * audio_frame_t into each message; no raw sample pointer crosses the buffer.
     */
    MessageBufferHandle_t capture_buffer = xMessageBufferCreate(NETWORK_CAPTURE_BUFFER_BYTES);

    MessageBufferHandle_t playback_buffer = xMessageBufferCreate(NETWORK_PLAYBACK_BUFFER_BYTES);

    if (capture_buffer == NULL || playback_buffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to create message buffers");

        if (capture_buffer != NULL)
        {
            vMessageBufferDelete(capture_buffer);
        }
        if (playback_buffer != NULL)
        {
            vMessageBufferDelete(playback_buffer);
        }
        return;
    }

    const esp_err_t init_result = mesh_network_init();

    /* Stop the test if initialization fails and print the readable error name. */
    if (init_result != ESP_OK)
    {
        ESP_LOGE(TAG, "Mesh startup failed: %s", esp_err_to_name(init_result));
        vMessageBufferDelete(capture_buffer);
        vMessageBufferDelete(playback_buffer);
        return;
    }

    const esp_err_t task_result = network_task_start(capture_buffer, playback_buffer);
    if (task_result != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start network task: %s", esp_err_to_name(task_result));
        vMessageBufferDelete(capture_buffer);
        vMessageBufferDelete(playback_buffer);
        return;
    }

    ESP_LOGI(TAG, "Network task started successfully");

/* Pass only the handles needed by the chosen test. Static bundle storage keeps
     * the benchmark task argument valid after app_main returns.
     */
#if TEST_BENCHMARK
    static benchmark_buffers_t buffers;
    buffers.capture = capture_buffer;
    buffers.playback = playback_buffer;
    void *worker_buffer = &buffers;
#elif TEST_IS_PRODUCER
    MessageBufferHandle_t worker_buffer = capture_buffer;
#else
    MessageBufferHandle_t worker_buffer = playback_buffer;
#endif

    BaseType_t worker_result = xTaskCreatePinnedToCore(relay_test_task, "relay_test", TEST_WORKER_STACK_BYTES,
                                                       worker_buffer, TEST_WORKER_PRIORITY, NULL, TEST_WORKER_CORE);

    if (worker_result != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create relay/stream test worker: insufficient memory");
        /* The network task is already running; its buffers must remain alive. */
        return;
    }

    /* app_main can return. Network and test worker retain the live buffers. */
}
