/* Optional network diagnostics are excluded from normal/benchmark builds.
 * This must precede every ESP-IDF header that could include logging macros.
 */
#ifndef ORATIO_NETWORK_DEBUG
#define ORATIO_NETWORK_DEBUG 0
#endif

#if (ORATIO_NETWORK_DEBUG != 0) && (ORATIO_NETWORK_DEBUG != 1)
#error "ORATIO_NETWORK_DEBUG must be 0 or 1"
#endif

#if ORATIO_NETWORK_DEBUG
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#else
#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#endif

#include "network.h"
#include "storage.h"
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <inttypes.h>
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
static const uint32_t HEARTBEAT_INTERVAL_MS = 1000;
static const uint32_t SOCKET_RETRY_MS = 1000;
static const uint32_t METRICS_INTERVAL_MS = 2000;
static const uint16_t ORATIO_UDP_PORT = 8989;
static const BaseType_t NETWORK_TASK_CORE = 1;
static const char ORATIO_SERVER_IP[] = "192.168.137.1";

static const char *TAG = "MESH_NETWORK";
static const bool IS_ROOT = (APP_IS_ROOT == 1);
static const uint8_t ROOT_LEVEL = 1;

/* Initial SoftAP channel; the upstream connection may determine the operating channel later. */
static const uint8_t MESH_CHANNEL = 6;

/* Keep the generated network name available for configuration and later status logs. */
static char s_softap_ssid[SOFTAP_SSID_BUFFER_SIZE];

static MessageBufferHandle_t s_capture_buffer = NULL;
static MessageBufferHandle_t s_playback_buffer = NULL;
static TaskHandle_t s_network_task_handle = NULL;

/* Future LED state. Atomic access keeps event-task writes and cross-core reads safe.
 * This tracks the local upstream connection, not gateway reachability or delivery.
 */
static atomic_bool s_network_connected = ATOMIC_VAR_INIT(false);

/* Single writer: the network task. Reset after each rolling summary. */
typedef struct
{
    uint32_t sent_audio;
    uint32_t received_audio;
    uint32_t invalid_capture;
    uint32_t rejected_packets;
    uint32_t playback_drops;
    uint32_t socket_errors;
} network_metrics_t;

static network_metrics_t s_metrics;
static TickType_t s_last_metrics;

static void network_report_metrics(void)
{
    TickType_t now = xTaskGetTickCount();
    if ((TickType_t)(now - s_last_metrics) < pdMS_TO_TICKS(METRICS_INTERVAL_MS))
    {
        return;
    }
    ESP_LOGI(TAG,
             "Window: tx=%lu rx=%lu invalid-capture=%lu rejected=%lu playback-drops=%lu "
             "socket-errors=%lu stack-free=%lu B",
             (unsigned long)s_metrics.sent_audio, (unsigned long)s_metrics.received_audio,
             (unsigned long)s_metrics.invalid_capture, (unsigned long)s_metrics.rejected_packets,
             (unsigned long)s_metrics.playback_drops, (unsigned long)s_metrics.socket_errors,
             (unsigned long)uxTaskGetStackHighWaterMark(NULL));
    memset(&s_metrics, 0, sizeof(s_metrics));
    s_last_metrics = now;
}

/* ========================================================================== */
/* 2. Mesh setup and diagnostics                                               */
/* ========================================================================== */

/* Query the state without exposing writable module storage. A root's upstream is
 * the router/hotspot; a non-root node's upstream is its mesh parent.
 */
bool network_is_connected(void)
{
    return atomic_load_explicit(&s_network_connected, memory_order_relaxed);
}

/* Called by the default ESP-IDF event task; no polling task or GPIO is needed.
 * Association alone is insufficient: wait until the station has an IPv4 address.
 * A child joining/leaving our SoftAP must not change this node's upstream state.
 */
static void network_connection_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        atomic_store_explicit(&s_network_connected, true, memory_order_relaxed);
        return;
    }
    if ((event_base == WIFI_EVENT &&
         (event_id == WIFI_EVENT_STA_DISCONNECTED || event_id == WIFI_EVENT_STA_STOP)) ||
        (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP))
    {
        atomic_store_explicit(&s_network_connected, false, memory_order_relaxed);
    }
}

