"""UDP side of the gateway: ROUTING ONLY.

Receives packets from the mesh, reads the 7-byte header, keeps the node -> address
table, forwards audio between nodes (the open group channel, like sw/udp_server.py),
sends control packets, and passes everything else on Q in, raw. It decodes no
payloads and runs no timers: it is the critical path (if it stalls, nobody hears
anybody), so it does as little as possible. Decoding, liveness and everything the
dashboard shows happen on the WebSocket side (ws_server.py).

Stand-in for the asyncio version of sw/udp_server.py: that one must expose the same
`start(q_in, q_out, host, port)` coroutine and follow the contract in messages.py.
"""

from __future__ import annotations

import asyncio
import logging
import time

import messages
import protocol

log = logging.getLogger("udp")

CONTROL_RESEND_S = 1.0  # devices treat 3 s without a control packet as "all off"


class _Protocol(asyncio.DatagramProtocol):
    def __init__(self, server: "UdpServer") -> None:
        self.server = server

    def datagram_received(self, data: bytes, addr: tuple[str, int]) -> None:
        self.server.handle(data, addr)


class UdpServer:
    def __init__(self, q_in: asyncio.Queue, q_out: asyncio.Queue) -> None:
        self.q_in = q_in
        self.q_out = q_out
        self.addresses: dict[int, tuple[str, int]] = {}
        self.listening: set[int] = set()   # nodes a dashboard listens to: their audio goes on Q in
        self.emergency: set[int] = set()   # nodes whose wearer must be shown an emergency listen
        self.malformed = 0
        self.seq = 0                        # sequence number of the packets the gateway sends
        self.transport: asyncio.DatagramTransport | None = None

    def handle(self, data: bytes, addr: tuple[str, int]) -> None:
        packet = protocol.parse(data)
        if packet is None:  # shorter than the header, or its length field is wrong: can't route it
            self.malformed += 1
            return
        node_id = packet.node_id
        self.addresses[node_id] = addr

        if packet.type == protocol.TYPE_AUDIO:
            for other, other_addr in self.addresses.items():  # the group channel
                if other != node_id and self.transport is not None:
                    self.transport.sendto(data, other_addr)
            if node_id in self.listening:
                messages.put_nowait_drop_oldest(self.q_in, messages.audio(node_id, packet.payload))
            return

        messages.put_nowait_drop_oldest(self.q_in, messages.packet(
            node_id, packet.type.decode("ascii", "replace"), packet.payload,
            f"{addr[0]}:{addr[1]}", packet.seq, time.time()))

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

    async def resend_control(self) -> None:
        while True:
            await asyncio.sleep(CONTROL_RESEND_S)
            for node_id in self.listening | self.emergency:  # resend while set: a lost packet heals
                self.send_control(node_id)

    def control_flags(self, node_id: int) -> int:
        flags = protocol.CONTROL_STREAM if node_id in self.listening else 0
        return flags | (protocol.CONTROL_EMERGENCY if node_id in self.emergency else 0)

    def send_control(self, node_id: int) -> None:
        """Tell a device whether to stream its microphone and whether to show an emergency listen."""
        addr = self.addresses.get(node_id)
        if addr is None or self.transport is None:
            return
        self.seq = (self.seq + 1) & 0xFFFF
        packet = protocol.build(protocol.TYPE_CONTROL, node_id, bytes([self.control_flags(node_id)]), self.seq)
        self.transport.sendto(packet, addr)


UdpDevServer = UdpServer  # old name, kept for anything that still imports it


async def start(q_in: asyncio.Queue, q_out: asyncio.Queue, host: str, port: int) -> UdpServer:
    server = UdpServer(q_in, q_out)
    loop = asyncio.get_running_loop()
    transport, _ = await loop.create_datagram_endpoint(lambda: _Protocol(server), local_addr=(host, port))
    server.transport = transport
    server.port = transport.get_extra_info("sockname")[1]  # the real port, useful when port=0
    server._tasks = [asyncio.create_task(server.read_commands()),
                     asyncio.create_task(server.resend_control())]
    log.info("UDP server (routing only) listening on %s:%d", host, server.port)
    return server
