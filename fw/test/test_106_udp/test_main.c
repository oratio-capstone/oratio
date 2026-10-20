#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "network.h"

#define PING_PACKET_SIZE 8

static const char *TAG = "TEST_106";
static const uint32_t STATUS_INTERVAL_MS = 5000;
static const char *SERVER_IP = "192.168.5.3";
static const uint16_t SERVER_PORT = 8989;

static const struct timeval receive_timeout =
    {
        .tv_sec = 2,
        .tv_usec = 0};

void app_main(void)
{
    uint8_t tx_packet[PING_PACKET_SIZE] =
        {
            0x4F, 0x52, 0x41, 0x54, 0x00, 0x00, 0x00, 0x00};

    uint8_t rx_packet[PING_PACKET_SIZE + 1] = {0};

    uint32_t sequence_number = 1;

    const esp_err_t result = mesh_network_init();

    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Mesh startup failed: %s", esp_err_to_name(result));
        return;
    }

    ESP_LOGI(TAG, "UDP test mesh startup requested");

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

    if (timeout_result < 0)
    {
        ESP_LOGE(TAG, "Failed to set receive timeout: %d", errno);
        close(socket_fd);
        return;
    }

    struct sockaddr_in server_address =
        {
            .sin_family = AF_INET,
            .sin_port = htons(SERVER_PORT),
            .sin_addr = {0}};

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

    struct sockaddr_in reply_address = {0};
    socklen_t reply_address_length = sizeof(reply_address);

    while (true)
    {
        tx_packet[4] = (uint8_t)((sequence_number >> 24) & 0xFF);
        tx_packet[5] = (uint8_t)((sequence_number >> 16) & 0xFF);
        tx_packet[6] = (uint8_t)((sequence_number >> 8) & 0xFF);
        tx_packet[7] = (uint8_t)(sequence_number & 0xFF);

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
        }
        else if (bytes_sent != (ssize_t)sizeof(tx_packet))
        {
            ESP_LOGW(
                TAG,
                "Sent %zd bytes, expected to send %zu bytes",
                bytes_sent,
                sizeof(tx_packet));
        }
        else
        {
            ESP_LOGI(
                TAG,
                "UDP packet accepted for sending, sequence: %u",
                (unsigned int)sequence_number);

            reply_address_length = sizeof(reply_address);

            const ssize_t bytes_received = recvfrom(
                socket_fd,
                rx_packet,
                sizeof(rx_packet),
                0,
                (struct sockaddr *)&reply_address,
                &reply_address_length);

            if (bytes_received < 0)
            {
                const int32_t receive_error = errno;
                if (receive_error == EAGAIN || receive_error == EWOULDBLOCK)
                {
                    ESP_LOGW(TAG, "Receive operation timed out");
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to receive UDP packet: %d", receive_error);
                }
            }
            else if (bytes_received != (ssize_t)sizeof(tx_packet))
            {
                ESP_LOGW(
                    TAG,
                    "Received %zd bytes, expected to receive %zu bytes",
                    bytes_received,
                    sizeof(tx_packet));
            }
            else if (reply_address.sin_addr.s_addr != server_address.sin_addr.s_addr ||
                     reply_address.sin_port != server_address.sin_port)
            {
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
                ESP_LOGW(TAG, "Received packet does not match sent packet");
            }
            else
            {
                ESP_LOGI(
                    TAG,
                    "Verified UDP packet received, sequence: %u",
                    (unsigned int)sequence_number);
            }

            sequence_number++;
        }

        mesh_network_log_status();
        vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
    }
}