/* Threaded test double, not an emulation of the ESP-IDF scheduler or DMA. */
#include "host_runtime.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "HOST_PLAYBACK";

struct host_queue
{
    size_t capacity;
    size_t item_size;
    size_t count;
    uint8_t data[];
};

struct host_semaphore
{
    pthread_mutex_t lock;
    pthread_cond_t condition;
    bool given;
};

struct host_task
{
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    uint32_t notifications;
    bool deleted;
    void (*entry)(void *);
    void *argument;
};

unsigned int host_fail_allocation;
unsigned int host_live_resources;
esp_err_t host_init_result;
esp_err_t host_start_result;
esp_err_t host_cleanup_result;
uint32_t host_init_delay_ms;
size_t host_output_calls;
size_t host_sample_count;
int16_t host_samples[32000];

static _Thread_local TaskHandle_t s_current_task;
static pthread_mutex_t s_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_gate_condition = PTHREAD_COND_INITIALIZER;
static bool s_waiting;
static bool s_release;
static bool s_free_run;
static bool s_output_owned;
static size_t s_accept;
static esp_err_t s_write_result;
static int64_t s_elapsed;
static int64_t s_clock_us;
static size_t s_write_generation;

static struct timespec deadline_ms(uint32_t ms)
{
    struct timespec value;
    (void)clock_gettime(CLOCK_REALTIME, &value);
    value.tv_sec += (time_t)(ms / 1000);
    value.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (value.tv_nsec >= 1000000000L)
    {
        value.tv_sec++;
        value.tv_nsec -= 1000000000L;
    }
    return value;
}

static void *allocate(size_t bytes)
{
    if (host_fail_allocation != 0 && --host_fail_allocation == 0)
    {
        return NULL;
    }
    void *memory = calloc(1, bytes);
    assert(memory != NULL);
    host_live_resources++;
    return memory;
}

QueueHandle_t xQueueCreate(UBaseType_t count, UBaseType_t size)
{
    QueueHandle_t queue = allocate(sizeof(*queue) + count * size);
    if (queue != NULL)
    {
        queue->capacity = count;
        queue->item_size = size;
    }
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait)
{
    assert(wait == 0);
    if (queue->count == queue->capacity)
    {
        return pdFALSE;
    }
    memcpy(queue->data + queue->count * queue->item_size, item, queue->item_size);
    queue->count++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    assert(wait == 0);
    if (queue->count == 0)
    {
        return pdFALSE;
    }
    memcpy(item, queue->data, queue->item_size);
    queue->count--;
    memmove(queue->data, queue->data + queue->item_size, queue->count * queue->item_size);
    return pdTRUE;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue)
{
    return (UBaseType_t)queue->count;
}

BaseType_t xQueueReset(QueueHandle_t queue)
{
    queue->count = 0;
    return pdTRUE;
}

void vQueueDelete(QueueHandle_t queue)
{
    free(queue);
    host_live_resources--;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    SemaphoreHandle_t semaphore = allocate(sizeof(*semaphore));
    if (semaphore != NULL)
    {
        assert(pthread_mutex_init(&semaphore->lock, NULL) == 0);
        assert(pthread_cond_init(&semaphore->condition, NULL) == 0);
    }
    return semaphore;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    pthread_mutex_lock(&semaphore->lock);
    semaphore->given = true;
    pthread_cond_signal(&semaphore->condition);
    pthread_mutex_unlock(&semaphore->lock);
    return pdTRUE;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait)
{
    const struct timespec deadline = deadline_ms(wait);
    pthread_mutex_lock(&semaphore->lock);
    while (!semaphore->given)
    {
        if (pthread_cond_timedwait(&semaphore->condition, &semaphore->lock, &deadline) != 0)
        {
            pthread_mutex_unlock(&semaphore->lock);
            return pdFALSE;
        }
    }
    semaphore->given = false;
    pthread_mutex_unlock(&semaphore->lock);
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    assert(pthread_cond_destroy(&semaphore->condition) == 0);
    assert(pthread_mutex_destroy(&semaphore->lock) == 0);
    free(semaphore);
    host_live_resources--;
}

static void *task_entry(void *argument)
{
    s_current_task = argument;
    s_current_task->entry(s_current_task->argument);
    return NULL;
}

BaseType_t xTaskCreate(void (*entry)(void *), const char *name, uint32_t stack, void *arg,
                      UBaseType_t priority, TaskHandle_t *task)
{
    (void)name;
    assert(stack >= 4096 && priority == tskIDLE_PRIORITY + 2);
    *task = allocate(sizeof(**task));
    if (*task == NULL)
    {
        return pdFALSE;
    }
    (*task)->entry = entry;
    (*task)->argument = arg;
    assert(pthread_mutex_init(&(*task)->lock, NULL) == 0);
    assert(pthread_cond_init(&(*task)->condition, NULL) == 0);
    assert(pthread_create(&(*task)->thread, NULL, task_entry, *task) == 0);
    return pdPASS;
}

void xTaskNotifyGive(TaskHandle_t task)
{
    pthread_mutex_lock(&task->lock);
    task->notifications++;
    pthread_cond_signal(&task->condition);
    pthread_mutex_unlock(&task->lock);
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    assert(clear == pdTRUE && wait == portMAX_DELAY);
    TaskHandle_t task = s_current_task;
    pthread_mutex_lock(&task->lock);
    while (task->notifications == 0 && !task->deleted)
    {
        pthread_cond_wait(&task->condition, &task->lock);
    }
    if (task->deleted)
    {
        pthread_mutex_unlock(&task->lock);
        pthread_exit(NULL);
    }
    const uint32_t count = task->notifications;
    task->notifications = 0;
    pthread_mutex_unlock(&task->lock);
    return count;
}

