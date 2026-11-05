"""Gateway process 1: the UDP side and the WebSocket server on one asyncio loop.

    python sw/gateway/main.py

The two halves only talk through q_in (UDP -> WebSocket) and q_out
(WebSocket -> UDP); see messages.py for what goes on each.
"""

from __future__ import annotations

import asyncio
import logging

import config
import udp_devserver  # TODO: swap for the asyncio UDP server once it exposes start(q_in, q_out, host, port)
from ws_server import WebSocketServer


async def main() -> None:
    logging.basicConfig(level=config.LOG_LEVEL,
                        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s")

    q_in: asyncio.Queue = asyncio.Queue(maxsize=config.QUEUE_SIZE)
    q_out: asyncio.Queue = asyncio.Queue(maxsize=config.QUEUE_SIZE)

    ws = WebSocketServer(q_in, q_out, path=config.WS_PATH,
                         client_buffer=config.CLIENT_BUFFER, alert_history=config.ALERT_HISTORY,
                         names_file=config.NAMES_FILE)
    await ws.start(config.WS_HOST, config.WS_PORT)
    await udp_devserver.start(q_in, q_out, config.UDP_HOST, config.UDP_PORT,
                              timeout=config.HEARTBEAT_TIMEOUT_S)

    await asyncio.Event().wait()  # run until stopped (Ctrl+C or systemd)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
