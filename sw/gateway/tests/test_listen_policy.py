"""Who can be heard: talk button, open SOS, or a logged emergency listen."""

import asyncio
import json
import types

import messages
from ws_server import WebSocketServer, _Client


def setup(*node_ids):
    ws = WebSocketServer(asyncio.Queue(), asyncio.Queue(), talk_timeout=3.0, talk_grace=2.0)
    for node_id in node_ids:
        ws.apply_event(messages.node_joined(node_id, "x", ts=100.0))
    client = _Client(types.SimpleNamespace(remote_address=("192.168.4.23", 50000)), 100)
    ws._clients.add(client)
    return ws, client


def sent(client):
    """Text messages pushed to this dashboard so far."""
    out = []
    while not client.outbox.empty():
        msg = client.outbox.get_nowait()
        if isinstance(msg, str):
            out.append(json.loads(msg))
    return out


def commands(ws):
    out = []
    while not ws.q_out.empty():
        out.append(ws.q_out.get_nowait())
    return out


def test_listening_is_refused_when_the_worker_is_not_talking_and_no_sos():
    ws, client = setup(3)
    ws._listen(client, 3, emergency=False)
    assert 3 not in client.listening
    assert commands(ws) == []  # the UDP side is never asked for audio
    denied = [m for m in sent(client) if m["type"] == "listen_denied"]
    assert denied and "Emergency listen" in denied[0]["message"]


def test_talk_button_opens_listening():
    ws, client = setup(3)
    ws.apply_event(messages.talk(3, True, ts=100.0))
    ws._listen(client, 3, emergency=False)
    assert client.listening == {3: "talk"}
    assert commands(ws) == [messages.listen(3)]


def test_open_sos_opens_listening():
    ws, client = setup(4)
    ws.apply_event(messages.sos(4, ts=100.0))
    ws._listen(client, 4, emergency=False)
    assert client.listening == {4: "sos"}


def test_releasing_talk_ends_the_listen_after_the_grace_period():
    ws, client = setup(3)
    ws.apply_event(messages.talk(3, True, ts=100.0))
    ws._listen(client, 3, emergency=False)
    commands(ws)
    ws.apply_event(messages.talk(3, False, ts=101.0))
    ws.check_talk(102.0)  # within the 2 s grace: still listening
    assert 3 in client.listening
    ws.check_talk(103.5)
    assert 3 not in client.listening
    assert commands(ws) == [messages.stop(3)]
    assert any(m["type"] == "listen_ended" for m in sent(client))


def test_pressing_again_within_the_grace_keeps_the_listen():
    ws, client = setup(3)
    ws.apply_event(messages.talk(3, True, ts=100.0))
    ws._listen(client, 3, emergency=False)
    ws.apply_event(messages.talk(3, False, ts=101.0))
    ws.apply_event(messages.talk(3, True, ts=102.0))
    ws.check_talk(104.0)
    assert client.listening == {3: "talk"}


def test_talk_button_that_goes_quiet_counts_as_released():
    ws, client = setup(3)
    ws.apply_event(messages.talk(3, True, ts=100.0))  # then no repeats: packet lost or device died
    ws.check_talk(103.5)
    assert ws.nodes[3]["talking"] is False
    ws.check_talk(106.0)
    assert ws.grant(3) is None


def test_listen_started_during_sos_continues_after_acknowledgement():
    ws, client = setup(4)
    ws.apply_event(messages.sos(4, ts=100.0))
    ws._listen(client, 4, emergency=False)
    ws.acknowledge(ws.open_sos[4]["id"])
    ws.check_talk(200.0)
    assert client.listening == {4: "sos"}
    # but a new listen after the SOS is closed is refused
    other = _Client(types.SimpleNamespace(remote_address=("192.168.4.24", 1)), 100)
    ws._clients.add(other)
    ws._listen(other, 4, emergency=False)
    assert 4 not in other.listening


def test_talk_listen_that_overlaps_an_sos_survives_the_release():
    ws, client = setup(3)
    ws.apply_event(messages.talk(3, True, ts=100.0))
    ws._listen(client, 3, emergency=False)
    ws.apply_event(messages.sos(3, ts=100.5))
    ws.apply_event(messages.talk(3, False, ts=101.0))
    ws.check_talk(104.0)
    assert client.listening == {3: "sos"}


def test_emergency_listen_is_allowed_logged_and_shown_on_the_device():
    ws, client = setup(5)
    ws.names[5] = "Alex"
    ws._listen(client, 5, emergency=True)
    assert client.listening == {5: "emergency"}
    assert commands(ws) == [messages.notify(5, "emergency_listen_on"), messages.listen(5)]
    logged = ws.events.query(kinds=["emergency_listen"]).result(timeout=2)
    assert logged[0]["message"] == "Emergency listen on Alex (node 5) from dashboard 192.168.4.23"

    ws._stop_listening(client, 5)
    assert commands(ws) == [messages.stop(5), messages.notify(5, "emergency_listen_off")]
    ended = ws.events.query(kinds=["emergency_listen_ended"]).result(timeout=2)
    assert ended and ended[0]["dashboard"] == "192.168.4.23"


def test_device_light_stays_on_until_the_last_emergency_listener_stops():
    ws, a = setup(5)
    b = _Client(types.SimpleNamespace(remote_address=("192.168.4.30", 1)), 100)
    ws._clients.add(b)
    ws._listen(a, 5, emergency=True)
    ws._listen(b, 5, emergency=True)
    commands(ws)
    ws._stop_listening(a, 5)
    assert messages.notify(5, "emergency_listen_off") not in commands(ws)
    ws._stop_listening(b, 5)
    assert messages.notify(5, "emergency_listen_off") in commands(ws)


def test_emergency_listen_on_an_unknown_node_is_refused():
    ws, client = setup()
    ws._listen(client, 99, emergency=True)
    assert client.listening == {}
