# Raspberry Pi gateway setup

Scripts that turn a freshly flashed Raspberry Pi 5 into the Oratio gateway: packages, code, the
gateway service and the `oratio-gw` access point. They automate the steps in the team's
"Raspberry Pi Gateway Bringup — Full Walkthrough". Every script checks before it changes
anything, so re-running one on a working Pi is safe.

## Before you start

Flash **Raspberry Pi OS Lite (64-bit)** with Raspberry Pi Imager and, in its settings, set:

- hostname `oratio-gateway`, username `oratio` (password shared privately, never in the repo)
- SSH enabled
- your Wi-Fi network (needed once, for downloading packages)

Boot the Pi and SSH in (`ssh oratio@oratio-gateway.local`). Raspberry Pi OS Lite has no git,
so install it and clone the repo once by hand:

```bash
sudo apt update && sudo apt install -y git
git clone https://github.com/oratio-capstone/oratio.git ~/oratio-repo
cd ~/oratio-repo/sw/pi-setup
```

## Run everything

```bash
sudo ./setup.sh
```

It runs steps 1 to 3, then asks before step 4, because starting the access point disconnects the
Pi from your Wi-Fi. **Do step 4 over Ethernet or with a monitor and keyboard.**

## What each step does

| Script | What it does | Run as |
| --- | --- | --- |
| `01-base.sh` | Updates packages, installs git / Python venv / pip, sets the console keyboard to US | `sudo` |
| `02-gateway.sh` | Clones or updates the repo, creates the virtual environment, installs `sw/requirements.txt` if it exists | `sudo` |
| `03-services.sh` | Installs and starts `oratio-gateway` (and `oratio-web` once `sw/web/` exists) as systemd services: restart on failure, start at boot, unbuffered logs | `sudo` |
| `04-access-point.sh` | Creates the `oratio-gw` access point: 2.4 GHz, channel 6, `192.168.4.1/24`, DHCP, starts at boot. Asks for the Wi-Fi password | `sudo` |
| `maintenance-mode.sh` | `home` joins a normal Wi-Fi network for updates, `ap` returns to the access point, `status` shows which is active | `sudo` (except `status`) |

## Settings

All defaults live in `config.sh`. Override any of them on the command line, for example:

```bash
sudo AP_CHANNEL=11 ./04-access-point.sh          # different Wi-Fi channel
sudo REPO_BRANCH=my-branch ./02-gateway.sh        # run a different branch
sudo GATEWAY_ENTRY=sw/gateway/main.py ./03-services.sh   # once the asyncio gateway lands
```

The default gateway entry is `sw/udp_server.py`. Until that file is on `main`, point the scripts
at the branch that has it with `REPO_BRANCH=...`.

## Updating the Pi later

```bash
sudo ./maintenance-mode.sh home    # join a network with internet (drops Wi-Fi SSH)
sudo ./02-gateway.sh               # pull the latest code
sudo ./03-services.sh              # restart the services if anything changed
sudo ./maintenance-mode.sh ap      # back to the field access point
```

Never edit code directly on the Pi: change it in the repo and pull. `02-gateway.sh` refuses to
update a checkout with local edits.

## Moving an existing Pi onto these scripts

A Pi set up by hand runs a service called `udp-server` on the same port. `03-services.sh` stops if
it finds it; to replace it with `oratio-gateway`, run:

```bash
sudo REPLACE_OLD_SERVICE=1 ./03-services.sh
```

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| Service runs but `journalctl` shows nothing | Output buffering; the unit sets `PYTHONUNBUFFERED=1`, so re-run `03-services.sh` |
| `/` types `é` on the Pi's console | Keyboard layout; re-run `01-base.sh` and reboot |
| ESP32 can't see `oratio-gw` | The ESP32 is 2.4 GHz only; keep `AP_CHANNEL` at 1, 6 or 11 |
| `04-access-point.sh` refuses to run over SSH | You are connected over Wi-Fi and would be cut off; use Ethernet or a monitor |
