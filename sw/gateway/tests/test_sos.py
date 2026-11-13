import asyncio
import json

from websockets.asyncio.client import connect

import messages
from ws_server import WebSocketServer


async def recv_json(client):
    return json.loads(await asyncio.wait_for(client.recv(), timeout=2))


async def recv_type(client, kind):
    while True:
        msg = await recv_json(client)
        if msg["type"] == kind:
            return msg


def make_server():
    return WebSocketServer(asyncio.Queue(), asyncio.Queue())


def test_sos_opens_once_and_repeats_bump_the_count():
    ws = make_server()
    ws.apply_event(messages.sos(3, ts=100.0))
    ws.apply_event(messages.sos(3, ts=101.0))
    ws.apply_event(messages.sos(3, ts=102.0))
    assert [a["kind"] for a in ws.alerts] == ["sos"]  # one history entry, not three
    alert = ws.open_sos[3]
    assert alert["count"] == 3 and alert["last_ts"] == 102.0 and alert["acked"] is False


def test_acknowledge_closes_the_sos_in_place():
    ws = make_server()
    ws.apply_event(messages.sos(3))
    alert_id = ws.open_sos[3]["id"]
    ws.acknowledge(alert_id)
    assert ws.open_sos == {}
    assert ws.alerts[0]["acked"] is True and ws.alerts[0]["acked_ts"] >= ws.alerts[0]["ts"]
    assert len(ws.alerts) == 1  # no second "acknowledged" entry
    # A new SOS after the acknowledgement is a fresh alert.
    ws.apply_event(messages.sos(3))
    assert ws.open_sos[3]["id"] != alert_id


def test_unknown_alert_id_is_ignored():
    ws = make_server()
    ws.apply_event(messages.sos(3))
    ws.acknowledge(9999)
    assert 3 in ws.open_sos


def test_open_sos_survives_a_full_alert_history():
    ws = WebSocketServer(asyncio.Queue(), asyncio.Queue(), alert_history=5)
    ws.apply_event(messages.sos(3))
    for i in range(20):  # flood the history with other alerts
        ws.apply_event(messages.node_joined(10 + i, "x"))
        ws.apply_event(messages.node_dropped(10 + i))
    assert all(a["kind"] != "sos" for a in ws.alerts)
    assert ws.snapshot()["open_sos"][0]["node_id"] == 3


async def test_ack_from_one_dashboard_clears_it_on_the_other():
    q_in: asyncio.Queue = asyncio.Queue()
    ws = WebSocketServer(q_in, asyncio.Queue())
    server = await ws.start("127.0.0.1", 0)
    url = f"ws://127.0.0.1:{server.sockets[0].getsockname()[1]}/ws"
    try:
        async with connect(url) as a, connect(url) as b:
            await recv_type(a, "snapshot")
            await recv_type(b, "snapshot")
            await q_in.put(messages.sos(4))
            alert = (await recv_type(b, "alert"))["alert"]
            assert alert["kind"] == "sos"
            await a.send(json.dumps({"type": "ack", "alert_id": alert["id"]}))
            update = await recv_type(b, "sos_update")
            assert update["alert"]["id"] == alert["id"] and update["alert"]["acked"] is True
        async with connect(url) as c:  # a dashboard opened later sees no open SOS
            assert (await recv_type(c, "snapshot"))["open_sos"] == []
    finally:
        ws._consumer.cancel()
        ws._talk_task.cancel()
        server.close()
        await server.wait_closed()
