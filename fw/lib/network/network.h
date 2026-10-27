#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t mesh_network_init(void);
void mesh_network_log_status(void);
esp_err_t network_task_start(void);

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

esp_err_t oratio_packet_encode(
    const oratio_packet_t *packet,
    uint8_t *buffer,
    size_t buffer_capacity,
    size_t *encoded_length);

esp_err_t oratio_packet_decode(
    const uint8_t *buffer,
    size_t received_length,
    oratio_packet_t *packet);