/* Install before Wi-Fi starts so startup/reconnection events cannot be missed. */
static esp_err_t network_connection_events_init(void)
{
    esp_event_handler_instance_t wifi_handler = NULL;
    esp_event_handler_instance_t ip_handler = NULL;
    atomic_store_explicit(&s_network_connected, false, memory_order_relaxed);
    esp_err_t result = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, network_connection_event, NULL, &wifi_handler);
    if (result != ESP_OK)
    {
        return result;
    }
    result = esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, network_connection_event, NULL, &ip_handler);
    if (result != ESP_OK)
    {
        /* Leave no partially installed subscription if the second registration fails. */
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler);
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
        if (ssid_length == 0 || ssid_length > sizeof(station_config.sta.ssid) ||
            password_length >= sizeof(station_config.sta.password))
        {
            ESP_LOGE(TAG, "Hotspot credentials are empty or exceed supported lengths");
            return ESP_ERR_INVALID_ARG;
        }

        memcpy(station_config.sta.ssid, CONFIG_ORATIO_ROUTER_SSID, ssid_length);

        memcpy(station_config.sta.password, CONFIG_ORATIO_ROUTER_PASSWORD, password_length);
    }

    ESP_RETURN_ON_ERROR(esp_bridge_wifi_set_config(WIFI_IF_STA, &station_config), TAG, "Failed to configure station");

    /* Provide the initial Wi-Fi settings for the access point hosted by this board. */
    wifi_config_t softap_config = {
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

    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_AP, softap_mac), TAG, "Failed to read SoftAP MAC");

    /*
     * Limit the base name to 25 characters so the underscore and six hexadecimal
     * characters fit within the 32-byte SSID limit. snprintf writes into the buffer.
     */
    snprintf(s_softap_ssid, sizeof(s_softap_ssid), "%.25s_%02x%02x%02x", CONFIG_BRIDGE_SOFTAP_SSID, softap_mac[3],
             softap_mac[4], softap_mac[5]);

    return esp_mesh_lite_set_softap_info(s_softap_ssid, CONFIG_BRIDGE_SOFTAP_PASSWORD);
}

/*
 * Initialize the shared networking services, configure the selected role, and start Mesh-Lite.
 * ESP_RETURN_ON_ERROR logs and returns an error if a checked initialization step fails.
 */
