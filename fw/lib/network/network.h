#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "audio_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"

esp_err_t mesh_network_init(void);
void mesh_network_log_status(void);
/* Future mesh-status LED input: true after the station gets an IPv4 address;
 * false before connection, on disconnect, stop, or address loss. Root: hotspot;
 * other nodes: mesh parent. This does not prove gateway/end-to-end connectivity.
 * Safe to read from tasks on either core; networking owns updates.
 */
bool network_is_connected(void);
/* One writer and one reader per buffer, with handles kept alive for the task.
 * Capture: producer -> network, one complete audio_frame_t per message.
 * Playback: network -> consumer, one complete audio_frame_t per message (audio only).
 * These are in-memory records; network serialization remains explicit.
 * NULL handles select heartbeat-only operation. Call once from startup.
 */
esp_err_t network_task_start(MessageBufferHandle_t capture_buffer, MessageBufferHandle_t playback_buffer);

typedef enum
{
    ORATIO_PACKET_AUDIO = 'A',
    ORATIO_PACKET_TELEMETRY = 'T',
    ORATIO_PACKET_HEARTBEAT = 'H'
} oratio_packet_type_t;

#define ORATIO_HEADER_SIZE 7
#define ORATIO_MAX_PAYLOAD_SIZE 640

typedef struct
{
    uint8_t type;
    uint16_t node_id;
    uint16_t sequence;
    uint16_t length;
    uint8_t payload[ORATIO_MAX_PAYLOAD_SIZE];

} oratio_packet_t;

esp_err_t oratio_packet_encode(const oratio_packet_t *packet, uint8_t *buffer, size_t buffer_capacity,
                               size_t *encoded_length);

esp_err_t oratio_packet_decode(const uint8_t *buffer, size_t received_length, oratio_packet_t *packet);

/* Direct audio codecs: no intermediate oratio_packet_t payload copy.
 * The producer's sequence is preserved. Only length bytes of samples are used.
 * Sample bytes retain their ESP32 native little-endian representation.
 */
esp_err_t network_audio_frame_encode(const audio_frame_t *frame, uint16_t node_id, uint8_t *buffer, size_t capacity,
                                     size_t *encoded_length);

esp_err_t network_audio_frame_decode(const uint8_t *buffer, size_t received_length, audio_frame_t *frame);

_Static_assert(AUDIO_FRAME_BYTES == ORATIO_MAX_PAYLOAD_SIZE, "Audio and network payload sizes differ");

/* Four complete records, including FreeRTOS's length prefix for each message.
 * xMessageBufferCreate() adds its internal ring-buffer sentinel byte.
 */
#define NETWORK_CAPTURE_BUFFER_BYTES (4U * (sizeof(audio_frame_t) + sizeof(configMESSAGE_BUFFER_LENGTH_TYPE)))
#define NETWORK_PLAYBACK_BUFFER_BYTES (4U * (sizeof(audio_frame_t) + sizeof(configMESSAGE_BUFFER_LENGTH_TYPE)))
