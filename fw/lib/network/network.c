#include "network.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "sdkconfig.h"
#include "esp_bridge.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_mesh_lite.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

/* ========================================================================== */
/* 1. Build configuration and shared state                                     */
/* ========================================================================== */

/* Stop compilation if the build environment has not selected a valid firmware role. */
#ifndef APP_IS_ROOT
#error "APP_IS_ROOT must be supplied by the build environment"
#endif

#if (APP_IS_ROOT != 0) && (APP_IS_ROOT != 1)
#error "APP_IS_ROOT must be 0 or 1"
#endif

#if !defined(CONFIG_MESH_LITE_ENABLE)
#error "Enable Mesh-Lite in menuconfig"
#endif

/* A Wi-Fi SSID holds up to 32 bytes, plus one byte for the string terminator. */
#define SOFTAP_SSID_BUFFER_SIZE 33
#define HEARTBEAT_INTERVAL_MS 5000
#define SOCKET_RETRY_MS 1000
#define ORATIO_UDP_PORT 8989
#define ORATIO_SERVER_IP "192.168.137.1"

static const char *TAG = "MESH_NETWORK";
static const bool IS_ROOT = (APP_IS_ROOT == 1);
static const uint8_t ROOT_LEVEL = 1;

/* Initial SoftAP channel; the upstream connection may determine the operating channel later. */
static const uint8_t MESH_CHANNEL = 6;

/* Keep the generated network name available for configuration and later status logs. */
static char s_softap_ssid[SOFTAP_SSID_BUFFER_SIZE];

static QueueHandle_t s_capture_queue = NULL;
static QueueHandle_t s_playback_queue = NULL;
static TaskHandle_t s_network_task_handle = NULL;

/* ========================================================================== */
/* 2. Mesh setup and diagnostics                                               */
/* ========================================================================== */

/* Initialize nonvolatile storage, which Wi-Fi and Mesh-Lite use for persistent settings. */
static esp_err_t storage_init(void)
{
    esp_err_t result = nvs_flash_init();

    /* These initialization errors require erasing the NVS partition before retrying. */
    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "Reinitializing NVS; stored settings will be erased");

        ESP_RETURN_ON_ERROR(
            nvs_flash_erase(), TAG, "Failed to erase NVS");

        result = nvs_flash_init();
    }

    return result;
}

/* Configure the station interface for joining Wi-Fi and the SoftAP interface for mesh children. */
static esp_err_t mesh_wifi_configure(void)
{
    wifi_config_t station_config = {0};

    /* Only the root receives laptop hotspot credentials; other nodes find a mesh parent. */
    if (IS_ROOT)
    {
        const size_t ssid_length = strlen(CONFIG_ORATIO_ROUTER_SSID);
        const size_t password_length = strlen(CONFIG_ORATIO_ROUTER_PASSWORD);

        /* Reject an empty SSID or credentials that will not fit in the destination arrays. */
        if (ssid_length == 0 ||
            ssid_length > sizeof(station_config.sta.ssid) ||
            password_length >= sizeof(station_config.sta.password))
        {
            ESP_LOGE(TAG, "Hotspot credentials are empty or exceed supported lengths");
            return ESP_ERR_INVALID_ARG;
        }

        memcpy(
            station_config.sta.ssid,
            CONFIG_ORATIO_ROUTER_SSID,
            ssid_length);

        memcpy(
            station_config.sta.password,
            CONFIG_ORATIO_ROUTER_PASSWORD,
            password_length);
    }

    ESP_RETURN_ON_ERROR(
        esp_bridge_wifi_set_config(WIFI_IF_STA, &station_config),
        TAG,
        "Failed to configure station");

    /* Provide the initial Wi-Fi settings for the access point hosted by this board. */
    wifi_config_t softap_config =
        {
            .ap =
                {
                    .ssid = CONFIG_BRIDGE_SOFTAP_SSID,
                    .password = CONFIG_BRIDGE_SOFTAP_PASSWORD,
                    .channel = MESH_CHANNEL,
                },
    };

    return esp_bridge_wifi_set_config(WIFI_IF_AP, &softap_config);
}