void vTaskDelete(TaskHandle_t task)
{
    pthread_mutex_lock(&task->lock);
    task->deleted = true;
    pthread_cond_signal(&task->condition);
    pthread_mutex_unlock(&task->lock);
    assert(pthread_join(task->thread, NULL) == 0);
    pthread_mutex_destroy(&task->lock);
    pthread_cond_destroy(&task->condition);
    free(task);
    host_live_resources--;
}

TickType_t xTaskGetTickCount(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (TickType_t)((uint64_t)value.tv_sec * 1000 + (uint64_t)value.tv_nsec / 1000000);
}

void vTaskDelay(TickType_t ticks)
{
    const struct timespec delay = {.tv_sec = (time_t)(ticks / 1000),
                                   .tv_nsec = (long)(ticks % 1000) * 1000000L};
    nanosleep(&delay, NULL);
}

int64_t esp_timer_get_time(void)
{
    pthread_mutex_lock(&s_gate);
    const int64_t value = s_clock_us;
    pthread_mutex_unlock(&s_gate);
    return value;
}

esp_err_t audio_output_init(const audio_output_config_t *config)
{
    assert(s_current_task != NULL && !s_output_owned);
    assert(config->bclk_gpio == 4 && config->ws_gpio == 5 && config->dout_gpio == 7);
    vTaskDelay(host_init_delay_ms);
    s_output_owned = host_init_result == ESP_OK;
    return host_init_result;
}

esp_err_t audio_output_start(void)
{
    assert(s_current_task != NULL && s_output_owned);
    return host_start_result;
}

esp_err_t audio_output_deinit(void)
{
    assert(s_current_task != NULL);
    if (host_cleanup_result == ESP_OK)
    {
        s_output_owned = false;
    }
    return host_cleanup_result;
}

esp_err_t audio_output_write(const int16_t *samples, size_t count, size_t *written, uint32_t timeout)
{
    assert(s_current_task != NULL && s_output_owned && count > 0 && timeout > 0 && timeout <= 20);
    pthread_mutex_lock(&s_gate);
    s_waiting = true;
    pthread_cond_broadcast(&s_gate_condition);
    while (!s_release && !s_free_run)
    {
        pthread_cond_wait(&s_gate_condition, &s_gate);
    }
    *written = s_free_run ? count : (s_accept < count ? s_accept : count);
    assert(host_sample_count + *written <= sizeof(host_samples) / sizeof(host_samples[0]));
    memcpy(host_samples + host_sample_count, samples, *written * sizeof(*samples));
    host_sample_count += *written;
    host_output_calls++;
    s_clock_us += s_free_run ? 20000 : s_elapsed;
    const esp_err_t result = s_free_run ? ESP_OK : s_write_result;
    s_waiting = false;
    s_release = false;
    s_write_generation++;
    pthread_cond_broadcast(&s_gate_condition);
    pthread_mutex_unlock(&s_gate);
    vTaskDelay(1);
    return result;
}

void host_reset(void)
{
    assert(host_live_resources == 0 && !s_output_owned);
    host_fail_allocation = 0;
    host_init_result = ESP_OK;
    host_start_result = ESP_OK;
    host_cleanup_result = ESP_OK;
    host_init_delay_ms = 0;
    host_output_calls = 0;
    host_sample_count = 0;
    s_waiting = false;
    s_release = false;
    s_free_run = false;
    s_clock_us = 0;
    ESP_LOGI(TAG, "Reset host runtime");
}

void host_wait_write(void)
{
    const struct timespec deadline = deadline_ms(3000);
    pthread_mutex_lock(&s_gate);
    while (!s_waiting)
    {
        assert(pthread_cond_timedwait(&s_gate_condition, &s_gate, &deadline) == 0);
    }
    pthread_mutex_unlock(&s_gate);
}

void host_release_write(size_t count, esp_err_t result, int64_t elapsed_us)
{
    host_wait_write();
    pthread_mutex_lock(&s_gate);
    const size_t generation = s_write_generation;
    s_accept = count;
    s_write_result = result;
    s_elapsed = elapsed_us;
    s_release = true;
    pthread_cond_broadcast(&s_gate_condition);
    while (generation == s_write_generation)
    {
        pthread_cond_wait(&s_gate_condition, &s_gate);
    }
    pthread_mutex_unlock(&s_gate);
}

void host_wait_idle(void)
{
    const TickType_t begin = xTaskGetTickCount();
    audio_playback_stats_t stats;
    do
    {
        assert(xTaskGetTickCount() - begin < 3000);
        vTaskDelay(1);
        assert(audio_playback_get_stats(&stats) == ESP_OK);
    } while (stats.state != AUDIO_PLAYBACK_FAULTED && stats.state != AUDIO_PLAYBACK_STOPPED);
}

void host_finish(void)
{
    pthread_mutex_lock(&s_gate);
    s_free_run = true;
    pthread_cond_broadcast(&s_gate_condition);
    pthread_mutex_unlock(&s_gate);
    assert(audio_playback_deinit() == ESP_OK);
    assert(host_live_resources == 0 && !s_output_owned);
}
