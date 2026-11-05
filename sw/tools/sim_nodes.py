"""Node simulator (task 169): fake mesh nodes that talk to the gateway over UDP.

Each node has its own UDP socket (so its own source port, like a real node) and sends:
  - a heartbeat every second with a drifting RSSI and position
  - if it is a talker, a 20 ms audio frame every 20 ms: a speech-like signal (bursts like
    syllables with short pauses), a different pitch per node
Optional events: a node going silent (dropped) or sending an SOS at a given time.

Examples:
  python sw/tools/sim_nodes.py                          # 10 nodes, heartbeats only, to 127.0.0.1:8989
  python sw/tools/sim_nodes.py --nodes 6 --talkers 2    # nodes 1 and 2 also send audio
  python sw/tools/sim_nodes.py --drop 3:10 --sos 5:15   # node 3 goes silent at 10 s, node 5 sends SOS at 15 s
  python sw/tools/sim_nodes.py --host 192.168.4.1       # against the Pi
"""

from __future__ import annotations

import argparse
import asyncio
import math
import random
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gateway"))
import protocol  # noqa: E402  (shared wire format)

SAMPLES_PER_FRAME = protocol.AUDIO_FRAME_BYTES // 2


class _Ignore(asyncio.DatagramProtocol):
    """Nodes don't need to read what the gateway forwards to them."""


class SimNode:
    def __init__(self, node_id: int, talker: bool, rng: random.Random) -> None:
        self.node_id = node_id
        self.talker = talker
        self.rng = rng
        self.rssi = rng.uniform(-75, -45)
        self.x, self.y = rng.uniform(0, 20), rng.uniform(0, 20)
        self.tone_hz = 220 + 55 * node_id
        self.phase = 0.0
        self.env = 0.0          # current loudness, 0..1
        self.target = 0.0       # loudness the envelope is moving towards
        self.burst_left = 0     # frames left in the current syllable or pause
        self.silent = False
        self.transport: asyncio.DatagramTransport | None = None
        self.sent = 0

    async def connect(self, host: str, port: int) -> None:
        loop = asyncio.get_running_loop()
        self.transport, _ = await loop.create_datagram_endpoint(_Ignore, remote_addr=(host, port))

    def send(self, msg_type: bytes, payload: bytes = b"") -> None:
        if self.silent or self.transport is None:
            return
        self.transport.sendto(protocol.build(msg_type, self.node_id, payload))
        self.sent += 1

    async def heartbeats(self, interval: float) -> None:
        while True:
            self.rssi = max(-95, min(-35, self.rssi + self.rng.uniform(-3, 3)))
            self.x = max(0.0, min(20.0, self.x + self.rng.uniform(-0.5, 0.5)))
            self.y = max(0.0, min(20.0, self.y + self.rng.uniform(-0.5, 0.5)))
            self.send(protocol.TYPE_HEARTBEAT, protocol.build_heartbeat(round(self.rssi), self.x, self.y))
            await asyncio.sleep(interval)

    def _audio_frame(self) -> bytes:
        """20 ms of a speech-like test signal: a voiced tone with harmonics whose loudness
        follows random 'syllables' (0.1-0.35 s) separated by short pauses."""
        if self.burst_left <= 0:
            speaking = self.rng.random() < 0.75
            self.burst_left = self.rng.randint(5, 18) if speaking else self.rng.randint(2, 10)  # in frames
            self.target = self.rng.uniform(0.35, 1.0) if speaking else 0.0
        self.burst_left -= 1
        loudness = self.target * self.rng.uniform(0.45, 1.0)  # frame-to-frame variation, like real speech
        step = 2 * math.pi * self.tone_hz / protocol.SAMPLE_RATE
        samples = []
        for _ in range(SAMPLES_PER_FRAME):
            self.env += (loudness - self.env) * 0.004  # smooth attack and decay (~15 ms)
            voice = 0.6 * math.sin(self.phase) + 0.3 * math.sin(2 * self.phase) + 0.1 * math.sin(3 * self.phase)
            noise = self.rng.uniform(-0.08, 0.08)
            samples.append(int(max(-1.0, min(1.0, self.env * (voice + noise))) * 12000))
            self.phase = (self.phase + step) % (2 * math.pi)
        return struct.pack(f"<{SAMPLES_PER_FRAME}h", *samples)  # 16-bit little-endian PCM

    async def audio(self) -> None:
        loop = asyncio.get_running_loop()
        frame_s = protocol.FRAME_MS / 1000
        next_at = loop.time()
        while True:
            self.send(protocol.TYPE_AUDIO, self._audio_frame())
            next_at += frame_s  # schedule from a fixed clock so timing doesn't drift
            await asyncio.sleep(max(0.0, next_at - loop.time()))


