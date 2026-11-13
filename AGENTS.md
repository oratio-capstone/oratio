# AGENTS.md

Instructions for AI coding agents (and a quick start for new teammates) working in this repo.
Oratio: wearable ESP32-S3 nodes in a Wi-Fi mesh, a Raspberry Pi gateway (UDP server + dashboard).
Human-facing docs: `README.md`, `fw/README.md`, `sw/README.md`, `sw/pi-setup/README.md`.

## Layout

- `fw/`: ESP-IDF firmware, built with PlatformIO. ESP32-S3 only (the C3 was dropped).
  - `lib/audio`, `lib/network`, `lib/storage`, `src/main.c`, `test/` (hardware tests)
- `sw/udp_server.py`: the UDP router that runs on the Pi
- `sw/gateway/`: the gateway process (UDP side + WebSocket server)
- `sw/web/`: the dashboard (plain HTML/JS/CSS, no build step)
- `sw/tools/`: `run_dev.py` (whole stack locally) and `sim_nodes.py` (fake nodes)
- `sw/pi-setup/`: scripts that set up the Pi (numbered, run in order)

## Commands

Firmware (from `fw/`, PlatformIO Core **6.1.19** exactly; see `README.md`):

```sh
pio run -e root -e node -e esp32-s3-n8r8          # what CI builds
pio test -e node_test -f test_155_protocol --without-uploading --without-testing   # compile a test only
```

Tests in `fw/test/` need real boards. CI only checks that they compile; a build-only pass is not a test pass.

Gateway (from `sw/`, Python 3.12):

```sh
pip install -r requirements-dev.txt
python -m pytest -q
python tools/run_dev.py          # gateway + simulated nodes + dashboard on http://localhost:8080
```

## Rules that are easy to get wrong

- **`fw/dependencies.lock` must be committed whenever it changes.** A build rewrites it when the components
  in `src/idf_component.yml` change. CI fails if the lock it regenerates differs from the committed one.
- Don't edit generated `sdkconfig.*` files. Board settings go in `fw/sdkconfig.defaults` (shared) or
  `fw/sdkconfig.defaults.esp32s3` (S3).
- Pinned versions (PlatformIO platform in `platformio.ini`, PlatformIO Core 6.1.19, Python 3.12 in CI) move
  together and on purpose. Never `pip install -U` them.
- Firmware code follows `fw/STYLE_GUIDE.md` and `fw/.clang-format`. The headers in `fw/lib/audio` are agreed
  interfaces: don't change them without the team.
- **Packet format:** still evolving, so don't copy it into other docs. The firmware and
  `sw/gateway/protocol.py` (the gateway's copy, documented in its docstring) must always match: change both
  in the same PR, or in PRs that land together, and update `protocol.py`'s tests. Parts of the format are planned for
  after the POC, so the gateway must keep working when a node doesn't send them.
- **The UDP side only routes.** It reads the header, keeps the node → address table, forwards audio and sends
  control packets. Decoding, timers and dashboard logic belong on the WebSocket side (`ws_server.py`). The
  contract between them is `sw/gateway/messages.py`; update it when you change what crosses the queues.
- **Listening policy:** a dashboard may listen to a node only while it has an open SOS. An emergency listen is
  always allowed, but it is logged and the device is told. Don't weaken this.
- **Never commit secrets:** Wi-Fi credentials, the Pi access-point password, user passwords. Local credentials
  stay in ignored files.

## Workflow

- `main` is protected: open a PR, CI (Azure Pipelines) must pass, and a code owner (Adam or Yadah) must approve.
- Branch names: `<name>/feature/<issue>-<short-name>` (or `<name>/fix/...`, `<name>/docs/...`).
- CI skips firmware or gateway steps a PR didn't touch; editing `azure-pipelines.yml` runs everything.
- Keep PRs to one topic, and run the relevant build/tests above before pushing.
