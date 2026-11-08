"""WebSocket server: the dashboard half of the gateway process (task 168).

Reads events from Q in, keeps the display state (nodes, alerts), and pushes it
to every connected browser. Commands from browsers ("listen" / "stop") go to
the UDP side through Q out.

Browser protocol (all text frames are JSON):
    server -> browser   {"type": "snapshot", "nodes": [...], "alerts": [...], "open_sos": [...]}   on connect
                        {"type": "node_update", "node": {...}}
                        {"type": "alert", "alert": {...}}
                        {"type": "sos_update", "alert": {...}}   an open SOS repeated or was acknowledged
                        binary: 2-byte node_id (big-endian) + 16-bit PCM, for nodes being listened to
    browser -> server   {"type": "listen", "node_id": 3}
                        {"type": "stop", "node_id": 3}   (or without node_id: stop everything)
                        {"type": "rename", "node_id": 3, "name": "Alex"}   ("" clears the name)
                        {"type": "ack", "alert_id": 12}   acknowledge an SOS

Every alert has an "id". An SOS stays open (shown as a banner on every dashboard)
until one dashboard acknowledges it. A node that keeps sending SOS while its alert
is open bumps "count" and "last_ts" on that alert instead of adding new ones.

Node names (e.g. the person wearing the device) are shared by every dashboard and
saved to a small JSON file, so they survive a gateway restart.

Mesh tree: each node dict carries "role" ("worker" / "relay"), "level", and
"parent" (the parent's node_id, 0 for the Pi itself, None while unknown), worked
out by matching a node's parent MAC to another node's own MAC. "near" is the
first relay above a worker, which is the closest thing we have to a location.
"""

from __future__ import annotations

import asyncio
import collections
import json
import logging
import os
import time
import unicodedata
from pathlib import Path
from typing import Any

from websockets.asyncio.server import Server, ServerConnection, serve
from websockets.exceptions import ConnectionClosed

import messages

log = logging.getLogger("ws")

MAX_NAME_LENGTH = 32


def clean_name(name: Any) -> str:
    """Trim, drop control characters and limit length. Returns "" for no name."""
    if not isinstance(name, str):
        return ""
    name = "".join(ch for ch in name if unicodedata.category(ch)[0] != "C")
    return " ".join(name.split())[:MAX_NAME_LENGTH]


class _Client:
    def __init__(self, ws: ServerConnection, buffer: int) -> None:
        self.ws = ws
        self.outbox: asyncio.Queue[str | bytes] = asyncio.Queue(maxsize=buffer)
        self.listening: set[int] = set()


GATEWAY = 0  # "parent" value for level-1 nodes: their parent is the Pi itself


