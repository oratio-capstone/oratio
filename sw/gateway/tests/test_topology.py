import asyncio

import messages
from ws_server import GATEWAY, WebSocketServer

PI = "02:00:00:00:ff:ff"


def mac(node_id: int) -> str:
    return f"02:00:00:00:00:{node_id:02x}"


def hb(ws, node_id, role, level, parent, link=-60):
    parent_mac = PI if parent == GATEWAY else mac(parent)
    ws.apply_event(messages.heartbeat(node_id, link, role=role, level=level,
                                      parent_mac=parent_mac, mac=mac(node_id)))


def tunnel():
    """Pi <- relay 1 <- relay 2, with worker 10 on relay 1 and workers 11, 12 on relay 2."""
    ws = WebSocketServer(asyncio.Queue(), asyncio.Queue())
    hb(ws, 1, "relay", 1, GATEWAY)
    hb(ws, 2, "relay", 2, 1)
    hb(ws, 10, "worker", 2, 1)
    hb(ws, 11, "worker", 3, 2)
    hb(ws, 12, "worker", 3, 2)
    return ws


def test_parents_are_resolved_from_macs():
    ws = tunnel()
    assert {i: ws.nodes[i]["parent"] for i in ws.nodes} == {1: GATEWAY, 2: 1, 10: 1, 11: 2, 12: 2}
    assert ws.ancestors(11) == [2, 1]


def test_worker_is_near_its_first_relay():
    ws = tunnel()
    assert ws.nodes[10]["near"] == 1
    assert ws.nodes[11]["near"] == 2
    assert ws.nodes[1]["near"] is None  # relays aren't "near" anything


def test_worker_behind_another_worker_is_near_that_workers_relay():
    ws = tunnel()
    hb(ws, 13, "worker", 4, 11)  # joined through worker 11
    assert ws.nodes[13]["parent"] == 11 and ws.nodes[13]["near"] == 2


def test_child_heard_before_its_parent_resolves_on_next_heartbeat():
    ws = WebSocketServer(asyncio.Queue(), asyncio.Queue())
    hb(ws, 10, "worker", 2, 1)
    assert ws.nodes[10]["parent"] is None  # relay 1 not heard yet
    hb(ws, 1, "relay", 1, GATEWAY)
    hb(ws, 10, "worker", 2, 1)
    assert ws.nodes[10]["parent"] == 1


def test_moving_worker_changes_near():
    ws = tunnel()
    hb(ws, 10, "worker", 3, 2)  # walked down the tunnel to relay 2
    assert ws.nodes[10]["near"] == 2


def test_relay_drop_names_everyone_behind_it():
    ws = tunnel()
    ws.names[11] = "Alex"
    ws.apply_event(messages.node_dropped(1))
    alert = ws.alerts[-1]
    assert alert["kind"] == "relay_dropped"
    assert alert["behind"] == [10, 11, 12]
    assert alert["message"].startswith("Relay 1 stopped responding")
    assert "Alex (node 11)" in alert["message"]


def test_dropping_a_worker_is_an_ordinary_alert():
    ws = tunnel()
    ws.apply_event(messages.node_dropped(12))
    assert ws.alerts[-1]["kind"] == "node_dropped"


def test_cycle_in_parents_does_not_hang():
    ws = WebSocketServer(asyncio.Queue(), asyncio.Queue())
    hb(ws, 1, "relay", 2, 2)
    hb(ws, 2, "relay", 2, 1)
    hb(ws, 1, "relay", 2, 2)
    assert ws.ancestors(1) == [2]


def test_relay_drop_reports_people_who_were_behind_it_even_after_reroute():
    ws = WebSocketServer(asyncio.Queue(), asyncio.Queue())
    for t in (100.0, 101.0):
        ws.apply_event(messages.heartbeat(1, -60, role="relay", level=1, parent_mac=PI, mac=mac(1), ts=t))
        ws.apply_event(messages.heartbeat(2, -60, role="relay", level=2, parent_mac=mac(1), mac=mac(2), ts=t))
        ws.apply_event(messages.heartbeat(11, -60, role="worker", level=3, parent_mac=mac(2), mac=mac(11), ts=t))
    # Relay 2 goes quiet after 101 s; by 103 s worker 11 has rerouted through relay 1.
    ws.apply_event(messages.heartbeat(11, -70, role="worker", level=2, parent_mac=mac(1), mac=mac(11), ts=103.0))
    assert ws.nodes[11]["near"] == 1
    ws.apply_event(messages.node_dropped(2, ts=104.0))
    assert ws.alerts[-1]["kind"] == "relay_dropped" and ws.alerts[-1]["behind"] == [11]
