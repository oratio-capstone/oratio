#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_timer.h"
#include "unity.h"

#include "network.h"

/* Total UDP payload: 8 bytes of test header and 640 bytes of padding. */
#define PING_PACKET_SIZE 648

static const char *TAG = "TEST_106";

/* Pause between completed attempts; receiving and logging add to the actual interval. */
static const uint32_t STATUS_INTERVAL_MS = 5000;

/* Python echo server running on the laptop's hotspot interface. */
static const char *SERVER_IP = "192.168.137.1";
static const uint16_t SERVER_PORT = 8989;

/* Bound each receive call so a missing reply cannot block indefinitely. */
static const struct timeval receive_timeout =
    {
        .tv_sec = 2,
        .tv_usec = 0};

/* Used when selecting the finite loop below instead of continuous operation. */
static const uint32_t REQUEST_COUNT = 10;

void app_main(void)
{
    /*
     * Bytes 0-3 contain the "ORAT" marker; bytes 4-7 hold the sequence number.
     * All remaining bytes are automatically initialized to zero.
     * Static storage keeps these reusable buffers off the task stack.
     */
    static uint8_t tx_packet[PING_PACKET_SIZE] =
        {
            0x4F, 0x52, 0x41, 0x54, 0x00, 0x00, 0x00, 0x00};

    /*
     * One extra byte lets the length check detect oversized replies instead
     * of accepting a longer packet truncated to exactly the expected size.
     */
    static uint8_t rx_packet[PING_PACKET_SIZE + 1] = {0};

    uint32_t sequence_number = 1;

    /* Start the shared mesh network. This does not wait for an IP address or a usable route. */
    const esp_err_t result = mesh_network_init();

    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Mesh startup failed: %s", esp_err_to_name(result));
        return;
    }

    ESP_LOGI(TAG, "UDP test mesh startup requested");

    /*
     * Create one IPv4 UDP socket for both sending requests and receiving echoes.
     * A nonnegative descriptor identifies the socket; a negative result indicates failure.
     */
    int32_t socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (socket_fd < 0)
    {
        ESP_LOGE(TAG, "Failed to create UDP socket: %d", errno);
        return;
    }

    const int32_t timeout_result = setsockopt(
        socket_fd,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &receive_timeout,
        sizeof(receive_timeout));

    /* Release the socket if its required timeout cannot be configured. */
    if (timeout_result < 0)
    {
        ESP_LOGE(TAG, "Failed to set receive timeout: %d", errno);
        close(socket_fd);
        return;
    }

    /* Prepare the IPv4 destination, converting the port into network byte order. */
    struct sockaddr_in server_address =
        {
            .sin_family = AF_INET,
            .sin_port = htons(SERVER_PORT),
            .sin_addr = {0}};

    /* Convert the readable IP address into the binary representation used by sockets. */
    const int32_t inet_pton_result = inet_pton(
        AF_INET,
        SERVER_IP,
        &server_address.sin_addr);

    if (inet_pton_result < 0)
    {
        ESP_LOGE(TAG, "Address family isn't supported: %d", errno);
        close(socket_fd);
        return;
    }
    else if (inet_pton_result == 0)
    {
        ESP_LOGE(TAG, "Invalid IP address: %s", SERVER_IP);
        close(socket_fd);
        return;
    }
    else
    {
        ESP_LOGI(
            TAG,
            "Server address set to: %s:%u",
            SERVER_IP,
            (unsigned int)SERVER_PORT);
    }

    /* recvfrom() writes the responding device's IP address and port into this structure. */
    struct sockaddr_in reply_address = {0};
    socklen_t reply_address_length = sizeof(reply_address);

    /* Cumulative counts for this run; these reset when the program restarts. */
    uint32_t send_attempts = 0;
    uint32_t successful_sends = 0;
    uint32_t send_failures = 0;
    uint32_t successful_replies = 0;
    uint32_t timeout_requests = 0;
    uint32_t receive_failures = 0;
    uint32_t invalid_replies = 0;

    /* Use only one loop condition: continuous operation or a fixed number of attempts. */
    while (true)
    // while (send_attempts < REQUEST_COUNT)
    {
        /* Encode the 32-bit sequence number with the most significant byte first. */
        tx_packet[4] = (uint8_t)((sequence_number >> 24) & 0xFF);
        tx_packet[5] = (uint8_t)((sequence_number >> 16) & 0xFF);
        tx_packet[6] = (uint8_t)((sequence_number >> 8) & 0xFF);
        tx_packet[7] = (uint8_t)(sequence_number & 0xFF);

        send_attempts++;

        /* Start timing immediately before sending; avoid console logging until after receiving. */
        const int64_t send_time_us = esp_timer_get_time();

        const ssize_t bytes_sent = sendto(
            socket_fd,
            tx_packet,
            sizeof(tx_packet),
            0,
            (const struct sockaddr *)&server_address,
            sizeof(server_address));

        if (bytes_sent < 0)
        {
            ESP_LOGE(TAG, "Failed to send UDP packet: %d", errno);
            send_failures++;
        }
        else if (bytes_sent != (ssize_t)sizeof(tx_packet))
        {
            ESP_LOGW(
                TAG,
                "Sent %zd bytes, expected to send %zu bytes",
                bytes_sent,
                sizeof(tx_packet));
            send_failures++;
        }
        else
        {
            /* Local acceptance for sending does not prove that Python received the request. */
            successful_sends++;

            /* Reset the address capacity because recvfrom() can update this value. */
            reply_address_length = sizeof(reply_address);

            /*
             * Wait for one incoming packet using the same socket.
             * This implementation ends the attempt after that packet, even if it is invalid.
             */
            const ssize_t bytes_received = recvfrom(
                socket_fd,
                rx_packet,
                sizeof(rx_packet),
                0,
                (struct sockaddr *)&reply_address,
                &reply_address_length);

            /* Preserve the socket error before another call, then capture the receive timestamp. */
            const int32_t receive_error = errno;
            const int64_t receive_time_us = esp_timer_get_time();

            if (bytes_received < 0)
            {
                /* A timeout is tracked separately from other receive errors. */
                if (receive_error == EAGAIN || receive_error == EWOULDBLOCK)
                {
                    ESP_LOGW(TAG, "Receive operation timed out");
                    timeout_requests++;
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to receive UDP packet: %d", receive_error);
                    receive_failures++;
                }
            }
            else if (bytes_received != (ssize_t)sizeof(tx_packet))
            {
                /* An echo must have exactly the same length as the original request. */
                ESP_LOGW(
                    TAG,
                    "Received %zd bytes, expected to receive %zu bytes",
                    bytes_received,
                    sizeof(tx_packet));
                invalid_replies++;
            }
            else if (reply_address.sin_addr.s_addr != server_address.sin_addr.s_addr ||
                     reply_address.sin_port != server_address.sin_port)
            {
                /* Reject packets from any IP address or port other than the configured server. */
                invalid_replies++;
                char reply_ip[INET_ADDRSTRLEN] = {0};

                if (inet_ntop(AF_INET, &reply_address.sin_addr, reply_ip, sizeof(reply_ip)) == NULL)
                {
                    ESP_LOGW(TAG, "Received packet from unexpected source; IP conversion failed");
                }
                else
                {
                    ESP_LOGW(
                        TAG,
                        "Received packet from unexpected source: %s:%u",
                        reply_ip,
                        (unsigned int)ntohs(reply_address.sin_port));
                }
            }
            else if (memcmp(tx_packet, rx_packet, sizeof(tx_packet)) != 0)
            {
                /* Compare every byte, including the marker, sequence number, and padding. */
                ESP_LOGW(TAG, "Received packet does not match sent packet");
                invalid_replies++;
            }
            else
            {
                /*
                 * Report RTT only for a verified echo.
                 * It includes local processing, the network path, and Python's echo processing.
                 */
                const int64_t rtt_us = receive_time_us - send_time_us;

                ESP_LOGI(
                    TAG,
                    "Verified UDP packet received, sequence: %u, RTT: %" PRId64 " us",
                    (unsigned int)sequence_number,
                    rtt_us);

                successful_replies++;
            }

            /*
             * Advance after every successful send, including attempts with missing or invalid replies.
             * A failed send retains its sequence number for the next attempt.
             */
            sequence_number++;
        }

        /* Report all outcomes together so failures can be distinguished from verified round trips. */
        ESP_LOGI(
            TAG,
            "Status: Attempts: %u, Successful Sends: %u, Send Failures: %u, "
            "Successful Replies: %u, Timeouts: %u, "
            "Receive Failures: %u, Invalid Replies: %u",
            (unsigned int)send_attempts,
            (unsigned int)successful_sends,
            (unsigned int)send_failures,
            (unsigned int)successful_replies,
            (unsigned int)timeout_requests,
            (unsigned int)receive_failures,
            (unsigned int)invalid_replies);

        /* Record mesh context for the measurements and yield execution between attempts. */
        mesh_network_log_status();
        vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
    }

    /* Reached after a finite run; the continuous loop does not normally exit. */
    close(socket_fd);
}