/* Give each board a recognizable SoftAP name using the last three bytes of its MAC address. */
static esp_err_t mesh_softap_configure(void)
{
    uint8_t softap_mac[6] = {0};

    ESP_RETURN_ON_ERROR(
        esp_wifi_get_mac(WIFI_IF_AP, softap_mac),
        TAG,
        "Failed to read SoftAP MAC");

    /*
     * Limit the base name to 25 characters so the underscore and six hexadecimal
     * characters fit within the 32-byte SSID limit. snprintf writes into the buffer.
     */
    snprintf(
        s_softap_ssid,
        sizeof(s_softap_ssid),
        "%.25s_%02x%02x%02x",
        CONFIG_BRIDGE_SOFTAP_SSID,
        softap_mac[3],
        softap_mac[4],
        softap_mac[5]);

    return esp_mesh_lite_set_softap_info(
        s_softap_ssid, CONFIG_BRIDGE_SOFTAP_PASSWORD);
}

/*
 * Initialize the shared networking services, configure the selected role, and start Mesh-Lite.
 * ESP_RETURN_ON_ERROR logs and returns an error if a checked initialization step fails.
 */
esp_err_t mesh_network_init(void)
{
    ESP_RETURN_ON_ERROR(
        storage_init(), TAG, "Storage initialization failed");

    ESP_RETURN_ON_ERROR(
        esp_netif_init(), TAG, "Network interface initialization failed");

    /* The default event loop delivers Wi-Fi and IP events to registered handlers. */
    ESP_RETURN_ON_ERROR(
        esp_event_loop_create_default(), TAG, "Event loop creation failed");

    /* Create the network interfaces enabled in the IoT-Bridge configuration. */
    esp_bridge_create_all_netif();

    ESP_RETURN_ON_ERROR(
        mesh_wifi_configure(), TAG, "Wi-Fi configuration failed");

    esp_mesh_lite_config_t mesh_config = ESP_MESH_LITE_DEFAULT_INIT();

    /*
     * Allow mesh formation without requiring an existing router connection.
     * Non-root nodes can join without their own router credentials.
     * Keeping leaf_node false allows a node to serve as a relay for children.
     */
    mesh_config.join_mesh_ignore_router_status = true;
    mesh_config.join_mesh_without_configured_wifi = !IS_ROOT;
    mesh_config.leaf_node = false;

    esp_mesh_lite_init(&mesh_config);

    /*
     * Also supply the root's upstream credentials to Mesh-Lite itself.
     * The earlier Wi-Fi configuration step has already validated their lengths.
     * This API must be called after Mesh-Lite initialization and before startup.
     */
    if (IS_ROOT)
    {
        mesh_lite_sta_config_t router_config = {0};

        memcpy(
            router_config.ssid,
            CONFIG_ORATIO_ROUTER_SSID,
            strlen(CONFIG_ORATIO_ROUTER_SSID));

        memcpy(
            router_config.password,
            CONFIG_ORATIO_ROUTER_PASSWORD,
            strlen(CONFIG_ORATIO_ROUTER_PASSWORD));

        ESP_RETURN_ON_ERROR(
            esp_mesh_lite_set_router_config(&router_config),
            TAG,
            "Failed to configure Mesh-Lite router");
    }

    ESP_RETURN_ON_ERROR(
        mesh_softap_configure(), TAG, "Mesh SoftAP configuration failed");

    /* Enforce a fixed root rather than allowing non-root devices to become root. */
    if (IS_ROOT)
    {
        ESP_RETURN_ON_ERROR(
            esp_mesh_lite_set_allowed_level(ROOT_LEVEL),
            TAG,
            "Failed to restrict root to level 1");
    }
    else
    {
#if defined(APP_FORCE_LEVEL3) && (APP_FORCE_LEVEL3 == 1)
        /* Temporary test restriction: this board must join through a level-2 parent. */
        ESP_RETURN_ON_ERROR(
            esp_mesh_lite_set_allowed_level(3),
            TAG,
            "Failed to restrict test node to level 3");
#else
        /* Ordinary nodes may choose a suitable mesh level, but cannot become level 1. */
        ESP_RETURN_ON_ERROR(
            esp_mesh_lite_set_disallowed_level(ROOT_LEVEL),
            TAG,
            "Failed to prevent node from becoming root");
#endif
    }

    /* Connection and topology formation continue asynchronously after this call. */
    esp_mesh_lite_start();

    /* Successful initialization does not yet guarantee an IP address or server connectivity. */
    return ESP_OK;
}

