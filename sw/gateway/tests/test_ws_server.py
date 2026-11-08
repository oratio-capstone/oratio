import asyncio
import json

import pytest
from websockets.asyncio.client import connect

import messages
from ws_server import WebSocketServer


@pytest.fixture
async def gateway():
    q_in: asyncio.Queue = asyncio.Queue(maxsize=200)
    q_out: asyncio.Queue = asyncio.Queue(maxsize=200)
    ws = WebSocketServer(q_in, q_out)
    server = await ws.start("127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    yield ws, q_in, q_out, f"ws://127.0.0.1:{port}/ws"
    ws._consumer.cancel()
    server.close()
    await server.wait_closed()


async def recv_json(client):
    return json.loads(await asyncio.wait_for(client.recv(), timeout=2))


@pytest.mark.asyncio
async def test_new_dashboard_gets_snapshot_with_existing_nodes(gateway):
    ws, q_in, _, url = gateway
    await q_in.put(messages.node_joined(1, "192.168.4.10:4000"))
    await asyncio.sleep(0.05)
    async with connect(url) as client:
        snapshot = await recv_json(client)
    assert snapshot["type"] == "snapshot"
    assert [n["node_id"] for n in snapshot["nodes"]] == [1]
    assert snapshot["nodes"][0]["status"] == "active"


@pytest.mark.asyncio
async def test_heartbeat_is_pushed_live(gateway):
    _, q_in, _, url = gateway
    async with connect(url) as client:
        await recv_json(client)  # snapshot
        await q_in.put(messages.heartbeat(2, link=-60, role="worker", level=1,
                                          parent_mac="02:00:00:00:ff:ff", mac="02:00:00:00:00:02"))
        update = await recv_json(client)
    assert update["type"] == "node_update"
    assert update["node"]["node_id"] == 2
    assert update["node"]["link"] == -60
    assert update["node"]["role"] == "worker" and update["node"]["parent"] == 0


@pytest.mark.asyncio
async def test_drop_and_sos_raise_alerts(gateway):
    ws, q_in, _, url = gateway
    async with connect(url) as client:
        await recv_json(client)
        await q_in.put(messages.heartbeat(3))
        await recv_json(client)
        await q_in.put(messages.node_dropped(3))
        dropped = await recv_json(client)
        alert = await recv_json(client)
        await q_in.put(messages.sos(4))
        sos = await recv_json(client)
    assert dropped["node"]["status"] == "dropped"
    assert alert["alert"]["kind"] == "node_dropped"
    assert sos["alert"]["kind"] == "sos"
    assert [a["kind"] for a in ws.alerts] == ["node_dropped", "sos"]


@pytest.mark.asyncio
async def test_listen_reaches_udp_side_and_audio_comes_back(gateway):
    _, q_in, q_out, url = gateway
    async with connect(url) as client:
        await recv_json(client)
        await client.send(json.dumps({"type": "listen", "node_id": 5}))
        assert await asyncio.wait_for(q_out.get(), 2) == messages.listen(5)

        pcm = bytes(640)
        await q_in.put(messages.audio(5, pcm))
        frame = await asyncio.wait_for(client.recv(), 2)
        assert frame[:2] == (5).to_bytes(2, "big") and frame[2:] == pcm

        await client.send(json.dumps({"type": "stop", "node_id": 5}))
        assert await asyncio.wait_for(q_out.get(), 2) == messages.stop(5)


@pytest.mark.asyncio
async def test_closing_the_tab_stops_listening(gateway):
    _, _, q_out, url = gateway
    async with connect(url) as client:
        await recv_json(client)
        await client.send(json.dumps({"type": "listen", "node_id": 6}))
        await asyncio.wait_for(q_out.get(), 2)
    assert await asyncio.wait_for(q_out.get(), 2) == messages.stop(6)


@pytest.mark.asyncio
async def test_two_dashboards_share_one_listen(gateway):
    _, _, q_out, url = gateway
    async with connect(url) as a, connect(url) as b:
        await recv_json(a)
        await recv_json(b)
        await a.send(json.dumps({"type": "listen", "node_id": 7}))
        await b.send(json.dumps({"type": "listen", "node_id": 7}))
        assert await asyncio.wait_for(q_out.get(), 2) == messages.listen(7)
        await asyncio.sleep(0.1)
        assert q_out.empty()  # second listener doesn't re-send "listen"


@pytest.mark.asyncio
async def test_bad_event_does_not_stop_the_server(gateway):
    _, q_in, _, url = gateway
    await q_in.put({"type": "heartbeat"})  # missing node_id is tolerated
    await q_in.put("garbage")  # wrong type entirely
    await asyncio.sleep(0.05)
    async with connect(url) as client:
        await recv_json(client)
        await q_in.put(messages.heartbeat(8))
        assert (await recv_json(client))["node"]["node_id"] == 8


@pytest.mark.asyncio
async def test_unknown_path_is_rejected(gateway):
    *_, url = gateway
    async with connect(url.replace("/ws", "/other")) as client:
        with pytest.raises(Exception):
            await asyncio.wait_for(client.recv(), 2)
