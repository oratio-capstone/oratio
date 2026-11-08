"""Simulator -> UDP stand-in -> queues -> WebSocket server -> browser client, all on localhost."""

import asyncio
import json
import sys
from pathlib import Path

import pytest
from websockets.asyncio.client import connect

import udp_devserver
from ws_server import WebSocketServer

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import sim_nodes  # noqa: E402


@pytest.mark.asyncio
async def test_simulated_nodes_show_up_and_drop():
    q_in: asyncio.Queue = asyncio.Queue(maxsize=200)
    q_out: asyncio.Queue = asyncio.Queue(maxsize=200)
    ws = WebSocketServer(q_in, q_out)
    server = await ws.start("127.0.0.1", 0)
    ws_port = server.sockets[0].getsockname()[1]
    udp = await udp_devserver.start(q_in, q_out, "127.0.0.1", 0, timeout=0.5)
    udp_port = udp.port

    args = sim_nodes.argparse.Namespace(host="127.0.0.1", port=udp_port, nodes=3, talkers=1, heartbeat=0.1,
                                        relays=2, spacing=60.0, reroute=0.3,
                                        drop=["2:0.3"], sos=["3:0.2"], duration=2.0, seed=1)
    sim = asyncio.create_task(sim_nodes.run(args))
    try:
        async with connect(f"ws://127.0.0.1:{ws_port}/ws") as client:
            await client.recv()  # snapshot
            asked = False  # listen to node 3 once its SOS arrives (allowed only then)
            seen, statuses, alerts, audio_frames, latest = set(), {}, set(), 0, {}
            deadline = asyncio.get_running_loop().time() + 1.8
            while asyncio.get_running_loop().time() < deadline:
                try:
                    msg = await asyncio.wait_for(client.recv(), 0.2)
                except asyncio.TimeoutError:
                    continue
                if isinstance(msg, bytes):
                    audio_frames += 1
                    assert len(msg) == 2 + 640 and msg[:2] == (3).to_bytes(2, "big")
                    continue
                data = json.loads(msg)
                if data["type"] == "node_update":
                    seen.add(data["node"]["node_id"])
                    statuses[data["node"]["node_id"]] = data["node"]["status"]
                    latest[data["node"]["node_id"]] = data["node"]

                elif data["type"] == "alert":
                    alerts.add((data["alert"]["kind"], data["alert"]["node_id"]))
                    if data["alert"]["kind"] == "sos" and not asked:
                        await client.send(json.dumps({"type": "listen", "node_id": 3}))
                        asked = True
        assert seen == {1, 2, 3, 101, 102}
        # The tree is resolved from the simulated MACs: relay 101 hangs off the Pi, 102 off 101,
        # and a worker is near one of the relays.
        assert latest[101]["role"] == "relay" and latest[101]["parent"] == 0
        assert latest[102]["parent"] == 101 and latest[102]["level"] == 2
        assert latest[1]["role"] == "worker" and latest[1]["near"] in (101, 102)
        assert statuses[2] == "dropped"
        assert ("node_dropped", 2) in alerts and ("sos", 3) in alerts
        assert audio_frames > 10  # node 3 streams its microphone once the gateway asks (SOS)
    finally:
        sim.cancel()
        for task in udp._tasks:
            task.cancel()
        ws._consumer.cancel()
        ws._talk_task.cancel()
        server.close()
        await server.wait_closed()
