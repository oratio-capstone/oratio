import protocol


def test_round_trip():
    data = protocol.build(protocol.TYPE_AUDIO, 513, b"\x01\x02", seq=7)
    assert len(data) == protocol.HEADER_SIZE + 2 == 9
    assert protocol.parse(data) == protocol.Packet(b"A", 513, 7, b"\x01\x02")


def test_header_matches_the_firmware_byte_for_byte():
    # Same vectors as fw/test/test_155_protocol: node 0x1234, sequence 0x0102, big-endian,
    # then the payload length.
    data = protocol.build(protocol.TYPE_HEARTBEAT, 0x1234, b"\xAA\xBB\xCC", seq=0x0102)
    assert data == b"H\x12\x34\x01\x02\x00\x03\xAA\xBB\xCC"


def test_node_id_matches_udp_server_py():
    # sw/udp_server.py reads node_id = (buffer[1] << 8) | buffer[2]
    data = protocol.build(protocol.TYPE_HEARTBEAT, 0x0102)
    assert (data[1] << 8) | data[2] == 0x0102


def test_sequence_wraps_at_16_bits():
    assert protocol.parse(protocol.build(protocol.TYPE_AUDIO, 1, seq=0x10001)).seq == 1


def test_short_or_inconsistent_datagram_is_rejected_not_crashing():
    assert protocol.parse(b"") is None
    assert protocol.parse(b"H\x00\x01\x00") is None              # shorter than the header
    good = protocol.build(protocol.TYPE_SOS, 3, b"\x01")
    assert protocol.parse(good[:-1]) is None                        # payload cut off
    assert protocol.parse(good + b"\x00") is None                  # extra trailing byte


def test_heartbeat_telemetry():
    payload = protocol.build_heartbeat("relay", 2, -61, "02:00:00:00:00:01", "02:00:00:00:00:0a")
    assert len(payload) == 15
    hb = protocol.parse_heartbeat(payload)
    assert hb == ("relay", 2, -61, "02:00:00:00:00:01", "02:00:00:00:00:0a")
    assert hb.role == "relay" and hb.rssi == -61
    assert protocol.parse_heartbeat(b"") is None
    assert protocol.parse_heartbeat(payload[:-1]) is None  # truncated


def test_audio_frame_size():
    assert protocol.AUDIO_FRAME_BYTES == 640
