import asyncio
import json

from websockets.asyncio.client import connect

import messages
from ws_server import WebSocketServer, clean_name


async def start(names_file):
    q_in: asyncio.Queue = asyncio.Queue(maxsize=200)
    q_out: asyncio.Queue = asyncio.Queue(maxsize=200)
    ws = WebSocketServer(q_in, q_out, names_file=names_file)
    server = await ws.start("127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    return ws, server, q_in, f"ws://127.0.0.1:{port}/ws"


async def stop(ws, server):
    ws._consumer.cancel()
    ws._talk_task.cancel()
    server.close()
    await server.wait_closed()


async def recv_json(client):
    """Next message, skipping event-log entries (tested in test_event_log.py)."""
    while True:
        msg = json.loads(await asyncio.wait_for(client.recv(), timeout=2))
        if msg.get("type") != "log_event":
            return msg


def test_clean_name():
    assert clean_name("  Alex   Smith ") == "Alex Smith"
    assert clean_name("Al\nex\x00") == "Alex"
    assert clean_name("x" * 100) == "x" * 32
    assert clean_name("   ") == ""
    assert clean_name(None) == ""
    assert clean_name(42) == ""


async def test_rename_reaches_every_dashboard_and_is_saved(tmp_path):
    names_file = tmp_path / "names.json"
    ws, server, q_in, url = await start(names_file)
    try:
        async with connect(url) as a, connect(url) as b:
            await recv_json(a)
            await recv_json(b)
            await q_in.put(messages.heartbeat(3))
            await recv_json(a)
            await recv_json(b)
            await a.send(json.dumps({"type": "rename", "node_id": 3, "name": " Alex "}))
            update_a, update_b = await recv_json(a), await recv_json(b)
        assert update_a["node"]["name"] == update_b["node"]["name"] == "Alex"
        await asyncio.sleep(0.1)
        assert json.loads(names_file.read_text()) == {"3": "Alex"}
    finally:
        await stop(ws, server)


async def test_names_survive_a_restart_and_appear_in_alerts(tmp_path):
    names_file = tmp_path / "names.json"
    names_file.write_text(json.dumps({"5": "Sam"}))
    ws, server, q_in, url = await start(names_file)
    try:
        async with connect(url) as client:
            await recv_json(client)
            await q_in.put(messages.heartbeat(5))
            joined = await recv_json(client)
            await q_in.put(messages.sos(5))
            alert = await recv_json(client)
        assert joined["node"]["name"] == "Sam"
        assert alert["alert"]["message"] == "SOS from Sam (node 5)"
    finally:
        await stop(ws, server)


async def test_empty_name_clears_it(tmp_path):
    names_file = tmp_path / "names.json"
    names_file.write_text(json.dumps({"2": "Jo"}))
    ws, server, q_in, url = await start(names_file)
    try:
        async with connect(url) as client:
            await recv_json(client)
            await q_in.put(messages.heartbeat(2))
            await recv_json(client)
            await client.send(json.dumps({"type": "rename", "node_id": 2, "name": ""}))
            update = await recv_json(client)
        assert update["node"]["name"] is None
        await asyncio.sleep(0.1)
        assert json.loads(names_file.read_text()) == {}
        assert ws.label(2) == "Node 2"
    finally:
        await stop(ws, server)


async def test_unreadable_names_file_is_ignored(tmp_path):
    names_file = tmp_path / "names.json"
    names_file.write_text("not json")
    ws, server, _, _ = await start(names_file)
    try:
        assert ws.names == {}
    finally:
        await stop(ws, server)