/* Log this board's role and mesh level, plus parent information for non-root nodes. */
void mesh_network_log_status(void)
{
    const uint8_t level = esp_mesh_lite_get_level();

    ESP_LOGI(
        TAG,
        "Firmware role: %s | mesh level: %u | SoftAP: %s",
        IS_ROOT ? "root" : "node",
        (unsigned int)level,
        s_softap_ssid);

    /* Skip parent reporting for the root, even when it has an upstream hotspot connection. */
    if (IS_ROOT)
    {
        return;
    }

    wifi_ap_record_t parent_info = {0};

    /* A node without a current Wi-Fi parent has no parent details to report. */
    if (esp_wifi_sta_get_ap_info(&parent_info) != ESP_OK)
    {
        return;
    }

    /* BSSID identifies the parent's access point; RSSI reports received signal strength. */
    ESP_LOGI(
        TAG,
        "Parent BSSID: " MACSTR " | RSSI: %d dBm",
        MAC2STR(parent_info.bssid),
        parent_info.rssi);
}

/* ========================================================================== */
/* 3. Packet encoding and decoding                                             */
/* ========================================================================== */

esp_err_t oratio_packet_encode(
    const oratio_packet_t *packet,
    uint8_t *buffer,
    size_t buffer_capacity,
    size_t *encoded_length)
{
    /* Clear the output count even if another argument is invalid. */
    if (encoded_length != NULL)
    {
        *encoded_length = 0;
    }

    /* Validate first, then write bytes. */
    if (packet == NULL || buffer == NULL || encoded_length == NULL)
    {
        ESP_LOGE(TAG, "Null pointer argument(s) to oratio_packet_encode");
        return ESP_ERR_INVALID_ARG;
    }

    if (packet->type != ORATIO_PACKET_AUDIO &&
        packet->type != ORATIO_PACKET_TELEMETRY &&
        packet->type != ORATIO_PACKET_HEARTBEAT)
    {
        ESP_LOGE(TAG, "Invalid packet type: %u", (unsigned int)packet->type);
        return ESP_ERR_INVALID_ARG;
    }
    else if (packet->length > ORATIO_MAX_PAYLOAD_SIZE)
    {
        ESP_LOGE(TAG, "Packet length exceeds maximum: %u", (unsigned int)packet->length);
        return ESP_ERR_INVALID_ARG;
    }
    else if (packet->type == ORATIO_PACKET_HEARTBEAT && packet->length != 0)
    {
        ESP_LOGE(TAG, "Heartbeat packet must have zero payload length");
        return ESP_ERR_INVALID_ARG;
    }
    else if (buffer_capacity < ORATIO_HEADER_SIZE + packet->length)
    {
        ESP_LOGE(TAG, "Buffer capacity insufficient for packet encoding");
        return ESP_ERR_INVALID_SIZE;
    }

    buffer[0] = packet->type;
    buffer[1] = (uint8_t)(packet->node_id >> 8);
    buffer[2] = (uint8_t)(packet->node_id & 0xFF);
    buffer[3] = (uint8_t)(packet->sequence >> 8);
    buffer[4] = (uint8_t)(packet->sequence & 0xFF);
    buffer[5] = (uint8_t)(packet->length >> 8);
    buffer[6] = (uint8_t)(packet->length & 0xFF);

    memcpy(buffer + ORATIO_HEADER_SIZE, packet->payload, packet->length);
    *encoded_length = ORATIO_HEADER_SIZE + packet->length;
    return ESP_OK;
}

