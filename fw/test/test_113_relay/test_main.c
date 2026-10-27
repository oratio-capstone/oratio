#include "network.h"
#include <stdbool.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define TEST_PACKET_COUNT 1000
#define TEST_SEND_INTERVAL_MS 100
#define TEST_STARTUP_DELAY_MS 60000
/* Allow time to flash/start the producer after starting the consumer. */
#define TEST_FIRST_PACKET_TIMEOUT_MS 300000
#define TEST_RECEIVE_TIMEOUT_MS \
    (TEST_PACKET_COUNT * TEST_SEND_INTERVAL_MS + 30000)

#ifndef TEST_IS_PRODUCER
#error "Define TEST_IS_PRODUCER as 0 or 1 in the test environment"
#elif (TEST_IS_PRODUCER != 0) && (TEST_IS_PRODUCER != 1)
#error "TEST_IS_PRODUCER must be 0 or 1"
#endif

static const char *TAG = "TEST_113";

void app_main(void)
{
    QueueHandle_t capture_queue =
        xQueueCreate(4, sizeof(oratio_tx_item_t));

    QueueHandle_t playback_queue =
        xQueueCreate(4, sizeof(oratio_packet_t));

    if (capture_queue == NULL || playback_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create queues");

        if (capture_queue != NULL)
        {
            vQueueDelete(capture_queue);
        }
        if (playback_queue != NULL)
        {
            vQueueDelete(playback_queue);
        }
        return;
    }

    const esp_err_t init_result = mesh_network_init();

    /* Stop the test if initialization fails and print the readable error name. */
    if (init_result != ESP_OK)
    {
        ESP_LOGE(TAG, "Mesh startup failed: %s", esp_err_to_name(init_result));
        vQueueDelete(capture_queue);
        vQueueDelete(playback_queue);
        return;
    }

    const esp_err_t task_result = network_task_start(capture_queue, playback_queue);
    if (task_result != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start network task: %s", esp_err_to_name(task_result));
        vQueueDelete(capture_queue);
        vQueueDelete(playback_queue);
        return;
    }

    ESP_LOGI(TAG, "Network task started successfully");

#if TEST_IS_PRODUCER
    /* Start the consumer first and confirm gateway registration before this send. */
    oratio_tx_item_t test_item = {
        .type = ORATIO_PACKET_TELEMETRY,
        .length = 4};

    ESP_LOGI(TAG, "Waiting %u seconds before starting the numbered packet run",
             (unsigned int)(TEST_STARTUP_DELAY_MS / 1000));
    /* The network task keeps sending heartbeats during this delay. */
    vTaskDelay(pdMS_TO_TICKS(TEST_STARTUP_DELAY_MS));
    ESP_LOGI(TAG, "Starting numbered packet run");

    uint32_t queued_packets = 0;
    uint32_t queue_failures = 0;

    for (uint16_t test_number = 1; // producer loop
         test_number <= TEST_PACKET_COUNT;
         test_number++)
    {
        test_item.payload[0] = (uint8_t)(test_number >> 8);
        test_item.payload[1] = (uint8_t)(test_number & 0xFF);
        test_item.payload[2] = 0xAA;
        test_item.payload[3] = 0x55;

        /* Queue the item and check whether it succeeded. */
        if (xQueueSend(capture_queue, &test_item, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            queued_packets++;
            ESP_LOGI(TAG, "Test packet %u queued", (unsigned int)test_number);
        }
        else
        {
            queue_failures++;
            ESP_LOGE(TAG, "Capture queue full; test payload not queued");
        }

        vTaskDelay(pdMS_TO_TICKS(TEST_SEND_INTERVAL_MS));
    }

    ESP_LOGI(TAG, "Producer finished: queued=%lu, queue failures=%lu",
             (unsigned long)queued_packets,
             (unsigned long)queue_failures);
    /* Networking keeps running. Its queues must remain alive. */
    return;
#endif

    static oratio_packet_t received_packet;

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
    uint16_t producer_node_id = 0;
    uint32_t foreign_packets = 0;

    ESP_LOGI(TAG, "Consumer ready: expecting %u packets; first-packet timeout %u s",
             (unsigned int)TEST_PACKET_COUNT,
             (unsigned int)(TEST_FIRST_PACKET_TIMEOUT_MS / 1000));

    while (true) // Consumer loop
    {
        TickType_t now = xTaskGetTickCount();
        bool timed_out = run_started &&
                         (TickType_t)(now - run_start_time) >= pdMS_TO_TICKS(TEST_RECEIVE_TIMEOUT_MS);
        bool no_packet_timeout = !run_started &&
                                 (TickType_t)(now - first_packet_wait_start) >=
                                     pdMS_TO_TICKS(TEST_FIRST_PACKET_TIMEOUT_MS);

        if (unique_received == TEST_PACKET_COUNT || timed_out || no_packet_timeout)
        {
            uint32_t missing_packets =
                TEST_PACKET_COUNT - unique_received;

            ESP_LOGI(TAG,
                     "Summary: unique=%lu missing=%lu duplicates=%lu "
                     "out-of-order=%lu malformed=%lu foreign=%lu",
                     (unsigned long)unique_received,
                     (unsigned long)missing_packets,
                     (unsigned long)duplicate_packets,
                     (unsigned long)out_of_order_packets,
                     (unsigned long)malformed_packets,
                     (unsigned long)foreign_packets);

            for (uint16_t number = 1; number <= TEST_PACKET_COUNT; number++)
            {
                if (!seen[number])
                {
                    ESP_LOGW(TAG, "Missing test packet: %u",
                             (unsigned int)number);
                }
            }

            ESP_LOGI(TAG, "Run ended: %s",
                     unique_received == TEST_PACKET_COUNT ? "all expected packets received" : no_packet_timeout ? "no valid test packet arrived"
                                                                                                                : "receive deadline reached");
            ESP_LOGI(TAG, "Missing means not observed; compare with producer queue failures.");
            /* Do not delete queues: the independent network task still owns them. */

            return;
        }

        if (xQueueReceive(playback_queue, &received_packet, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            ESP_LOGI(TAG, "Consumer received: type=%c node=%04x sequence=%u length=%u",
                     received_packet.type, (unsigned int)received_packet.node_id,
                     (unsigned int)received_packet.sequence,
                     (unsigned int)received_packet.length);

            if (received_packet.type != ORATIO_PACKET_TELEMETRY) // malformed packet check
            {
                ESP_LOGI(TAG, "Received packet type is not telemetry; ignoring payload");
                continue;
            }
            else if (received_packet.length != 4)
            {
                ESP_LOGI(TAG, "Received telemetry packet with unexpected length: %u",
                         (unsigned int)received_packet.length);
                malformed_packets++;
                continue;
            }
            else if (received_packet.payload[2] != 0xAA || received_packet.payload[3] != 0x55)
            {
                ESP_LOGI(TAG, "Received telemetry packet with unexpected payload pattern");
                malformed_packets++;
                continue;
            }

            uint16_t test_number = ((uint16_t)received_packet.payload[0] << 8) |
                                   (uint16_t)received_packet.payload[1];

            if (test_number < 1 || test_number > TEST_PACKET_COUNT)
            {
                ESP_LOGI(TAG, "Received telemetry packet with out-of-range test number: %u",
                         (unsigned int)test_number);
                malformed_packets++;
                continue;
            }
            else
            {
                ESP_LOGI(TAG, "Received test packet %u from node %04x",
                         (unsigned int)test_number,
                         (unsigned int)received_packet.node_id);
            }

            if (!run_started)
            {
                run_started = true;
                run_start_time = xTaskGetTickCount();
                producer_node_id = received_packet.node_id;
                ESP_LOGI(TAG, "Tracking producer node %04x", (unsigned int)producer_node_id);
            }
            else if (received_packet.node_id != producer_node_id)
            {
                foreign_packets++;
                ESP_LOGW(TAG, "Ignoring test packet from another producer node %04x",
                         (unsigned int)received_packet.node_id);
                continue;
            }

            if (seen[test_number]) // duplicate packet check
            {
                duplicate_packets++;

                ESP_LOGW(TAG, "Duplicate test packet: %u",
                         (unsigned int)test_number);
                continue;
            }

            seen[test_number] = true;
            unique_received++;

            if (test_number < highest_received) // out-of-order packet check
            {
                out_of_order_packets++;

                ESP_LOGW(TAG, "Out-of-order packet: %u, highest received: %u",
                         (unsigned int)test_number,
                         (unsigned int)highest_received);
            }
            else
            {
                highest_received = test_number;
            }
        }
    }
}