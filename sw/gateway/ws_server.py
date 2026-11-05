"""WebSocket server: the dashboard half of the gateway process (task 168).

Reads events from Q in, keeps the display state (nodes, alerts), and pushes it
to every connected browser. Commands from browsers ("listen" / "stop") go to
the UDP side through Q out.

Browser protocol (all text frames are JSON):
    server -> browser   {"type": "snapshot", "nodes": [...], "alerts": [...]}   once, on connect
                        {"type": "node_update", "node": {...}}
                        {"type": "alert", "alert": {...}}
                        binary: 2-byte node_id (big-endian) + 16-bit PCM, for nodes being listened to
    browser -> server   {"type": "listen", "node_id": 3}
                        {"type": "stop", "node_id": 3}   (or without node_id: stop everything)
"""

from __future__ import annotations

import asyncio
import collections
import json
import logging
import time
from typing import Any

from websockets.asyncio.server import Server, ServerConnection, serve
from websockets.exceptions import ConnectionClosed

import messages

log = logging.getLogger("ws")


class _Client:
    def __init__(self, ws: ServerConnection, buffer: int) -> None:
        self.ws = ws
        self.outbox: asyncio.Queue[str | bytes] = asyncio.Queue(maxsize=buffer)
        self.listening: set[int] = set()


class WebSocketServer:
    def __init__(self, q_in: asyncio.Queue, q_out: asyncio.Queue, *, path: str = "/ws",
                 client_buffer: int = 128, alert_history: int = 50) -> None:
        self.q_in = q_in
        self.q_out = q_out
        self.path = path
        self.client_buffer = client_buffer
        self.nodes: dict[int, dict[str, Any]] = {}
        self.alerts: collections.deque[dict[str, Any]] = collections.deque(maxlen=alert_history)
        self._clients: set[_Client] = set()
        self._listeners: collections.Counter[int] = collections.Counter()

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
                "alerts": list(self.alerts)}

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
            if event.get("link") is not None:
                node["link"] = event["link"]
            if event.get("pos") is not None:
                node["pos"] = event["pos"]
            self._mark_active(node, ts)
        elif kind == "node_dropped":
            node = self.nodes.get(node_id)
            if node and node["status"] != "dropped":
                node["status"] = "dropped"
                self._broadcast({"type": "node_update", "node": node})
                self._alert("node_dropped", node_id, ts, f"Node {node_id} stopped responding")
        elif kind == "sos":
            self._node(node_id, ts)
            self._alert("sos", node_id, ts, f"SOS from node {node_id}")
        else:
            log.debug("ignoring unknown event %r", event)

    def _node(self, node_id: int, ts: float) -> dict[str, Any]:
        node = self.nodes.get(node_id)
        if node is None:
            node = {"node_id": node_id, "status": "new", "addr": None,
                    "last_seen": ts, "link": None, "pos": None}
            self.nodes[node_id] = node
        return node

    def _mark_active(self, node: dict[str, Any], ts: float) -> None:
        if node["status"] == "dropped":
            self._alert("node_rejoined", node["node_id"], ts, f"Node {node['node_id']} is back")
        node["status"] = "active"
        node["last_seen"] = ts
        self._broadcast({"type": "node_update", "node": node})

    def _alert(self, kind: str, node_id: int, ts: float, message: str) -> None:
        alert = {"kind": kind, "node_id": node_id, "ts": ts, "message": message}
        self.alerts.append(alert)
        log.info(message)
        self._broadcast({"type": "alert", "alert": alert})

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