esp_err_t oratio_packet_decode(
    const uint8_t *buffer,
    size_t received_length,
    oratio_packet_t *packet)
{
    /* Check the input storage before reading any header bytes. */
    if (buffer == NULL || packet == NULL)
    {
        ESP_LOGE(TAG, "Null pointer argument(s) to oratio_packet_decode");
        return ESP_ERR_INVALID_ARG;
    }

    if (received_length < ORATIO_HEADER_SIZE)
    {
        ESP_LOGE(TAG, "Received length is insufficient for packet header");
        return ESP_ERR_INVALID_SIZE;
    }

    packet->type = buffer[0];
    packet->node_id = ((uint16_t)buffer[1] << 8) | (uint16_t)buffer[2];
    packet->sequence = ((uint16_t)buffer[3] << 8) | (uint16_t)buffer[4];
    packet->length = ((uint16_t)buffer[5] << 8) | (uint16_t)buffer[6];

    /* Only validate header fields after decoding their received values. */
    if (packet->type != ORATIO_PACKET_AUDIO &&
        packet->type != ORATIO_PACKET_TELEMETRY &&
        packet->type != ORATIO_PACKET_HEARTBEAT)
    {
        ESP_LOGE(TAG, "Invalid packet type: %u", (unsigned int)packet->type);
        return ESP_ERR_INVALID_ARG;
    }

    if (packet->length > ORATIO_MAX_PAYLOAD_SIZE)
    {
        ESP_LOGE(TAG, "Decoded payload exceeds maximum: %u", (unsigned int)packet->length);
        return ESP_ERR_INVALID_SIZE;
    }

    if (packet->type == ORATIO_PACKET_HEARTBEAT && packet->length != 0)
    {
        ESP_LOGE(TAG, "Heartbeat packet must have zero payload length");
        return ESP_ERR_INVALID_ARG;
    }

    if (received_length != ORATIO_HEADER_SIZE + packet->length)
    {
        ESP_LOGE(TAG, "Received length does not match declared payload length");
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(packet->payload, buffer + ORATIO_HEADER_SIZE, packet->length);
    return ESP_OK;
}

/* ========================================================================== */
/* 4. UDP socket setup and incoming packets                                    */
/* ========================================================================== */

/* Create, bind, and configure a UDP socket. The caller decides when to retry.
 * Return the descriptor on success, or -1 after cleaning up a setup failure.
 */
static int network_socket_create(void)
{
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (socket_fd < 0)
    {
        ESP_LOGE(TAG, "Failed to create UDP socket: %d", errno);
        return -1;
    }

    struct sockaddr_in local_address =
        {
            .sin_family = AF_INET,
            .sin_port = htons(ORATIO_UDP_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY)};

    int bind_result = bind(
        socket_fd, (const struct sockaddr *)&local_address, sizeof(local_address));

    if (bind_result < 0)
    {
        ESP_LOGE(TAG, "Failed to bind UDP socket: %d", errno);
        close(socket_fd);
        return -1;
    }

    struct timeval receive_timeout =
        {
            .tv_sec = 0,
            .tv_usec = 10000};

    int setsockopt_result = setsockopt(
        socket_fd,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &receive_timeout,
        sizeof(receive_timeout));

    if (setsockopt_result < 0)
    {
        ESP_LOGE(TAG, "Failed to set receive timeout: %d", errno);
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

/* Receive at most one datagram per loop so sending also gets processor time.
 * Return false only for a socket error that requires cleanup and recreation.
 * Valid data packets are copied to the optional playback queue.
 */
static bool network_receive_packet(int socket_fd,
                                   const struct sockaddr_in *server_address)
{
    /* Reusable storage owned by the single network task. */
    static uint8_t receive_buffer[ORATIO_HEADER_SIZE + ORATIO_MAX_PAYLOAD_SIZE + 1];
    struct sockaddr_in sender_address = {0};
    socklen_t sender_length = sizeof(sender_address);
    ssize_t bytes_received = recvfrom(
        socket_fd, receive_buffer, sizeof(receive_buffer), 0,
        (struct sockaddr *)&sender_address, &sender_length);

    if (bytes_received < 0)
    {
        const int receive_error = errno;
        /* A quiet socket is expected with a short receive timeout. */
        if (receive_error == EAGAIN || receive_error == EWOULDBLOCK ||
            receive_error == EINTR)
        {
            return true;
        }

        ESP_LOGE(TAG, "Failed to receive UDP packet: %d", receive_error);
        return false;
    }

    /* Forwarded packets still arrive from the gateway's socket. The original
     * sender is identified by the Node ID inside the decoded header.
     */
    if (sender_address.sin_addr.s_addr != server_address->sin_addr.s_addr ||
        sender_address.sin_port != server_address->sin_port)
    {
        ESP_LOGW(TAG, "Ignoring packet from unexpected gateway endpoint");
        return true;
    }

    static oratio_packet_t received_packet;
    esp_err_t result = oratio_packet_decode(
        receive_buffer, (size_t)bytes_received, &received_packet);
    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "Ignoring invalid UDP packet: %s", esp_err_to_name(result));
        return true;
    }

    if (received_packet.type == ORATIO_PACKET_HEARTBEAT)
    {
        ESP_LOGI(TAG, "Heartbeat received: node=%04x sequence=%u",
                 (unsigned int)received_packet.node_id,
                 (unsigned int)received_packet.sequence);
    }
    else
    {
        ESP_LOGI(TAG, "Packet received: type=%c node=%04x sequence=%u length=%u",
                 received_packet.type, (unsigned int)received_packet.node_id,
                 (unsigned int)received_packet.sequence,
                 (unsigned int)received_packet.length);

        if (s_playback_queue != NULL)
        {
            BaseType_t queue_result = xQueueSend(
                s_playback_queue,
                &received_packet,
                0);

            if (queue_result != pdTRUE)
            {
                ESP_LOGW(TAG, "Playback queue full; dropping received packet");
            }
        }
    }

    return true;
}

/* ========================================================================== */
/* 5. Network task loop                                                       */
/* ========================================================================== */

static void network_task(void *arg)
{
    (void)arg;
    int socket_fd = -1;
    const struct sockaddr_in local_server = {
        .sin_family = AF_INET,
        .sin_port = htons(ORATIO_UDP_PORT)};
    struct sockaddr_in server_address = local_server;

    if (inet_pton(AF_INET, ORATIO_SERVER_IP, &server_address.sin_addr) != 1)
    {
        ESP_LOGE(TAG, "Could not configure server IP address");
        s_network_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint8_t station_mac[6] = {0};
    esp_err_t mac_result = esp_wifi_get_mac(WIFI_IF_STA, station_mac);
    if (mac_result != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read station MAC: %s", esp_err_to_name(mac_result));
        s_network_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint8_t heartbeat_packet[ORATIO_HEADER_SIZE] = {0};
    heartbeat_packet[0] = ORATIO_PACKET_HEARTBEAT;
    heartbeat_packet[1] = station_mac[4];
    heartbeat_packet[2] = station_mac[5];
    uint16_t sequence_number = 0;
    const TickType_t heartbeat_interval = pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS);
    TickType_t last_heartbeat_time = xTaskGetTickCount();

    static oratio_tx_item_t outgoing_data;
    static oratio_packet_t outgoing_packet;

    static uint8_t send_buffer[ORATIO_HEADER_SIZE + ORATIO_MAX_PAYLOAD_SIZE];

    while (true)
    {
        /* A. Create/recover the socket. Delay failed setup attempts. */
        if (socket_fd < 0)
        {
            socket_fd = network_socket_create();
            if (socket_fd < 0)
            {
                ESP_LOGW(TAG, "Socket setup failed; retrying in one second");
                vTaskDelay(pdMS_TO_TICKS(SOCKET_RETRY_MS));
                continue;
            }

            /* Send immediately after setup, without catching up missed periods. */
            last_heartbeat_time = xTaskGetTickCount() - heartbeat_interval;
        }

        /* B. Connect the capture queue to outgoing packets */
        if (s_capture_queue != NULL &&
            xQueueReceive(s_capture_queue, &outgoing_data, 0) == pdTRUE)
        {
            /* Validate the item, build its header, encode, and send. */
            if (outgoing_data.length > ORATIO_MAX_PAYLOAD_SIZE)
            {
                ESP_LOGW(TAG, "Outgoing item exceeds maximum payload size; dropping");
            }
            else if (outgoing_data.type != ORATIO_PACKET_AUDIO &&
                     outgoing_data.type != ORATIO_PACKET_TELEMETRY)
            {
                ESP_LOGW(TAG, "Outgoing item has invalid type; dropping");
            }
            else
            {
                outgoing_packet.type = outgoing_data.type;
                outgoing_packet.node_id = ((uint16_t)station_mac[4] << 8) | (uint16_t)station_mac[5];
                outgoing_packet.sequence = sequence_number;
                outgoing_packet.length = outgoing_data.length;
                memcpy(outgoing_packet.payload, outgoing_data.payload, outgoing_data.length);

                size_t encoded_length = 0;
                esp_err_t encode_result = oratio_packet_encode(
                    &outgoing_packet, send_buffer, sizeof(send_buffer), &encoded_length);

                if (encode_result != ESP_OK)
                {
                    ESP_LOGW(TAG, "Failed to encode outgoing packet: %s", esp_err_to_name(encode_result));
                }
                else
                {
                    ssize_t bytes_sent = sendto(
                        socket_fd, send_buffer, encoded_length, 0,
                        (const struct sockaddr *)&server_address, sizeof(server_address));

                    if (bytes_sent != (ssize_t)encoded_length)
                    {
                        if (bytes_sent < 0)
                        {
                            ESP_LOGE(TAG, "Data send failed: %d", errno);
                        }
                        else
                        {
                            ESP_LOGW(TAG, "Sent %zd bytes, expected %zu", bytes_sent, encoded_length);
                        }
                        /* Drop this dequeued item and recover before sending again. */
                        close(socket_fd);
                        socket_fd = -1;
                        vTaskDelay(pdMS_TO_TICKS(SOCKET_RETRY_MS));
                        continue;
                    }
                    else
                    {
                        ESP_LOGI(TAG, "Packet sent: type=%c node=%04x sequence=%u length=%u",
                                 outgoing_packet.type,
                                 (unsigned int)outgoing_packet.node_id,
                                 (unsigned int)outgoing_packet.sequence,
                                 (unsigned int)outgoing_packet.length);
                        sequence_number++;
                        last_heartbeat_time = xTaskGetTickCount(); /* Reset heartbeat timer after sending data. */
                    }
                }
            }
        }

        /* C. Send a heartbeat only after five seconds without outgoing data. */
        TickType_t now = xTaskGetTickCount();
        if ((TickType_t)(now - last_heartbeat_time) >= heartbeat_interval)
        {
            heartbeat_packet[3] = (uint8_t)(sequence_number >> 8);
            heartbeat_packet[4] = (uint8_t)(sequence_number & 0xFF);
            ssize_t bytes_sent = sendto(
                socket_fd, heartbeat_packet, sizeof(heartbeat_packet), 0,
                (const struct sockaddr *)&server_address, sizeof(server_address));

            if (bytes_sent != (ssize_t)sizeof(heartbeat_packet))
            {
                if (bytes_sent < 0)
                {
                    ESP_LOGE(TAG, "Failed to send heartbeat packet: %d", errno);
                }
                else
                {
                    ESP_LOGW(TAG, "Sent %zd bytes, expected %zu",
                             bytes_sent, sizeof(heartbeat_packet));
                }
                close(socket_fd);
                socket_fd = -1;
                vTaskDelay(pdMS_TO_TICKS(SOCKET_RETRY_MS));
                continue;
            }

            last_heartbeat_time = xTaskGetTickCount();
            ESP_LOGI(TAG, "Heartbeat sent, sequence: %u", (unsigned int)sequence_number);
            sequence_number++;
            mesh_network_log_status();
        }

        /* D. Receive independently of heartbeat transmission. */
        if (!network_receive_packet(socket_fd, &server_address))
        {
            close(socket_fd);
            socket_fd = -1;
            vTaskDelay(pdMS_TO_TICKS(SOCKET_RETRY_MS));
            continue;
        }

        /* Yield even when packets arrive continuously. This is one RTOS tick,
         * not a five-second sleep that would delay forwarding reception.
         */
        vTaskDelay(1);
    }
}

/* ========================================================================== */
/* 6. Public task startup                                                     */
/* ========================================================================== */

esp_err_t network_task_start(QueueHandle_t capture_queue,
                             QueueHandle_t playback_queue)
{
    /* Call from startup once; task-owned buffers and queue handles are shared. */
    if (s_network_task_handle != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_capture_queue = capture_queue;
    s_playback_queue = playback_queue;

    BaseType_t result = xTaskCreate(
        network_task,
        "network_task",
        4096,
        NULL,
        tskIDLE_PRIORITY + 1,
        &s_network_task_handle);

    return (result == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}