"""The UDP side only routes; the WebSocket side decodes and keeps the timers."""

import asyncio

import messages
import protocol
import udp_devserver
from ws_server import WebSocketServer


class Device(asyncio.DatagramProtocol):
    def __init__(self):
        self.received = asyncio.Queue()

    def datagram_received(self, data, addr):
        self.received.put_nowait(data)


async def _device(port):
    loop = asyncio.get_running_loop()
    return await loop.create_datagram_endpoint(Device, remote_addr=("127.0.0.1", port))


async def test_audio_is_forwarded_to_the_other_nodes_and_only_listened_audio_goes_on_q_in():
    q_in, q_out = asyncio.Queue(), asyncio.Queue()
    server = await udp_devserver.start(q_in, q_out, "127.0.0.1", 0)
    (t1, d1), (t2, d2) = await _device(server.port), await _device(server.port)
    try:
        t1.sendto(protocol.build(protocol.TYPE_HEARTBEAT, 1))
        t2.sendto(protocol.build(protocol.TYPE_HEARTBEAT, 2))
        await asyncio.sleep(0.05)
        while not q_in.empty():
            q_in.get_nowait()

        audio = protocol.build(protocol.TYPE_AUDIO, 1, b"\x01\x02" * 80, seq=5)
        t1.sendto(audio)
        assert await asyncio.wait_for(d2.received.get(), 2) == audio  # node 2 hears node 1, untouched
        assert d1.received.empty()                                     # nobody hears themselves
        seen = q_in.get_nowait()                                       # only a "still alive" note,
        assert (seen["type"], seen["node_id"], seen["kind"], seen["payload"]) == ("packet", 1, "A", b"")
        assert q_in.empty()                                            # no audio: nobody is listening

        server.listening.add(1)
        t1.sendto(audio)
        await asyncio.wait_for(d2.received.get(), 2)
        assert await asyncio.wait_for(q_in.get(), 2) == messages.audio(1, b"\x01\x02" * 80)
        assert q_in.empty()                                            # the note was sent < 1 s ago
    finally:
        t1.close()
        t2.close()
        for task in server._tasks:
            task.cancel()


async def test_other_packets_are_passed_on_raw_and_malformed_ones_are_dropped():
    q_in, q_out = asyncio.Queue(), asyncio.Queue()
    server = await udp_devserver.start(q_in, q_out, "127.0.0.1", 0)
    transport, _ = await _device(server.port)
    try:
        transport.sendto(b"H\x00")                                        # shorter than the header
        transport.sendto(b"H\x00\x07\x00\x01\x00\x09abc")                 # length field says 9, has 3
        transport.sendto(protocol.build(protocol.TYPE_SOS, 7, seq=3))
        event = await asyncio.wait_for(q_in.get(), 2)
        assert (event["type"], event["node_id"], event["kind"], event["seq"], event["payload"]) == \
            ("packet", 7, "S", 3, b"")
        assert server.malformed == 2
        assert q_in.empty()
    finally:
        transport.close()
        for task in server._tasks:
            task.cancel()


def _ws(**kwargs):
    return WebSocketServer(asyncio.Queue(), asyncio.Queue(), **kwargs)


def test_websocket_side_decodes_heartbeats_and_sos():
    ws = _ws()
    payload = protocol.build_heartbeat("relay", 1, -55, "02:00:00:00:ff:ff", "02:00:00:00:00:07")
    ws.apply_event(messages.packet(7, "H", payload, "10.0.0.7:5000", ts=100.0))
    node = ws.nodes[7]
    assert (node["addr"], node["role"], node["level"], node["link"]) == ("10.0.0.7:5000", "relay", 1, -55)

    ws.apply_event(messages.packet(7, "S", b"", "10.0.0.8:5000", ts=101.0))
    assert ws.nodes[7]["addr"] == "10.0.0.8:5000"   # address change picked up
    assert ws.nodes[7]["last_seen"] == 101.0


def test_a_bad_payload_cannot_break_anything():
    ws = _ws()
    ws.apply_event(messages.packet(7, "H", b"\x01", "10.0.0.7:5000", ts=100.0))  # too short for telemetry
    ws.apply_event(messages.packet(7, "P", b"", "10.0.0.7:5000", ts=100.5))      # talk with no payload
    ws.apply_event(messages.packet(7, "Z", b"???", "10.0.0.7:5000", ts=101.0))   # unknown type
    assert ws.nodes[7]["status"] == "active"
    assert ws.nodes[7]["last_seen"] == 101.0


def test_liveness_is_checked_on_the_websocket_side():
    ws = _ws(node_timeout=3.0)
    ws.apply_event(messages.packet(7, "H", b"", "10.0.0.7:5000", ts=100.0))
    ws.check_liveness(102.0)
    assert ws.nodes[7]["status"] == "active"
    ws.check_liveness(104.0)
    assert ws.nodes[7]["status"] != "active"
    ws.apply_event(messages.packet(7, "H", b"", "10.0.0.7:5000", ts=105.0))
    assert ws.nodes[7]["status"] == "active"   # back as soon as it is heard again


async def test_a_talking_node_stays_online_without_heartbeats():
    """Devices skip heartbeats while they send audio; the gateway must not mark them dropped."""
    q_in, q_out = asyncio.Queue(), asyncio.Queue()
    server = await udp_devserver.start(q_in, q_out, "127.0.0.1", 0)
    transport, _ = await _device(server.port)
    ws = _ws(node_timeout=3.0)
    try:
        frame = protocol.build(protocol.TYPE_AUDIO, 4, b"\x00\x00" * 320)
        for _ in range(25):                    # half a second of talking: one note, not 25
            transport.sendto(frame)
        await asyncio.sleep(0.1)
        notes = []
        while not q_in.empty():
            notes.append(q_in.get_nowait())
        assert len(notes) == 1

        ws.apply_event({**notes[0], "ts": 100.0})   # first sign of the node is its audio
        assert ws.nodes[4]["status"] == "active"
        ws.apply_event({**notes[0], "ts": 102.5})   # still talking 2.5 s later
        ws.check_liveness(105.0)
        assert ws.nodes[4]["status"] == "active"   # 2.5 s since the last packet, under 3 s
    finally:
        transport.close()
        for task in server._tasks:
            task.cancel()


def test_poc_heartbeat_fills_the_link_column():
    ws = _ws()
    ws.apply_event(messages.packet(9, "H", bytes([0xC3]), "10.0.0.9:5000", ts=100.0))   # -61 dBm
    assert ws.nodes[9]["link"] == -61
    ws.apply_event(messages.packet(9, "H", bytes([127]), "10.0.0.9:5000", ts=101.0))     # unavailable
    assert ws.nodes[9]["link"] == -61   # keeps the last known value
    assert ws.nodes[9]["role"] is None  # role, level and topology come after the POC
