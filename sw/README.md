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
http://localhost:8080. **Ctrl+C stops all three.** The dashboard asks for a password: on your laptop it
is the throwaway `oratio-dev`, printed when `run_dev.py` starts.

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

**Who can be heard.** Nobody is listened to from the dashboard by default. **Listen** opens only
while that worker has an open SOS (the SOS button, later also the spoken keyword); the gateway enforces
this, not just the page. A listen that started during an SOS carries on after it's acknowledged.
Anyone else needs **Emergency…**, a confirmed emergency listen: it's recorded in the Log tab (when,
which dashboard, how long) and the worker's device is told so it can show them (LED / beep). In both
cases the gateway asks the device to stream its microphone. Workers still hear each other in the
open group channel; this only covers the dashboard. (The gateway also supports a talk button, unused
because the devices have none.)

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

### Dashboard login

The dashboard needs a password; until a browser has logged in, the gateway sends it nothing and
ignores its commands. There is one shared password. The gateway only stores a salted hash of it,
in `~/.local/share/oratio/dashboard-password` (outside the repo, readable only by the gateway's user).
A login lasts 12 hours on that browser, and restarting the gateway logs everyone out. Five wrong
passwords from one address lock it out for a minute, and pages from other websites can't connect.

- Set or change it (on the Pi: as the gateway's user, then restart the gateway):
  `python sw/gateway/auth.py set`. `pi-setup/03-services.sh` asks for it on first setup.
- With no password set, the gateway still routes audio, but every dashboard login is refused.
- Never commit it, and don't reuse the Wi-Fi password. Over the Pi's own Wi-Fi, WPA2 encrypts the
  connection; the dashboard itself uses plain `ws://`, so don't expose it on another network.

Settings (ports, timeouts, queue sizes, names file, events file and retention) are in `gateway/config.py` and can be overridden with
environment variables, for example `WS_PORT=9000 python sw/gateway/main.py`.

`gateway/udp_devserver.py` is a **temporary stand-in** for the UDP side. Like `udp_server.py`, it only
routes (forwards audio between nodes and sends control packets) and passes everything else to the
WebSocket server, which decodes it. The asyncio version of
`udp_server.py` replaces it by exposing the same `start(q_in, q_out, host, port)` coroutine and
following the queue contract in `gateway/messages.py`.

## Tests

```bash
cd sw && python -m pytest
```
