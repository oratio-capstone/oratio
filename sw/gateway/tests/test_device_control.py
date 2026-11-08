"""The UDP side tells a device when to stream its microphone and when to show an emergency listen."""

import asyncio

import messages
import protocol
import udp_devserver


class Device(asyncio.DatagramProtocol):
    def __init__(self):
        self.flags = asyncio.Queue()

    def datagram_received(self, data, addr):
        packet = protocol.parse(data)
        if packet and packet.type == protocol.TYPE_CONTROL:
            self.flags.put_nowait(packet.payload[0])


async def test_device_is_told_to_stream_and_to_show_an_emergency_listen():
    q_in, q_out = asyncio.Queue(), asyncio.Queue()
    server = await udp_devserver.start(q_in, q_out, "127.0.0.1", 0, timeout=5)
    loop = asyncio.get_running_loop()
    transport, device = await loop.create_datagram_endpoint(Device, remote_addr=("127.0.0.1", server.port))
    try:
        transport.sendto(protocol.build(protocol.TYPE_HEARTBEAT, 7))
        await asyncio.sleep(0.05)
        next_flags = lambda: asyncio.wait_for(device.flags.get(), 2)  # noqa: E731

        await q_out.put(messages.notify(7, "emergency_listen_on"))
        await q_out.put(messages.listen(7))
        assert await next_flags() == protocol.CONTROL_EMERGENCY
        assert await next_flags() == protocol.CONTROL_EMERGENCY | protocol.CONTROL_STREAM

        await q_out.put(messages.stop(7))
        await q_out.put(messages.notify(7, "emergency_listen_off"))
        assert await next_flags() == protocol.CONTROL_EMERGENCY
        assert await next_flags() == 0
    finally:
        transport.close()
        for task in server._tasks:
            task.cancel()
