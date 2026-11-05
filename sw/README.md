# Gateway software

Python code that runs on the Raspberry Pi gateway, plus its setup scripts.

| Path | What it is |
| --- | --- |
| `requirements.txt` | Runtime dependencies (what the Pi runs) |
| `requirements-dev.txt` | Runtime + test tools (laptops and CI) |
| `gateway/` | Process 1: UDP side + WebSocket server on one asyncio loop |
| `web/` | Process 2: the static dashboard page |
| `tools/sim_nodes.py` | Fake mesh nodes over UDP, for development and load tests |
| `pi-setup/` | Scripts that set up a fresh Pi, see `pi-setup/README.md` |

## Python environment

Each machine has its own virtual environment; it is never committed.

| Where | Location | Created by |
| --- | --- | --- |
| Pi | `~/oratio-venv` | `pi-setup/02-gateway.sh` |
| Laptop | `sw/.venv` | The commands below |

Python **3.11 or newer** is required (check with `python3 --version`).

From the repo root, on macOS or Linux:

```bash
python3 -m venv sw/.venv
source sw/.venv/bin/activate
pip install -r sw/requirements-dev.txt
```

On Windows (PowerShell):

```powershell
py -m venv sw\.venv
sw\.venv\Scripts\Activate.ps1
pip install -r sw\requirements-dev.txt
```

Activate the environment in each new terminal before running or testing gateway code. In VS Code,
pick `sw/.venv` with **Python: Select Interpreter**.

## Changing dependencies

Versions are pinned exactly (`==`) so the Pi, CI and every laptop run the same code. To add or
upgrade a package: change the pin in `requirements.txt`, run `pip install -r sw/requirements-dev.txt`,
test, and commit the change. On the Pi, `pi-setup/02-gateway.sh` installs the new pins.

## Running the gateway on your laptop

With the environment activated, open three terminals from the repo root:

```bash
python sw/gateway/main.py                                  # 1. gateway: UDP 8989, WebSocket 8000
python -m http.server 8080 --directory sw/web              # 2. dashboard page
python sw/tools/sim_nodes.py --talkers 1 --drop 3:10 --sos 5:15   # 3. ten fake nodes
```

Open http://localhost:8080. Nodes appear within a second, node 5 raises an SOS at 15 s and node 3
drops at 10 s (marked dropped about 3 s later). `python sw/tools/sim_nodes.py --help` lists every option.

Settings (ports, timeouts, queue sizes) are in `gateway/config.py` and can be overridden with
environment variables, for example `WS_PORT=9000 python sw/gateway/main.py`.

`gateway/udp_devserver.py` is a **temporary stand-in** for the UDP side: it tracks nodes and feeds
the WebSocket server, but does not forward audio between nodes. The asyncio version of
`udp_server.py` replaces it by exposing the same `start(q_in, q_out, host, port)` coroutine and
following the queue contract in `gateway/messages.py`.

## Tests

```bash
cd sw && python -m pytest
```
