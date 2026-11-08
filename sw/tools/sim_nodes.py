"""Node simulator (task 169): a fake mesh in a tunnel that talks to the gateway over UDP.

The world: a straight tunnel with the Pi at 0 m and fixed relays every --spacing metres
(ids 101, 102, ...; relay 101 is the mesh root). Workers (ids 1..N, worn by people) walk
up and down the tunnel and join the relay they hear best, switching only when another
relay is clearly stronger, the way Mesh-Lite does. Signal strength falls with distance.

Each node has its own UDP socket (so its own source port, like a real node) and sends:
  - a heartbeat every second: role, mesh level, RSSI to parent, parent MAC, own MAC
  - if it is a talker, a 20 ms audio frame every 20 ms: a speech-like signal (bursts like
    syllables with short pauses), a different pitch per node
Optional events: a node going silent (dropped) or sending an SOS at a given time.
When a relay goes silent, everything behind it goes quiet for --reroute seconds while
the mesh finds new parents, then carries on through another relay.

Examples:
  python sw/tools/sim_nodes.py                          # 3 relays + 10 workers, to 127.0.0.1:8989
  python sw/tools/sim_nodes.py --nodes 6 --talkers 2    # workers 1 and 2 also send audio
  python sw/tools/sim_nodes.py --drop 3:10 --sos 5:15   # worker 3 goes silent at 10 s, worker 5 sends SOS at 15 s
  python sw/tools/sim_nodes.py --drop 102:20            # relay 102 dies at 20 s; watch the mesh reroute
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
RELAY_ID_BASE = 100
PI_MAC = "02:00:00:ff:ff:ff"   # BSSID of the Pi's access point
SWITCH_MARGIN_DB = 6           # a worker changes relay only if the new one is this much stronger


def fake_mac(node_id: int) -> str:
    return f"02:00:00:00:{node_id >> 8:02x}:{node_id & 0xFF:02x}"


def rssi_at(distance_m: float, rng: random.Random) -> float:
    """Rough indoor path loss: -40 dBm at 1 m, falling ~22 dB per tenfold distance, plus noise."""
    return -40 - 22 * math.log10(max(distance_m, 1.0)) + rng.uniform(-3, 3)


class _Ignore(asyncio.DatagramProtocol):
    """Nodes don't need to read what the gateway forwards to them."""


class SimNode:
    def __init__(self, node_id: int, talker: bool, rng: random.Random, *,
                 role: str = "worker", x: float = 0.0) -> None:
        self.node_id = node_id
        self.talker = talker
        self.rng = rng
        self.role = role
        self.mac = fake_mac(node_id)
        self.x = x                      # metres along the tunnel (the Pi is at 0)
        self.speed = 0.0                # metres per second, negative = towards the Pi
        self.parent: SimNode | None = None   # None = the Pi itself (root only)
        self.level = 1
        self.rssi = -60
        self.offline_until = 0.0        # cut off while the mesh reroutes around a dead relay
        self.tone_hz = 220 + 55 * (node_id % 12)
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
        if self.silent or self.transport is None or time.monotonic() < self.offline_until:
            return
        self.transport.sendto(protocol.build(msg_type, self.node_id, payload))
        self.sent += 1

    def heartbeat(self) -> None:
        parent_mac = self.parent.mac if self.parent else PI_MAC
        self.send(protocol.TYPE_HEARTBEAT, protocol.build_heartbeat(
            self.role, self.level, max(-100, min(-20, round(self.rssi))), parent_mac, self.mac))

    def ancestors(self) -> list[SimNode]:
        chain, node = [], self.parent
        while node is not None and node not in chain:
            chain.append(node)
            node = node.parent
        return chain

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


