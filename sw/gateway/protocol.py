"""Wire format between mesh nodes and the gateway.

Single source of truth for the gateway and its test tools. It must match what
the firmware's network task sends.

Current layout (taken from the existing udp_server.py; to be confirmed with
the firmware team):

    byte 0      message type, one ASCII character
    bytes 1-2   node_id, unsigned 16-bit, big-endian
    bytes 3..   payload (depends on the type)

The heartbeat payload layout and the 'A' / 'S' type characters below are
proposals until the firmware defines them.
"""

from __future__ import annotations

import struct
from typing import NamedTuple

HEADER = struct.Struct(">cH")
HEADER_SIZE = HEADER.size  # 3 bytes

TYPE_HEARTBEAT = b"H"
TYPE_AUDIO = b"A"  # proposal
TYPE_SOS = b"S"  # proposal

# Heartbeat payload (proposal): link RSSI in dBm (int8), relative position x, y in metres (float32).
HEARTBEAT = struct.Struct(">bff")

# Shared audio format (fw/lib/audio/audio_common.h): 16 kHz, 16-bit PCM, 20 ms frames.
SAMPLE_RATE = 16_000
FRAME_MS = 20
AUDIO_FRAME_BYTES = SAMPLE_RATE * FRAME_MS // 1000 * 2  # 640


class Packet(NamedTuple):
    type: bytes
    node_id: int
    payload: bytes


def parse(data: bytes) -> Packet | None:
    """Split a datagram into header fields and payload. Returns None if it is too short."""
    if len(data) < HEADER_SIZE:
        return None
    msg_type, node_id = HEADER.unpack_from(data)
    return Packet(msg_type, node_id, data[HEADER_SIZE:])


def build(msg_type: bytes, node_id: int, payload: bytes = b"") -> bytes:
    """Encode a datagram. Used by the simulator and tests."""
    return HEADER.pack(msg_type, node_id) + payload


def parse_heartbeat(payload: bytes) -> tuple[int, float, float] | None:
    """Return (rssi, x, y), or None if the heartbeat carries no telemetry."""
    if len(payload) < HEARTBEAT.size:
        return None
    return HEARTBEAT.unpack_from(payload)


def build_heartbeat(rssi: int, x: float, y: float) -> bytes:
    return HEARTBEAT.pack(rssi, x, y)
