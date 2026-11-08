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

Heartbeat payload (proposal sent to the firmware team), 15 bytes:

    byte 3        role: 0 = worker (worn by a person), 1 = relay (fixed in place)
    byte 4        mesh level, root = 1                   esp_mesh_lite_get_level()
    byte 5        RSSI to parent in dBm, signed           esp_wifi_sta_get_ap_info()
    bytes 6-11    parent's BSSID (its SoftAP MAC)        esp_wifi_sta_get_ap_info()
    bytes 12-17   this node's own SoftAP MAC             esp_wifi_get_mac(WIFI_IF_AP)

The gateway matches each node's parent BSSID to the node with that SoftAP MAC,
which gives the mesh tree. A level-1 node's parent is the Pi's access point.
"""

from __future__ import annotations

import struct
from typing import NamedTuple

HEADER = struct.Struct(">cH")
HEADER_SIZE = HEADER.size  # 3 bytes

TYPE_HEARTBEAT = b"H"
TYPE_AUDIO = b"A"  # proposal
TYPE_SOS = b"S"  # proposal
TYPE_TALK = b"P"  # proposal: talk button, payload 1 byte (1 = pressed, 0 = released)
TYPE_CONTROL = b"N"  # proposal, gateway -> node: payload 1 byte of flags, see CONTROL_* below

# Talk button: the node sends 'P' 1 when pressed, repeats it about once a second while held
# (so a lost packet doesn't matter), and 'P' 0 when released.
#
# Control (gateway -> node): a device sends microphone audio while its talk button is held,
# OR while the gateway asks it to (CONTROL_STREAM): during an SOS or an emergency listen,
# when the wearer may not be able to press anything. CONTROL_EMERGENCY means an emergency
# listen is on: the device shows the wearer (LED / beep). The gateway resends the flags about
# once a second while any is set, so the device can treat 3 s of silence as "all off".
CONTROL_STREAM = 0x01
CONTROL_EMERGENCY = 0x02

HEARTBEAT = struct.Struct(">BBb6s6s")
ROLE_WORKER = 0
ROLE_RELAY = 1
ROLES = {ROLE_WORKER: "worker", ROLE_RELAY: "relay"}

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


def parse_talk(payload: bytes) -> bool | None:
    """True = talk button pressed, False = released, None = malformed."""
    return bool(payload[0]) if payload else None


def build_talk(pressed: bool) -> bytes:
    return bytes([1 if pressed else 0])


class Heartbeat(NamedTuple):
    role: str          # "worker" or "relay"
    level: int         # hops from the Pi; root = 1
    rssi: int          # signal to parent, dBm
    parent_mac: str    # "aa:bb:cc:dd:ee:ff"
    mac: str           # this node's SoftAP MAC


def mac_str(raw: bytes) -> str:
    return ":".join(f"{b:02x}" for b in raw)


def mac_bytes(mac: str) -> bytes:
    return bytes(int(part, 16) for part in mac.split(":"))


def parse_heartbeat(payload: bytes) -> Heartbeat | None:
    """Decode the telemetry, or None if the heartbeat is too short to carry it."""
    if len(payload) < HEARTBEAT.size:
        return None
    role, level, rssi, parent, own = HEARTBEAT.unpack_from(payload)
    return Heartbeat(ROLES.get(role, "worker"), level, rssi, mac_str(parent), mac_str(own))


def build_heartbeat(role: str, level: int, rssi: int, parent_mac: str, mac: str) -> bytes:
    role_code = ROLE_RELAY if role == "relay" else ROLE_WORKER
    return HEARTBEAT.pack(role_code, level, rssi, mac_bytes(parent_mac), mac_bytes(mac))
