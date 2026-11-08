import asyncio
import json
import time

from websockets.asyncio.client import connect

import messages
from event_log import EventLog
from ws_server import WebSocketServer


def result(future):
    return future.result(timeout=2)


def test_events_and_ids_survive_a_restart(tmp_path):
    db = tmp_path / "events.db"
    log = EventLog(db)
    first = log.add("sos", node_id=4, message="SOS from Node 4", acked=False, count=1)
    log.add("node_dropped", node_id=3, message="Node 3 stopped responding")
    log.close()

    log = EventLog(db)
    events = result(log.query())
    assert [e["kind"] for e in events] == ["node_dropped", "sos"]  # newest first
    assert events[1]["acked"] is False and events[1]["count"] == 1  # extra fields come back
    assert log.add("node_seen", node_id=1, message="x")["id"] == first["id"] + 2  # ids keep counting
    log.close()


def test_query_filters_and_paging():
    log = EventLog(None)
    for i in range(10):
        log.add("node_dropped" if i % 2 else "sos", node_id=i % 3, message=str(i), ts=1000.0 + i)
    assert {e["kind"] for e in result(log.query(kinds=["sos"]))} == {"sos"}
    assert {e["node_id"] for e in result(log.query(node_id=2))} == {2}
    assert [e["message"] for e in result(log.query(since=1007.0))] == ["9", "8", "7"]
    page1 = result(log.query(limit=4))
    page2 = result(log.query(limit=4, before_id=page1[-1]["id"]))
    assert [e["message"] for e in page1 + page2] == [str(i) for i in range(9, 1, -1)]


def test_update_changes_the_saved_event(tmp_path):
    log = EventLog(tmp_path / "e.db")
    event = log.add("sos", node_id=4, message="SOS", acked=False)
    log.update(event["id"], {"acked": True, "acked_ts": event["ts"] + 7})
    saved = result(log.query())[0]
    assert saved["acked"] is True and saved["acked_ts"] - saved["ts"] == 7


def test_stats_give_counts_and_acknowledgement_times():
    log = EventLog(None)
    for delay in (5, 10, 30):
        e = log.add("sos", node_id=1, message="SOS", ts=1000.0, acked=False)
        log.update(e["id"], {"acked": True, "acked_ts": 1000.0 + delay})
    log.add("sos", node_id=2, message="SOS", ts=1000.0, acked=False)  # never acknowledged
    log.add("relay_dropped", node_id=101, message="Relay down", ts=1000.0)
    stats = result(log.stats())
    assert stats["sos"] == 4 and stats["sos_open"] == 1
    assert stats["counts"]["relay_dropped"] == 1
    assert stats["ack_avg_s"] == 15 and stats["ack_median_s"] == 10 and stats["ack_max_s"] == 30


def test_old_events_are_removed_at_startup(tmp_path):
    log = EventLog(tmp_path / "e.db")
    log.add("sos", node_id=1, message="old", ts=time.time() - 100 * 86400)
    log.add("sos", node_id=1, message="new")
    log.close()
    log = EventLog(tmp_path / "e.db", retention_days=90)
    assert [e["message"] for e in result(log.query())] == ["new"]


def test_unwritable_folder_falls_back_to_memory(tmp_path):
    blocker = tmp_path / "file"
    blocker.write_text("not a folder")
    log = EventLog(blocker / "sub" / "events.db")
    log.add("sos", node_id=1, message="still works")
    assert len(result(log.query())) == 1


def test_gateway_restart_keeps_alerts_and_open_sos(tmp_path):
    db = tmp_path / "events.db"
    ws = WebSocketServer(asyncio.Queue(), asyncio.Queue(), event_log=EventLog(db))
    ws.apply_event(messages.sos(4))
    ws.apply_event(messages.sos(5))
    ws.acknowledge(ws.open_sos[5]["id"])
    ws.apply_event(messages.node_joined(3, "x"))
    ws.apply_event(messages.node_dropped(3))
    ws.events.close()

    ws = WebSocketServer(asyncio.Queue(), asyncio.Queue(), event_log=EventLog(db))
    assert [a["kind"] for a in ws.alerts] == ["sos", "sos", "node_dropped"]
    assert list(ws.open_sos) == [4]  # 5 was acknowledged before the restart
    # Acknowledging after the restart still works and is saved.
    ws.acknowledge(ws.open_sos[4]["id"])
    sos = result(ws.events.query(kinds=["sos"], node_id=4))[0]
    assert sos["acked"] is True


async def test_dashboard_can_query_the_log(tmp_path):
    q_in: asyncio.Queue = asyncio.Queue()
    ws = WebSocketServer(q_in, asyncio.Queue(), event_log=EventLog(tmp_path / "e.db"))
    server = await ws.start("127.0.0.1", 0)
    url = f"ws://127.0.0.1:{server.sockets[0].getsockname()[1]}/ws"
    try:
        for node in (1, 2):
            await q_in.put(messages.node_joined(node, "x"))
            await q_in.put(messages.heartbeat(node))
        await q_in.put(messages.sos(2))
        await asyncio.sleep(0.1)
        async with connect(url) as client:
            await client.send(json.dumps({"type": "log_query", "req": 7, "node_id": 2, "limit": 1}))
            await client.send(json.dumps({"type": "log_stats", "req": 8}))
            await client.send(json.dumps({"type": "log_query", "req": 9, "kinds": "oops", "limit": "x"}))
            replies = {}
            while len(replies) < 3:
                msg = json.loads(await asyncio.wait_for(client.recv(), 2))
                if msg.get("req"):
                    replies[msg["req"]] = msg
        assert replies[7]["type"] == "log_result"
        assert [e["kind"] for e in replies[7]["events"]] == ["sos"] and replies[7]["more"] is True
        assert replies[8]["stats"]["counts"]["node_seen"] == 2
        assert replies[8]["stats"]["counts"]["gateway_started"] == 1
        assert replies[9]["type"] == "log_result"  # bad fields are ignored, not fatal
    finally:
        ws._consumer.cancel()
        server.close()
        await server.wait_closed()


def test_relay_is_logged_as_a_relay_when_it_connects():
    ws = WebSocketServer(asyncio.Queue(), asyncio.Queue())
    ws.apply_event(messages.node_joined(101, "x"))  # first packet: role not known yet
    ws.apply_event(messages.heartbeat(101, -60, role="relay", level=1, parent_mac="02:00:00:ff:ff:ff",
                                      mac="02:00:00:00:00:65"))
    ws.apply_event(messages.heartbeat(101))
    seen = [e for e in result(ws.events.query(kinds=["node_seen"]))]
    assert [e["message"] for e in seen] == ["Relay 101 connected"]


def test_median_averages_the_middle_two_when_the_count_is_even():
    log = EventLog(None)
    for delay in (5, 10, 20, 30):
        e = log.add("sos", node_id=1, message="SOS", ts=1000.0, acked=False)
        log.update(e["id"], {"acked": True, "acked_ts": 1000.0 + delay})
    stats = result(log.stats())
    assert stats["ack_median_s"] == 15  # (10 + 20) / 2
    assert stats["ack_avg_s"] == 16.25 and stats["ack_max_s"] == 30