class WebSocketServer:
    def __init__(self, q_in: asyncio.Queue, q_out: asyncio.Queue, *, path: str = "/ws",
                 client_buffer: int = 128, alert_history: int = 50,
                 names_file: str | Path | None = None) -> None:
        self.q_in = q_in
        self.q_out = q_out
        self.path = path
        self.client_buffer = client_buffer
        self.nodes: dict[int, dict[str, Any]] = {}
        self.alerts: collections.deque[dict[str, Any]] = collections.deque(maxlen=alert_history)
        # Unacknowledged SOS alerts by node. Kept apart from the history so an SOS can
        # never scroll out of view before someone has seen it.
        self.open_sos: dict[int, dict[str, Any]] = {}
        self._by_mac: dict[str, int] = {}  # SoftAP MAC -> node_id, to resolve parents
        # Recent paths to the Pi per node: [(since_ts, (parent, grandparent, ...)), ...].
        # A relay is only declared dead after the heartbeat timeout, and by then the mesh may
        # have rerouted its children, so the alert looks up who was behind it when last heard.
        self._paths: dict[int, list[tuple[float, tuple[int, ...]]]] = {}
        self._next_alert_id = 1
        self._clients: set[_Client] = set()
        self._listeners: collections.Counter[int] = collections.Counter()
        self.names_file = Path(names_file) if names_file else None
        self.names: dict[int, str] = self._load_names()

    # ---- running -------------------------------------------------------------

    async def start(self, host: str, port: int) -> Server:
        """Start accepting connections and consuming Q in. Returns the listening server."""
        server = await serve(self._handle_connection, host, port)
        self._consumer = asyncio.create_task(self._consume_events(), name="ws-consume-q-in")
        addrs = ", ".join(f"{s.getsockname()[0]}:{s.getsockname()[1]}" for s in server.sockets)
        log.info("WebSocket server listening on %s%s", addrs, self.path)
        return server

    async def _consume_events(self) -> None:
        while True:
            event = await self.q_in.get()
            try:
                self.apply_event(event)
            except Exception:  # one bad event must never stop the dashboard
                log.exception("could not apply event %r", event)

    # ---- display state -------------------------------------------------------

    def snapshot(self) -> dict[str, Any]:
        return {"type": "snapshot",
                "nodes": sorted(self.nodes.values(), key=lambda n: n["node_id"]),
                "alerts": list(self.alerts),
                "open_sos": sorted(self.open_sos.values(), key=lambda a: a["ts"])}

    def apply_event(self, event: Any) -> None:
        if isinstance(event, tuple) and event[0] == messages.AUDIO:
            _, node_id, pcm = event
            frame = node_id.to_bytes(2, "big") + pcm
            for client in list(self._clients):
                if node_id in client.listening:
                    self._push(client, frame)
            return

        kind = event.get("type")
        node_id = event.get("node_id")
        ts = event.get("ts") or time.time()

        if kind in ("node_joined", "node_address_changed"):
            node = self._node(node_id, ts)
            node["addr"] = event.get("addr")
            self._mark_active(node, ts)
        elif kind == "heartbeat":
            node = self._node(node_id, ts)
            for field in ("link", "role", "level", "parent_mac", "mac"):
                if event.get(field) is not None:
                    node[field] = event[field]
            if node["mac"]:
                self._by_mac[node["mac"]] = node_id
            self._resolve_tree(node)
            self._record_path(node_id, ts)
            self._mark_active(node, ts)
        elif kind == "node_dropped":
            node = self.nodes.get(node_id)
            if node and node["status"] != "dropped":
                node["status"] = "dropped"
                self._broadcast({"type": "node_update", "node": node})
                if node["role"] == "relay":
                    self._relay_dropped(node_id, ts)
                else:
                    self._alert("node_dropped", node_id, ts, f"{self.label(node_id)} stopped responding")
        elif kind == "sos":
            self._node(node_id, ts)
            open_alert = self.open_sos.get(node_id)
            if open_alert:  # still waiting for someone to acknowledge: don't pile up alerts
                open_alert["count"] += 1
                open_alert["last_ts"] = ts
                self._broadcast({"type": "sos_update", "alert": open_alert})
            else:
                alert = self._alert("sos", node_id, ts, f"SOS from {self.label(node_id)}",
                                    acked=False, count=1, last_ts=ts)
                self.open_sos[node_id] = alert
        else:
            log.debug("ignoring unknown event %r", event)

    def _node(self, node_id: int, ts: float) -> dict[str, Any]:
        node = self.nodes.get(node_id)
        if node is None:
            node = {"node_id": node_id, "name": self.names.get(node_id), "status": "new",
                    "addr": None, "last_seen": ts, "link": None, "role": None, "level": None,
                    "mac": None, "parent_mac": None, "parent": None, "near": None}
            self.nodes[node_id] = node
        return node

    def _mark_active(self, node: dict[str, Any], ts: float) -> None:
        if node["status"] == "dropped":
            self._alert("node_rejoined", node["node_id"], ts, f"{self.label(node['node_id'])} is back")
        node["status"] = "active"
        node["last_seen"] = ts
        self._broadcast({"type": "node_update", "node": node})

    def label(self, node_id: int) -> str:
        """How a node is named in alerts: "Alex (node 3)", "Tunnel B (relay 2)", or "Node 3"."""
        node = self.nodes.get(node_id)
        kind = "relay" if node and node["role"] == "relay" else "node"
        name = self.names.get(node_id)
        return f"{name} ({kind} {node_id})" if name else f"{kind.capitalize()} {node_id}"

    # ---- mesh tree -----------------------------------------------------------

    def _resolve_tree(self, node: dict[str, Any]) -> None:
        """Fill in "parent" and "near" from the MACs. Runs on every heartbeat, so the
        tree heals by itself within a second when a parent shows up late or a node moves."""
        if node["level"] == 1:
            node["parent"] = GATEWAY
        elif node["parent_mac"]:
            node["parent"] = self._by_mac.get(node["parent_mac"])
        node["near"] = None
        if node["role"] != "relay":
            for ancestor in self.ancestors(node["node_id"]):
                if self.nodes[ancestor]["role"] == "relay":
                    node["near"] = ancestor
                    break

    def ancestors(self, node_id: int) -> list[int]:
        """Node ids from this node's parent up to the root (excluding the Pi)."""
        chain: list[int] = []
        parent = self.nodes[node_id]["parent"] if node_id in self.nodes else None
        while parent and parent in self.nodes and parent != node_id and parent not in chain:
            chain.append(parent)
            parent = self.nodes[parent]["parent"]
        return chain

    def _record_path(self, node_id: int, ts: float) -> None:
        path = tuple(self.ancestors(node_id))
        history = self._paths.setdefault(node_id, [])
        if not history or history[-1][1] != path:
            history.append((ts, path))
            del history[:-4]  # a few recent changes are enough

    def _path_at(self, node_id: int, ts: float) -> tuple[int, ...]:
        """The node's path to the Pi as it was at time ts."""
        path: tuple[int, ...] = ()
        for since, p in self._paths.get(node_id, []):
            if since <= ts:
                path = p
        return path

    def _relay_dropped(self, relay_id: int, ts: float) -> None:
        """A relay going down cuts off everyone behind it until the mesh reroutes, so
        the alert says who was connected through it when it was last heard."""
        # Just after its last heartbeat: covers people whose own heartbeat came a moment later,
        # but is earlier than the mesh can reroute them (several seconds).
        last_heard = self.nodes[relay_id]["last_seen"] + 1.0
        behind = [n["node_id"] for n in sorted(self.nodes.values(), key=lambda n: n["node_id"])
                  if n["role"] != "relay" and relay_id in self._path_at(n["node_id"], last_heard)]
        if behind:
            who = ", ".join(self.label(i) for i in behind)
            message = f"{self.label(relay_id)} stopped responding. Connected through it: {who}"
        else:
            message = f"{self.label(relay_id)} stopped responding (nobody was connected through it)"
        self._alert("relay_dropped", relay_id, ts, message, behind=behind)

    # ---- names ---------------------------------------------------------------

    async def rename(self, node_id: int, name: Any) -> None:
        name = clean_name(name)
        if self.names.get(node_id, "") == name:
            return
        if name:
            self.names[node_id] = name
        else:
            self.names.pop(node_id, None)
        log.info("node %d renamed to %r", node_id, name or None)
        node = self.nodes.get(node_id)
        if node is not None:
            node["name"] = name or None
            self._broadcast({"type": "node_update", "node": node})
        await asyncio.to_thread(self._save_names, dict(self.names))  # file I/O off the event loop

    def _load_names(self) -> dict[int, str]:
        if not self.names_file or not self.names_file.exists():
            return {}
        try:
            raw = json.loads(self.names_file.read_text(encoding="utf-8"))
            return {int(k): clean_name(v) for k, v in raw.items() if clean_name(v)}
        except (OSError, ValueError, AttributeError) as exc:
            log.warning("could not read node names from %s: %s", self.names_file, exc)
            return {}

    def _save_names(self, names: dict[int, str]) -> None:
        if not self.names_file:
            return
        try:
            self.names_file.parent.mkdir(parents=True, exist_ok=True)
            tmp = self.names_file.with_suffix(".tmp")
            tmp.write_text(json.dumps({str(k): v for k, v in sorted(names.items())}, indent=2),
                           encoding="utf-8")
            os.replace(tmp, self.names_file)  # atomic: never leaves a half-written file
        except OSError as exc:
            log.warning("could not save node names to %s: %s", self.names_file, exc)

    def _alert(self, kind: str, node_id: int, ts: float, message: str, **extra: Any) -> dict[str, Any]:
        alert = {"id": self._next_alert_id, "kind": kind, "node_id": node_id, "ts": ts,
                 "message": message, **extra}
        self._next_alert_id += 1
        self.alerts.append(alert)
        log.info(message)
        self._broadcast({"type": "alert", "alert": alert})
        return alert

    def acknowledge(self, alert_id: int) -> None:
        """Close an open SOS for every dashboard; the alert itself records when."""
        for node_id, alert in list(self.open_sos.items()):
            if alert["id"] == alert_id:
                del self.open_sos[node_id]
                alert["acked"] = True
                alert["acked_ts"] = time.time()
                log.info("SOS from %s acknowledged", self.label(node_id))
                self._broadcast({"type": "sos_update", "alert": alert})
                return

    # ---- browsers ------------------------------------------------------------

    async def _handle_connection(self, ws: ServerConnection) -> None:
        if ws.request is None or ws.request.path != self.path:
            await ws.close(1008, "unknown path")
            return
        client = _Client(ws, self.client_buffer)
        self._clients.add(client)
        sender = asyncio.create_task(self._send_loop(client))
        log.info("dashboard connected from %s (%d open)", ws.remote_address, len(self._clients))
        self._push(client, json.dumps(self.snapshot()))
        try:
            async for message in ws:
                await self._handle_command(client, message)
        except ConnectionClosed:
            pass
        finally:
            self._clients.discard(client)
            sender.cancel()
            for node_id in list(client.listening):
                await self._stop_listening(client, node_id)
            log.info("dashboard disconnected (%d open)", len(self._clients))

    async def _send_loop(self, client: _Client) -> None:
        try:
            while True:
                await client.ws.send(await client.outbox.get())
        except ConnectionClosed:
            pass

    def _push(self, client: _Client, data: str | bytes) -> None:
        """Queue data for one browser without waiting; drop a browser that falls too far behind."""
        try:
            client.outbox.put_nowait(data)
        except asyncio.QueueFull:
            log.warning("dashboard %s too slow, disconnecting", client.ws.remote_address)
            self._clients.discard(client)
            asyncio.ensure_future(client.ws.close(1013, "too slow"))

    def _broadcast(self, obj: dict[str, Any]) -> None:
        data = json.dumps(obj)
        for client in list(self._clients):
            self._push(client, data)

    async def _handle_command(self, client: _Client, message: str | bytes) -> None:
        if isinstance(message, bytes):
            return
        try:
            cmd = json.loads(message)
        except ValueError:
            return
        node_id = cmd.get("node_id")
        if cmd.get("type") == "listen" and isinstance(node_id, int):
            await self._start_listening(client, node_id)
        elif cmd.get("type") == "stop":
            targets = [node_id] if isinstance(node_id, int) else list(client.listening)
            for target in targets:
                await self._stop_listening(client, target)
        elif cmd.get("type") == "rename" and isinstance(node_id, int):
            await self.rename(node_id, cmd.get("name"))
        elif cmd.get("type") == "ack" and isinstance(cmd.get("alert_id"), int):
            self.acknowledge(cmd["alert_id"])

    async def _start_listening(self, client: _Client, node_id: int) -> None:
        if node_id in client.listening:
            return
        client.listening.add(node_id)
        self._listeners[node_id] += 1
        if self._listeners[node_id] == 1:  # first dashboard listening to this node
            await self.q_out.put(messages.listen(node_id))

    async def _stop_listening(self, client: _Client, node_id: int) -> None:
        if node_id not in client.listening:
            return
        client.listening.discard(node_id)
        self._listeners[node_id] -= 1
        if self._listeners[node_id] <= 0:  # last dashboard stopped listening
            del self._listeners[node_id]
            await self.q_out.put(messages.stop(node_id))
