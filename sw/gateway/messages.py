"""Messages on the two queues inside the gateway process.

This is the contract between the UDP side and the WebSocket side. Both halves
import from here instead of writing dictionaries by hand.

Q in  (UDP -> WebSocket)
    {"type": "node_joined",          "node_id": int, "addr": "ip:port", "ts": float}
    {"type": "node_address_changed", "node_id": int, "addr": "ip:port", "ts": float}
    {"type": "heartbeat",            "node_id": int, "ts": float, "link": int|None, "pos": [x, y]|None}
    {"type": "node_dropped",         "node_id": int, "ts": float}
    {"type": "sos",                  "node_id": int, "ts": float}
    ("audio", node_id, pcm_bytes)    a tuple, kept light because it arrives 50 times a second per talker;
                                     only sent for nodes a dashboard is listening to

Q out (WebSocket -> UDP)
    {"type": "listen", "node_id": int}   start sending this node's audio on Q in
    {"type": "stop",   "node_id": int}   stop sending it
"""

from __future__ import annotations

import asyncio
import time
from typing import Any

AUDIO = "audio"


def node_joined(node_id: int, addr: str, ts: float | None = None) -> dict[str, Any]:
    return {"type": "node_joined", "node_id": node_id, "addr": addr, "ts": ts or time.time()}


def node_address_changed(node_id: int, addr: str, ts: float | None = None) -> dict[str, Any]:
    return {"type": "node_address_changed", "node_id": node_id, "addr": addr, "ts": ts or time.time()}


def heartbeat(node_id: int, link: int | None = None, pos: list[float] | None = None,
              ts: float | None = None) -> dict[str, Any]:
    return {"type": "heartbeat", "node_id": node_id, "link": link, "pos": pos, "ts": ts or time.time()}


def node_dropped(node_id: int, ts: float | None = None) -> dict[str, Any]:
    return {"type": "node_dropped", "node_id": node_id, "ts": ts or time.time()}


def sos(node_id: int, ts: float | None = None) -> dict[str, Any]:
    return {"type": "sos", "node_id": node_id, "ts": ts or time.time()}


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
