# Gateway software

Python code that runs on the Raspberry Pi gateway, plus its setup scripts.

| Path | What it is |
| --- | --- |
| `requirements.txt` | Runtime dependencies (what the Pi runs) |
| `requirements-dev.txt` | Runtime + test tools (laptops and CI) |
| `gateway/` | Process 1: UDP side + WebSocket server on one asyncio loop |
| `web/` | Process 2: the static dashboard page |
| `tools/sim_nodes.py` | Fake mesh nodes over UDP, for development and load tests |
| `tools/run_dev.py` | Starts gateway + page + simulator in one terminal |
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

With the environment activated, from the repo root:

```bash
python sw/tools/run_dev.py
```

That starts the gateway (UDP 8989, WebSocket 8000), the dashboard page (port 8080) and a simulated
mesh in one terminal, with each line labelled `[gateway]`, `[web]` or `[sim]`, and opens
http://localhost:8080. **Ctrl+C stops all three.**

The simulated mesh is a tunnel with the Pi at the entrance, three fixed relays (101, 102, 103; 101 is
the root) and ten people (nodes 1–10) walking up and down it, each joining the relay they hear best.
By default: node 3 goes silent at 10 s (marked dropped about 3 s later), node 5 raises an SOS at
15 s, and relay 102 dies at 25 s so you can watch the people behind it reroute on the Topology tab.

| Variant | Command |
| --- | --- |
| Different simulation (any `sim_nodes.py` option) | `python sw/tools/run_dev.py --nodes 6 --talkers 2` |
| More relays, further apart | `python sw/tools/run_dev.py --relays 5 --spacing 80` |
| Gateway + page only, e.g. with a real ESP | `python sw/tools/run_dev.py --no-sim` |
| Don't open the browser | `python sw/tools/run_dev.py --no-browser` |

To run the pieces separately instead (one terminal each):

```bash
python sw/gateway/main.py
python -m http.server 8080 --directory sw/web
python sw/tools/sim_nodes.py --talkers 1 --drop 3:10 --sos 5:15
```

The dashboard has two tabs. **Live** lists people (with the relay they're near), relays (what each
connects to and how many people it carries) and alerts. **Topology** draws the mesh as a tree from
the Pi down; click a person to listen. The SOS banner and the Listening panel show on both tabs.

The **Log** tab shows the saved history (SOS, connections lost and back, relay failures, renames,
gateway restarts) with filters by person, type and period, summary numbers (SOS count, time to
acknowledge, connections lost, relay failures) and **Export CSV** for spreadsheets and reports. Events
are kept for 90 days in `~/.local/share/oratio/events.db` (SQLite, outside the repo). After a gateway
restart, the recent alerts and any SOS nobody acknowledged come back. To keep test runs out of the
real history, point the simulator session at another file: `EVENTS_DB=/tmp/test.db python sw/tools/run_dev.py`
(Windows Command Prompt: `set EVENTS_DB=%TEMP%\test.db`, then `python sw/tools/run_dev.py`;
PowerShell: `$env:EVENTS_DB="$env:TEMP\test.db"; python sw/tools/run_dev.py`).

Nodes can be renamed from the dashboard (✎ next to each node): people by name, relays by place
("Tunnel B 200 m"). Names are shared by every open dashboard and saved on the gateway in
`~/.local/share/oratio/node-names.json` (outside the repo), so they survive restarts. Clear a name to
go back to "Node 3".

Settings (ports, timeouts, queue sizes, names file, events file and retention) are in `gateway/config.py` and can be overridden with
environment variables, for example `WS_PORT=9000 python sw/gateway/main.py`.

`gateway/udp_devserver.py` is a **temporary stand-in** for the UDP side: it tracks nodes and feeds
the WebSocket server, but does not forward audio between nodes. The asyncio version of
`udp_server.py` replaces it by exposing the same `start(q_in, q_out, host, port)` coroutine and
following the queue contract in `gateway/messages.py`.

## Tests

```bash
cd sw && python -m pytest
```
