"""Temporary UDP side of the gateway, for developing the dashboard.

STAND-IN ONLY. It receives packets, tracks which nodes are alive, and reports
to Q in using the contract in messages.py, so the WebSocket server and the
simulator can be built and tested now. It does NOT forward audio between nodes.
The real UDP server (sw/udp_server.py, being moved to asyncio) replaces this
module: it must expose the same `start(q_in, q_out, host, port)` coroutine.
"""

from __future__ import annotations

import asyncio
import logging
import time

import messages
import protocol

log = logging.getLogger("udp")


class _Protocol(asyncio.DatagramProtocol):
    def __init__(self, server: "UdpDevServer") -> None:
        self.server = server

    def datagram_received(self, data: bytes, addr: tuple[str, int]) -> None:
        self.server.handle(data, addr)


class UdpDevServer:
    def __init__(self, q_in: asyncio.Queue, q_out: asyncio.Queue, timeout: float) -> None:
        self.q_in = q_in
        self.q_out = q_out
        self.timeout = timeout
        self.addresses: dict[int, tuple[str, int]] = {}
        self.last_seen: dict[int, float] = {}
        self.dropped: set[int] = set()
        self.listening: set[int] = set()
        self.malformed = 0

    def handle(self, data: bytes, addr: tuple[str, int]) -> None:
        packet = protocol.parse(data)
        if packet is None:
            self.malformed += 1
            return
        node_id, now = packet.node_id, time.time()
        addr_text = f"{addr[0]}:{addr[1]}"

        if node_id not in self.addresses:
            self.addresses[node_id] = addr
            messages.put_nowait_drop_oldest(self.q_in, messages.node_joined(node_id, addr_text, now))
        elif self.addresses[node_id] != addr:
            self.addresses[node_id] = addr
            messages.put_nowait_drop_oldest(self.q_in, messages.node_address_changed(node_id, addr_text, now))
        self.last_seen[node_id] = now
        self.dropped.discard(node_id)

        if packet.type == protocol.TYPE_HEARTBEAT:
            telemetry = protocol.parse_heartbeat(packet.payload)
            link, pos = (telemetry[0], [telemetry[1], telemetry[2]]) if telemetry else (None, None)
            messages.put_nowait_drop_oldest(self.q_in, messages.heartbeat(node_id, link, pos, now))
        elif packet.type == protocol.TYPE_SOS:
            messages.put_nowait_drop_oldest(self.q_in, messages.sos(node_id, now))
        elif packet.type == protocol.TYPE_AUDIO and node_id in self.listening:
            messages.put_nowait_drop_oldest(self.q_in, messages.audio(node_id, packet.payload))

    async def check_liveness(self) -> None:
        while True:
            await asyncio.sleep(1.0)
            now = time.time()
            for node_id, seen in list(self.last_seen.items()):
                if node_id not in self.dropped and now - seen > self.timeout:
                    self.dropped.add(node_id)
                    messages.put_nowait_drop_oldest(self.q_in, messages.node_dropped(node_id, now))

    async def read_commands(self) -> None:
        while True:
            cmd = await self.q_out.get()
            if cmd.get("type") == "listen":
                self.listening.add(cmd["node_id"])
            elif cmd.get("type") == "stop":
                self.listening.discard(cmd["node_id"])


async def start(q_in: asyncio.Queue, q_out: asyncio.Queue, host: str, port: int,
                timeout: float = 3.0) -> UdpDevServer:
    server = UdpDevServer(q_in, q_out, timeout)
    loop = asyncio.get_running_loop()
    transport, _ = await loop.create_datagram_endpoint(lambda: _Protocol(server), local_addr=(host, port))
    server.port = transport.get_extra_info("sockname")[1]  # the real port, useful when port=0
    server._tasks = [asyncio.create_task(server.check_liveness()),
                     asyncio.create_task(server.read_commands())]
    log.info("UDP stand-in listening on %s:%d (no forwarding between nodes)", host, server.port)
    return server