class Tunnel:
    """Moves the workers, picks every node's parent, and sends the heartbeats."""

    def __init__(self, relays: list[SimNode], workers: list[SimNode], reroute_s: float,
                 rng: random.Random) -> None:
        self.relays = relays
        self.workers = workers
        self.reroute_s = reroute_s
        self.rng = rng
        self.length = (relays[-1].x + 40) if relays else 60

    def alive(self, node: SimNode) -> bool:
        return not node.silent

    def _attach(self, node: SimNode, parent: SimNode | None) -> None:
        node.parent = parent
        node.level = 1 if parent is None else parent.level + 1
        node.rssi = rssi_at(abs(node.x - (parent.x if parent else 0.0)), self.rng)

    def relay_died(self, dead: SimNode) -> None:
        """Everything whose path ran through the dead relay is cut off while the mesh reroutes."""
        until = time.monotonic() + self.reroute_s
        for node in self.relays + self.workers:
            if dead in node.ancestors():
                node.offline_until = max(node.offline_until, until)

    def step(self, dt: float) -> None:
        now = time.monotonic()
        # Relays don't move. Each joins the nearest live relay closer to the Pi (or the Pi).
        for relay in self.relays:
            if now < relay.offline_until and relay.parent and not self.alive(relay.parent):
                continue  # still rerouting
            upstream = [r for r in self.relays if r.x < relay.x and self.alive(r)]
            self._attach(relay, max(upstream, key=lambda r: r.x) if upstream else None)
        # Workers walk, pause and turn around now and then.
        for w in self.workers:
            if self.rng.random() < 0.05 * dt:
                w.speed = 0.0 if self.rng.random() < 0.3 else self.rng.choice([-1, 1]) * self.rng.uniform(0.6, 1.5)
            w.x += w.speed * dt
            if not 2 <= w.x <= self.length:
                w.x = min(max(w.x, 2.0), self.length)
                w.speed = -w.speed
            if now < w.offline_until and w.parent and not self.alive(w.parent):
                continue  # still rerouting
            live = [r for r in self.relays if self.alive(r)]
            if not live:
                continue
            heard = {r: rssi_at(abs(w.x - r.x), self.rng) for r in live}
            best = max(heard, key=heard.get)
            current = w.parent if w.parent in heard else None
            if current is None or heard[best] > heard[current] + SWITCH_MARGIN_DB:
                current = best
            self._attach(w, current)
            w.rssi = heard[current]
        for node in self.relays + self.workers:
            node.heartbeat()

    async def run(self, interval: float) -> None:
        while True:
            self.step(interval)
            await asyncio.sleep(interval)


async def run(args: argparse.Namespace) -> None:
    rng = random.Random(args.seed)
    relays = [SimNode(RELAY_ID_BASE + i, talker=False, rng=rng, role="relay", x=args.spacing * (i - 0.5))
              for i in range(1, args.relays + 1)]
    length = (relays[-1].x + 40) if relays else 60
    workers = [SimNode(i, talker=i <= args.talkers, rng=rng, x=rng.uniform(2, length))
               for i in range(1, args.nodes + 1)]
    nodes = relays + workers
    by_id = {n.node_id: n for n in nodes}
    for node in nodes:
        await node.connect(args.host, args.port)

    tunnel = Tunnel(relays, workers, args.reroute, rng)
    tasks = [asyncio.create_task(tunnel.run(args.heartbeat))]
    tasks += [asyncio.create_task(n.audio()) for n in nodes if n.talker]

    async def at(seconds: float, action, label: str) -> None:
        await asyncio.sleep(seconds)
        action()
        print(f"[{seconds:6.1f}s] {label}")

    for node_id, t in _parse_events(args.drop, "--drop"):
        if node_id in by_id:
            def go_silent(n: SimNode = by_id[node_id]) -> None:
                n.silent = True
                if n.role == "relay":
                    tunnel.relay_died(n)
            kind = "relay" if by_id[node_id].role == "relay" else "worker"
            tasks.append(asyncio.create_task(at(t, go_silent, f"{kind} {node_id} goes silent")))
    for node_id, t in _parse_events(args.sos, "--sos"):
        if node_id in by_id:
            tasks.append(asyncio.create_task(
                at(t, lambda n=by_id[node_id]: n.send(protocol.TYPE_SOS), f"worker {node_id} sends SOS")))

    talkers = ", ".join(str(n.node_id) for n in nodes if n.talker) or "none"
    relay_ids = ", ".join(str(r.node_id) for r in relays) or "none"
    print(f"Simulating {len(relays)} relays ({relay_ids}) + {len(workers)} workers -> "
          f"{args.host}:{args.port} (talkers: {talkers}). Ctrl+C to stop.")
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
    parser.add_argument("--nodes", type=int, default=10, help="number of workers, ids 1..N (default 10)")
    parser.add_argument("--relays", type=int, default=3, help="number of fixed relays, ids 101.. (default 3)")
    parser.add_argument("--spacing", type=float, default=60.0, help="metres between relays (default 60)")
    parser.add_argument("--reroute", type=float, default=2.0,
                        help="seconds the mesh needs to route around a dead relay (default 2)")
    parser.add_argument("--talkers", type=int, default=0, help="how many workers (1..K) also send audio")
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
