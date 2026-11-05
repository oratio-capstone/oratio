import protocol


def test_round_trip():
    data = protocol.build(protocol.TYPE_AUDIO, 513, b"\x01\x02")
    packet = protocol.parse(data)
    assert packet == protocol.Packet(b"A", 513, b"\x01\x02")


def test_node_id_is_big_endian_in_bytes_1_and_2():
    # Matches the existing udp_server.py: node_id = (buffer[1] << 8) | buffer[2]
    data = protocol.build(protocol.TYPE_HEARTBEAT, 0x0102)
    assert data[1] == 0x01 and data[2] == 0x02


def test_short_datagram_is_rejected_not_crashing():
    assert protocol.parse(b"") is None
    assert protocol.parse(b"H\x00") is None


def test_heartbeat_telemetry():
    payload = protocol.build_heartbeat(-61, 2.5, 4.0)
    assert protocol.parse_heartbeat(payload) == (-61, 2.5, 4.0)
    assert protocol.parse_heartbeat(b"") is None


def test_audio_frame_size():
    assert protocol.AUDIO_FRAME_BYTES == 640
