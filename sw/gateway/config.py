"""Gateway settings. Override any value with an environment variable of the same name."""

import os


def _get(name: str, default: str) -> str:
    return os.environ.get(name, default)


UDP_HOST = _get("UDP_HOST", "0.0.0.0")
UDP_PORT = int(_get("UDP_PORT", "8989"))

WS_HOST = _get("WS_HOST", "0.0.0.0")
WS_PORT = int(_get("WS_PORT", "8000"))
WS_PATH = _get("WS_PATH", "/ws")

# A node with no packet for this long is marked dropped.
HEARTBEAT_TIMEOUT_S = float(_get("HEARTBEAT_TIMEOUT_S", "3.0"))

# Bounded queues between the two halves; the oldest audio is dropped when full.
QUEUE_SIZE = int(_get("QUEUE_SIZE", "200"))

# Per-browser send buffer; a client this far behind is disconnected.
CLIENT_BUFFER = int(_get("CLIENT_BUFFER", "128"))

# How many recent alerts a newly opened dashboard receives.
ALERT_HISTORY = int(_get("ALERT_HISTORY", "50"))

# Node names given on the dashboard (e.g. who is wearing each device). Kept outside the repo.
NAMES_FILE = _get("NAMES_FILE", os.path.join(os.path.expanduser("~"), ".local", "share", "oratio",
                                             "node-names.json"))

LOG_LEVEL = _get("LOG_LEVEL", "INFO")
