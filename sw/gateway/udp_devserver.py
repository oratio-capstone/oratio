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
        self.transport: asyncio.DatagramTransport | None = None
        self.emergency: set[int] = set()  # nodes whose wearer must be shown an emergency listen

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
            hb = protocol.parse_heartbeat(packet.payload)
            event = (messages.heartbeat(node_id, hb.rssi, role=hb.role, level=hb.level,
                                        parent_mac=hb.parent_mac, mac=hb.mac, ts=now)
                     if hb else messages.heartbeat(node_id, ts=now))
            messages.put_nowait_drop_oldest(self.q_in, event)
        elif packet.type == protocol.TYPE_SOS:
            messages.put_nowait_drop_oldest(self.q_in, messages.sos(node_id, now))
        elif packet.type == protocol.TYPE_TALK:
            pressed = protocol.parse_talk(packet.payload)
            if pressed is not None:
                messages.put_nowait_drop_oldest(self.q_in, messages.talk(node_id, pressed, now))
        elif packet.type == protocol.TYPE_AUDIO and node_id in self.listening:
            messages.put_nowait_drop_oldest(self.q_in, messages.audio(node_id, packet.payload))

    async def check_liveness(self) -> None:
        while True:
            await asyncio.sleep(1.0)
            for node_id in self.listening | self.emergency:  # resend while set: a lost packet heals
                self.send_control(node_id)
            now = time.time()
            for node_id, seen in list(self.last_seen.items()):
                if node_id not in self.dropped and now - seen > self.timeout:
                    self.dropped.add(node_id)
                    messages.put_nowait_drop_oldest(self.q_in, messages.node_dropped(node_id, now))

    async def read_commands(self) -> None:
        while True:
            cmd = await self.q_out.get()
            node_id = cmd.get("node_id")
            if cmd.get("type") == "listen":
                self.listening.add(node_id)
            elif cmd.get("type") == "stop":
                self.listening.discard(node_id)
            elif cmd.get("type") == "notify":
                (self.emergency.add if cmd["what"] == "emergency_listen_on" else self.emergency.discard)(node_id)
            self.send_control(node_id)

    def control_flags(self, node_id: int) -> int:
        flags = protocol.CONTROL_STREAM if node_id in self.listening else 0
        return flags | (protocol.CONTROL_EMERGENCY if node_id in self.emergency else 0)

    def send_control(self, node_id: int) -> None:
        """Tell a device whether to stream its microphone and whether to show an emergency listen."""
        addr = self.addresses.get(node_id)
        if addr is None or self.transport is None:
            return
        flags = self.control_flags(node_id)
        self.transport.sendto(protocol.build(protocol.TYPE_CONTROL, node_id, bytes([flags])), addr)


async def start(q_in: asyncio.Queue, q_out: asyncio.Queue, host: str, port: int,
                timeout: float = 3.0) -> UdpDevServer:
    server = UdpDevServer(q_in, q_out, timeout)
    loop = asyncio.get_running_loop()
    transport, _ = await loop.create_datagram_endpoint(lambda: _Protocol(server), local_addr=(host, port))
    server.transport = transport
    server.port = transport.get_extra_info("sockname")[1]  # the real port, useful when port=0
    server._tasks = [asyncio.create_task(server.check_liveness()),
                     asyncio.create_task(server.read_commands())]
    log.info("UDP stand-in listening on %s:%d (no forwarding between nodes)", host, server.port)
    return server
