"""Messages on the two queues inside the gateway process.

This is the contract between the UDP side and the WebSocket side. Both halves
import from here instead of writing dictionaries by hand.

The UDP side only ROUTES: it reads the 7-byte header, keeps the node -> address
table, forwards audio and sends control packets. It never decodes payloads or
keeps timers, so nothing a node sends can break routing. Everything that needs
understanding (decoding heartbeats, liveness, topology, SOS) is on the WebSocket side.

Q in  (UDP -> WebSocket)
    {"type": "packet", "node_id": int, "kind": "H"|"S"|"P"|"T"|..., "seq": int,
     "payload": bytes, "addr": "ip:port", "ts": float}
                                     every packet that is not audio, raw (header split off)
    ("audio", node_id, pcm_bytes)    a tuple, kept light because it arrives 50 times a second per talker;
                                     only sent for nodes a dashboard is listening to

Inside the WebSocket side, packets are decoded into these events (also handy in tests):
    {"type": "node_joined" | "node_address_changed", "node_id": int, "addr": "ip:port", "ts": float}
    {"type": "heartbeat",            "node_id": int, "ts": float, "link": int|None,
                                      "role": "worker"|"relay"|None, "level": int|None,
                                      "parent_mac": str|None, "mac": str|None}
                                     (None = the heartbeat carried no telemetry; keep the last value)
    {"type": "node_dropped",         "node_id": int, "ts": float}   from the WebSocket side's own timer
    {"type": "sos",                  "node_id": int, "ts": float}
    {"type": "talk",                 "node_id": int, "pressed": bool, "ts": float}   talk button

Q out (WebSocket -> UDP)
    {"type": "listen", "node_id": int}   start sending this node's audio on Q in
    {"type": "stop",   "node_id": int}   stop sending it
    {"type": "notify", "node_id": int, "what": "emergency_listen_on" | "emergency_listen_off"}
                                         tell the device to show this to the wearer (LED / beep)
"""

from __future__ import annotations

import asyncio
import time
from typing import Any

AUDIO = "audio"


def packet(node_id: int, kind: str, payload: bytes, addr: str, seq: int = 0,
           ts: float | None = None) -> dict[str, Any]:
    """A raw packet from a node, as the UDP side passes it on (header already split off)."""
    return {"type": "packet", "node_id": node_id, "kind": kind, "seq": seq, "payload": payload,
            "addr": addr, "ts": ts or time.time()}


def node_joined(node_id: int, addr: str, ts: float | None = None) -> dict[str, Any]:
    return {"type": "node_joined", "node_id": node_id, "addr": addr, "ts": ts or time.time()}


def node_address_changed(node_id: int, addr: str, ts: float | None = None) -> dict[str, Any]:
    return {"type": "node_address_changed", "node_id": node_id, "addr": addr, "ts": ts or time.time()}


def heartbeat(node_id: int, link: int | None = None, *, role: str | None = None,
              level: int | None = None, parent_mac: str | None = None, mac: str | None = None,
              ts: float | None = None) -> dict[str, Any]:
    return {"type": "heartbeat", "node_id": node_id, "link": link, "role": role, "level": level,
            "parent_mac": parent_mac, "mac": mac, "ts": ts or time.time()}


def node_dropped(node_id: int, ts: float | None = None) -> dict[str, Any]:
    return {"type": "node_dropped", "node_id": node_id, "ts": ts or time.time()}


def sos(node_id: int, ts: float | None = None) -> dict[str, Any]:
    return {"type": "sos", "node_id": node_id, "ts": ts or time.time()}


def talk(node_id: int, pressed: bool, ts: float | None = None) -> dict[str, Any]:
    return {"type": "talk", "node_id": node_id, "pressed": pressed, "ts": ts or time.time()}


def notify(node_id: int, what: str) -> dict[str, Any]:
    return {"type": "notify", "node_id": node_id, "what": what}


def audio(node_id: int, pcm: bytes) -> tuple[str, int, bytes]:
    return (AUDIO, node_id, pcm)


def listen(node_id: int) -> dict[str, Any]:
    return {"type": "listen", "node_id": node_id}


def stop(node_id: int) -> dict[str, Any]:
    return {"type": "stop", "node_id": node_id}


def put_nowait_drop_oldest(queue: asyncio.Queue, item: Any) -> None:
    """Put without waiting; if the queue is full, discard the oldest item first.

    Late audio is useless, so a full queue loses old frames instead of blocking
    the receiver or growing without limit.
    """
    try:
        queue.put_nowait(item)
    except asyncio.QueueFull:
        try:
            queue.get_nowait()
        except asyncio.QueueEmpty:
            pass
        queue.put_nowait(item)