def _parse_events(values: list[str], flag: str) -> list[tuple[int, float]]:
    events = []
    for value in values:
        try:
            node, at = value.split(":")
            events.append((int(node), float(at)))
        except ValueError:
            sys.exit(f"{flag} expects NODE:SECONDS, got {value!r}")
    return events


async def run(args: argparse.Namespace) -> None:
    rng = random.Random(args.seed)
    nodes = [SimNode(i, talker=i <= args.talkers, rng=rng) for i in range(1, args.nodes + 1)]
    by_id = {n.node_id: n for n in nodes}
    for node in nodes:
        await node.connect(args.host, args.port)

    tasks = [asyncio.create_task(n.heartbeats(args.heartbeat)) for n in nodes]
    tasks += [asyncio.create_task(n.audio()) for n in nodes if n.talker]

    async def at(seconds: float, action, label: str) -> None:
        await asyncio.sleep(seconds)
        action()
        print(f"[{seconds:6.1f}s] {label}")

    for node_id, t in _parse_events(args.drop, "--drop"):
        if node_id in by_id:
            tasks.append(asyncio.create_task(
                at(t, lambda n=by_id[node_id]: setattr(n, "silent", True), f"node {node_id} goes silent")))
    for node_id, t in _parse_events(args.sos, "--sos"):
        if node_id in by_id:
            tasks.append(asyncio.create_task(
                at(t, lambda n=by_id[node_id]: n.send(protocol.TYPE_SOS), f"node {node_id} sends SOS")))

    talkers = ", ".join(str(n.node_id) for n in nodes if n.talker) or "none"
    print(f"Simulating {len(nodes)} nodes -> {args.host}:{args.port} (talkers: {talkers}). Ctrl+C to stop.")
    start = time.monotonic()
    try:
        if args.duration:
            await asyncio.sleep(args.duration)
        else:
            await asyncio.Event().wait()
    finally:
        for task in tasks:
            task.cancel()
        elapsed = time.monotonic() - start
        total = sum(n.sent for n in nodes)
        print(f"Sent {total} packets in {elapsed:.1f} s ({total / max(elapsed, 0.001):.0f}/s)")


def main() -> None:
    parser = argparse.ArgumentParser(description="Simulate Oratio mesh nodes over UDP.")
    parser.add_argument("--host", default="127.0.0.1", help="gateway address (default 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8989, help="gateway UDP port (default 8989)")
    parser.add_argument("--nodes", type=int, default=10, help="number of nodes (default 10)")
    parser.add_argument("--talkers", type=int, default=0, help="how many nodes (1..K) also send audio")
    parser.add_argument("--heartbeat", type=float, default=1.0, help="seconds between heartbeats")
    parser.add_argument("--drop", action="append", default=[], metavar="NODE:SECONDS",
                        help="node goes silent at this time (repeatable)")
    parser.add_argument("--sos", action="append", default=[], metavar="NODE:SECONDS",
                        help="node sends an SOS at this time (repeatable)")
    parser.add_argument("--duration", type=float, default=0, help="stop after N seconds (default: run forever)")
    parser.add_argument("--seed", type=int, default=None, help="random seed for repeatable runs")
    args = parser.parse_args()
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