esp_err_t mesh_network_init(void)
{
#if ORATIO_NETWORK_DEBUG
    /* Enable the already compiled-in DEBUG calls for this tag only. */
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif
    ESP_RETURN_ON_ERROR(storage_init(), TAG, "Storage initialization failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Network interface initialization failed");

    /* The default event loop delivers Wi-Fi and IP events to registered handlers. */
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Event loop creation failed");

    ESP_RETURN_ON_ERROR(network_connection_events_init(), TAG, "Connection status event registration failed");

    /* Create the network interfaces enabled in the IoT-Bridge configuration. */
    esp_bridge_create_all_netif();

    ESP_RETURN_ON_ERROR(mesh_wifi_configure(), TAG, "Wi-Fi configuration failed");

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

        memcpy(router_config.ssid, CONFIG_ORATIO_ROUTER_SSID, strlen(CONFIG_ORATIO_ROUTER_SSID));

        memcpy(router_config.password, CONFIG_ORATIO_ROUTER_PASSWORD, strlen(CONFIG_ORATIO_ROUTER_PASSWORD));

        ESP_RETURN_ON_ERROR(esp_mesh_lite_set_router_config(&router_config), TAG,
                            "Failed to configure Mesh-Lite router");
    }

    ESP_RETURN_ON_ERROR(mesh_softap_configure(), TAG, "Mesh SoftAP configuration failed");

    /* Enforce a fixed root rather than allowing non-root devices to become root. */
    if (IS_ROOT)
    {
        ESP_RETURN_ON_ERROR(esp_mesh_lite_set_allowed_level(ROOT_LEVEL), TAG, "Failed to restrict root to level 1");
    }
    else
    {
#if defined(APP_FORCE_LEVEL3) && (APP_FORCE_LEVEL3 == 1)
        /* Temporary test restriction: this board must join through a level-2 parent. */
        ESP_RETURN_ON_ERROR(esp_mesh_lite_set_allowed_level(3), TAG, "Failed to restrict test node to level 3");
#else
        /* Ordinary nodes may choose a suitable mesh level, but cannot become level 1. */
        ESP_RETURN_ON_ERROR(esp_mesh_lite_set_disallowed_level(ROOT_LEVEL), TAG,
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

    ESP_LOGI(TAG, "Firmware role: %s | mesh level: %u | SoftAP: %s", IS_ROOT ? "root" : "node", (unsigned int)level,
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
    ESP_LOGI(TAG, "Parent BSSID: " MACSTR " | RSSI: %d dBm", MAC2STR(parent_info.bssid), parent_info.rssi);
}

/* ========================================================================== */
/* 3. Packet encoding and decoding                                             */
/* ========================================================================== */

esp_err_t oratio_packet_encode(const oratio_packet_t *packet, uint8_t *buffer, size_t buffer_capacity,
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
        ESP_LOGD(TAG, "Null pointer argument(s) to oratio_packet_encode");
        return ESP_ERR_INVALID_ARG;
    }

    if (packet->type != ORATIO_PACKET_AUDIO && packet->type != ORATIO_PACKET_TELEMETRY &&
        packet->type != ORATIO_PACKET_HEARTBEAT)
    {
        ESP_LOGD(TAG, "Invalid packet type: %u", (unsigned int)packet->type);
        return ESP_ERR_INVALID_ARG;
    }
    else if (packet->length > ORATIO_MAX_PAYLOAD_SIZE)
    {
        ESP_LOGD(TAG, "Packet length exceeds maximum: %u", (unsigned int)packet->length);
        return ESP_ERR_INVALID_ARG;
    }
    else if (packet->type == ORATIO_PACKET_HEARTBEAT && packet->length != 0)
    {
        ESP_LOGD(TAG, "Heartbeat packet must have zero payload length");
        return ESP_ERR_INVALID_ARG;
    }
    else if (buffer_capacity < ORATIO_HEADER_SIZE + packet->length)
    {
        ESP_LOGD(TAG, "Buffer capacity insufficient for packet encoding");
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

esp_err_t oratio_packet_decode(const uint8_t *buffer, size_t received_length, oratio_packet_t *packet)
{
    /* Check the input storage before reading any header bytes. */
    if (buffer == NULL || packet == NULL)
    {
        ESP_LOGD(TAG, "Null pointer argument(s) to oratio_packet_decode");
        return ESP_ERR_INVALID_ARG;
    }

    if (received_length < ORATIO_HEADER_SIZE)
    {
        ESP_LOGD(TAG, "Received length is insufficient for packet header");
        return ESP_ERR_INVALID_SIZE;
    }

    packet->type = buffer[0];
    packet->node_id = ((uint16_t)buffer[1] << 8) | (uint16_t)buffer[2];
    packet->sequence = ((uint16_t)buffer[3] << 8) | (uint16_t)buffer[4];
    packet->length = ((uint16_t)buffer[5] << 8) | (uint16_t)buffer[6];

    /* Only validate header fields after decoding their received values. */
    if (packet->type != ORATIO_PACKET_AUDIO && packet->type != ORATIO_PACKET_TELEMETRY &&
        packet->type != ORATIO_PACKET_HEARTBEAT)
    {
        ESP_LOGD(TAG, "Invalid packet type: %u", (unsigned int)packet->type);
        return ESP_ERR_INVALID_ARG;
    }

    if (packet->length > ORATIO_MAX_PAYLOAD_SIZE)
    {
        ESP_LOGD(TAG, "Decoded payload exceeds maximum: %u", (unsigned int)packet->length);
        return ESP_ERR_INVALID_SIZE;
    }

    if (packet->type == ORATIO_PACKET_HEARTBEAT && packet->length != 0)
    {
        ESP_LOGD(TAG, "Heartbeat packet must have zero payload length");
        return ESP_ERR_INVALID_ARG;
    }

    if (received_length != ORATIO_HEADER_SIZE + packet->length)
    {
        ESP_LOGD(TAG, "Received length does not match declared payload length");
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(packet->payload, buffer + ORATIO_HEADER_SIZE, packet->length);
    return ESP_OK;
}

/* Parsed header only: no intermediate payload storage. */
typedef struct
{
    uint8_t type;
    uint16_t node_id;
    uint16_t sequence;
    uint16_t length;
} network_packet_header_t;

static esp_err_t network_read_packet_header(const uint8_t *buffer, size_t received_length,
                                            network_packet_header_t *header)
{
    if (buffer == NULL || header == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (received_length < ORATIO_HEADER_SIZE)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    header->type = buffer[0];
    header->node_id = ((uint16_t)buffer[1] << 8) | buffer[2];
    header->sequence = ((uint16_t)buffer[3] << 8) | buffer[4];
    header->length = ((uint16_t)buffer[5] << 8) | buffer[6];
    if (header->type != ORATIO_PACKET_AUDIO && header->type != ORATIO_PACKET_TELEMETRY &&
        header->type != ORATIO_PACKET_HEARTBEAT)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (header->length > ORATIO_MAX_PAYLOAD_SIZE)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    if (header->type == ORATIO_PACKET_HEARTBEAT && header->length != 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (received_length != ORATIO_HEADER_SIZE + header->length)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t network_audio_frame_encode(const audio_frame_t *frame, uint16_t node_id, uint8_t *buffer, size_t capacity,
                                     size_t *encoded_length)
{
    if (encoded_length != NULL)
    {
        *encoded_length = 0;
    }
    if (frame == NULL || buffer == NULL || encoded_length == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (frame->length > AUDIO_FRAME_BYTES || frame->length % sizeof(int16_t) != 0)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    if (capacity < ORATIO_HEADER_SIZE + frame->length)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    buffer[0] = ORATIO_PACKET_AUDIO;
    buffer[1] = (uint8_t)(node_id >> 8);
    buffer[2] = (uint8_t)node_id;
    buffer[3] = (uint8_t)(frame->sequence >> 8);
    buffer[4] = (uint8_t)frame->sequence;
    buffer[5] = (uint8_t)(frame->length >> 8);
    buffer[6] = (uint8_t)frame->length;
    memcpy(buffer + ORATIO_HEADER_SIZE, frame->samples, frame->length);
    *encoded_length = ORATIO_HEADER_SIZE + frame->length;
    return ESP_OK;
}

esp_err_t network_audio_frame_decode(const uint8_t *buffer, size_t received_length, audio_frame_t *frame)
{
    if (frame == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    network_packet_header_t header;
    esp_err_t result = network_read_packet_header(buffer, received_length, &header);
    if (result != ESP_OK)
    {
        return result;
    }
    if (header.type != ORATIO_PACKET_AUDIO)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (header.length % sizeof(int16_t) != 0)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    frame->sequence = header.sequence;
    frame->length = header.length;
    memcpy(frame->samples, buffer + ORATIO_HEADER_SIZE, header.length);
    return ESP_OK;
}

/* ========================================================================== */
/* 4. Task context and socket lifecycle                                       */
/* ========================================================================== */

/* Only the network task owns this state. Message buffers carry the current complete record types. */
typedef struct
{
    int32_t socket_fd;
    struct sockaddr_in server_address;
    uint16_t node_id;
    uint16_t heartbeat_sequence;
    TickType_t last_activity;
} network_context_t;

static esp_err_t network_context_init(network_context_t *context)
{
    memset(context, 0, sizeof(*context));
    context->socket_fd = -1;
    context->server_address.sin_family = AF_INET;
    context->server_address.sin_port = htons(ORATIO_UDP_PORT);

    if (inet_pton(AF_INET, ORATIO_SERVER_IP, &context->server_address.sin_addr) != 1)
    {
        ESP_LOGE(TAG, "Could not configure server IP address");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t station_mac[6] = {0};
    esp_err_t result = esp_wifi_get_mac(WIFI_IF_STA, station_mac);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read station MAC: %s", esp_err_to_name(result));
        return result;
    }

    context->node_id = ((uint16_t)station_mac[4] << 8) | (uint16_t)station_mac[5];
    return ESP_OK;
}

/* Output remains -1 on failure. Clean up partially configured sockets here. */
static esp_err_t network_socket_create(int32_t *socket_fd)
{
    *socket_fd = -1;
    int32_t descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (descriptor < 0)
    {
        ESP_LOGD(TAG, "Failed to create UDP socket: %d", errno);
        return ESP_FAIL;
    }

    struct sockaddr_in local_address = {
        .sin_family = AF_INET, .sin_port = htons(ORATIO_UDP_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};

    if (bind(descriptor, (const struct sockaddr *)&local_address, sizeof(local_address)) < 0)
    {
        ESP_LOGD(TAG, "Failed to bind UDP socket: %d", errno);
        close(descriptor);
        return ESP_FAIL;
    }

    const struct timeval receive_timeout = {.tv_sec = 0, .tv_usec = 10000};

    if (setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout)) < 0)
    {
        ESP_LOGD(TAG, "Failed to set receive timeout: %d", errno);
        close(descriptor);
        return ESP_FAIL;
    }

    *socket_fd = descriptor;
    return ESP_OK;
}

static esp_err_t network_socket_ensure_ready(network_context_t *context)
{
    if (context->socket_fd >= 0)
    {
        return ESP_OK;
    }

    esp_err_t result = network_socket_create(&context->socket_fd);
    if (result != ESP_OK)
    {
        return result;
    }

    /* An immediate heartbeat is due after setup unless buffered data is sent first. */
    context->last_activity = xTaskGetTickCount() - pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS);
    return ESP_OK;
}

/* Every socket failure follows the same close/reset/backoff path. */
static void network_socket_recover(network_context_t *context)
{
    if (context->socket_fd >= 0)
    {
        close(context->socket_fd);
    }

    context->socket_fd = -1;
    vTaskDelay(pdMS_TO_TICKS(SOCKET_RETRY_MS));
}

/* Shared by buffered data and heartbeats. Local send success is not an ACK. */
static esp_err_t network_socket_send(network_context_t *context, const uint8_t *buffer, size_t length)
{
    ssize_t bytes_sent = sendto(context->socket_fd, buffer, length, 0,
                                (const struct sockaddr *)&context->server_address, sizeof(context->server_address));

    if (bytes_sent < 0)
    {
        ESP_LOGD(TAG, "UDP send failed: %d", errno);
        return ESP_FAIL;
    }

    if (bytes_sent != (ssize_t)length)
    {
        ESP_LOGD(TAG, "Sent %zd bytes, expected %zu", bytes_sent, length);
        return ESP_FAIL;
    }

    context->last_activity = xTaskGetTickCount();
    return ESP_OK;
}

/* ========================================================================== */
/* 5. Outgoing message buffer and idle heartbeat processing                            */
/* ========================================================================== */

static esp_err_t network_process_capture(network_context_t *context)
{
    static audio_frame_t frame;
    static uint8_t send_buffer[ORATIO_HEADER_SIZE + ORATIO_MAX_PAYLOAD_SIZE];
    if (s_capture_buffer == NULL)
    {
        return ESP_OK;
    }
    size_t received_length = xMessageBufferReceive(s_capture_buffer, &frame, sizeof(frame), 0);
    if (received_length == 0)
    {
        return ESP_OK;
    }
    if (received_length != sizeof(frame))
    {
        s_metrics.invalid_capture++;
        return ESP_OK;
    }
    size_t encoded_length = 0;
    esp_err_t result =
        network_audio_frame_encode(&frame, context->node_id, send_buffer, sizeof(send_buffer), &encoded_length);
    if (result != ESP_OK)
    {
        s_metrics.invalid_capture++;
        return ESP_OK;
    }
    /* Capture assigns sequence. Network only adds its node ID and wire header. */
    result = network_socket_send(context, send_buffer, encoded_length);
    if (result == ESP_OK)
    {
        s_metrics.sent_audio++;
    }
    return result;
}

static bool network_is_heartbeat_due(const network_context_t *context)
{
    TickType_t elapsed = xTaskGetTickCount() - context->last_activity;
    return elapsed >= pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS);
}

static esp_err_t network_process_heartbeat(network_context_t *context)
{
    if (!network_is_heartbeat_due(context))
    {
        return ESP_OK;
    }

    uint8_t heartbeat[ORATIO_HEADER_SIZE] = {0};
    heartbeat[0] = ORATIO_PACKET_HEARTBEAT;
    heartbeat[1] = (uint8_t)(context->node_id >> 8);
    heartbeat[2] = (uint8_t)context->node_id;
    heartbeat[3] = (uint8_t)(context->heartbeat_sequence >> 8);
    heartbeat[4] = (uint8_t)context->heartbeat_sequence;

    uint16_t sent_sequence = context->heartbeat_sequence;
    esp_err_t result = network_socket_send(context, heartbeat, sizeof(heartbeat));
    if (result != ESP_OK)
    {
        return result;
    }

    context->heartbeat_sequence++;
    ESP_LOGD(TAG, "Heartbeat sent, sequence: %u", (unsigned int)sent_sequence);
#if ORATIO_NETWORK_DEBUG
    mesh_network_log_status();
#endif
    return ESP_OK;
}

/* ========================================================================== */
/* 6. Incoming packet validation and playback delivery                        */
/* ========================================================================== */

static bool network_is_gateway_sender(const network_context_t *context, const struct sockaddr_in *sender)
{
    return sender->sin_addr.s_addr == context->server_address.sin_addr.s_addr &&
           sender->sin_port == context->server_address.sin_port;
}

static void network_deliver_audio_frame(const audio_frame_t *frame)
{
    if (s_playback_buffer == NULL)
    {
        return;
    }
    if (xMessageBufferSend(s_playback_buffer, frame, sizeof(*frame), 0) != sizeof(*frame))
    {
        s_metrics.playback_drops++;
    }
}

/* Empty/interrupted receives and rejected datagrams are normal outcomes.
 * Only an actual socket error asks the main loop to recover the socket.
 */
static esp_err_t network_process_receive(network_context_t *context)
{
    static uint8_t receive_buffer[ORATIO_HEADER_SIZE + ORATIO_MAX_PAYLOAD_SIZE + 1];
    static audio_frame_t frame;
    struct sockaddr_in sender = {0};
    socklen_t sender_length = sizeof(sender);
    ssize_t bytes_received = recvfrom(context->socket_fd, receive_buffer, sizeof(receive_buffer), 0,
                                      (struct sockaddr *)&sender, &sender_length);

    if (bytes_received < 0)
    {
        int32_t receive_error = errno;
        if (receive_error == EAGAIN || receive_error == EWOULDBLOCK || receive_error == EINTR)
        {
            return ESP_OK;
        }

        ESP_LOGD(TAG, "Failed to receive UDP packet: %" PRId32, receive_error);
        return ESP_FAIL;
    }

    if (!network_is_gateway_sender(context, &sender))
    {
        s_metrics.rejected_packets++;
        return ESP_OK;
    }

    network_packet_header_t header;
    esp_err_t result = network_read_packet_header(receive_buffer, (size_t)bytes_received, &header);
    if (result != ESP_OK)
    {
        s_metrics.rejected_packets++;
        return ESP_OK;
    }
    if (header.type == ORATIO_PACKET_HEARTBEAT)
    {
        ESP_LOGD(TAG, "Heartbeat received: node=%04x sequence=%u", (unsigned int)header.node_id,
                 (unsigned int)header.sequence);
        return ESP_OK;
    }
    if (header.type != ORATIO_PACKET_AUDIO)
    {
        /* Telemetry must have its own consumer, not the PCM playback buffer. */
        ESP_LOGD(TAG, "Telemetry received from node=%04x", (unsigned int)header.node_id);
        return ESP_OK;
    }
    result = network_audio_frame_decode(receive_buffer, (size_t)bytes_received, &frame);
    if (result != ESP_OK)
    {
        s_metrics.rejected_packets++;
        return ESP_OK;
    }
    s_metrics.received_audio++;
    network_deliver_audio_frame(&frame);
    return ESP_OK;
}

/* ========================================================================== */
/* 7. High-level network loop                                                 */
/* ========================================================================== */

static esp_err_t network_process_iteration(network_context_t *context)
{
    esp_err_t result = network_socket_ensure_ready(context);
    if (result != ESP_OK)
    {
        return result;
    }

    result = network_process_capture(context);
    if (result != ESP_OK)
    {
        return result;
    }

    result = network_process_receive(context);
    if (result != ESP_OK)
    {
        return result;
    }

    /* Heartbeat runs last, after outgoing data and incoming packet processing. */
    return network_process_heartbeat(context);
}

static void network_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Network task running on core %d", (int)xPortGetCoreID());
    network_context_t context;
    if (network_context_init(&context) != ESP_OK)
    {
        s_network_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_last_metrics = xTaskGetTickCount();
    while (true)
    {
        if (network_process_iteration(&context) != ESP_OK)
        {
            s_metrics.socket_errors++;
            network_socket_recover(&context);
        }
        network_report_metrics();
    }
}

/* ========================================================================== */
/* 8. Public task startup                                                     */
/* ========================================================================== */

esp_err_t network_task_start(MessageBufferHandle_t capture_buffer, MessageBufferHandle_t playback_buffer)
{
    /* Call from startup once; task-owned buffers and message buffer handles are shared. */
    if (s_network_task_handle != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_capture_buffer = capture_buffer;
    s_playback_buffer = playback_buffer;

    BaseType_t result = xTaskCreatePinnedToCore(network_task, "network_task", 4096, NULL, tskIDLE_PRIORITY + 1,
                                                &s_network_task_handle, NETWORK_TASK_CORE);

    return (result == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
