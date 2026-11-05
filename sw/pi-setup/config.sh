# Shared settings for the Pi setup scripts.
# Every value can be overridden from the command line, for example:
#   sudo AP_CHANNEL=11 ./04-access-point.sh
# shellcheck shell=bash

# Account that owns the repo and runs the services (the user you log in as).
APP_USER="${APP_USER:-${SUDO_USER:-oratio}}"
APP_HOME="$(getent passwd "$APP_USER" | cut -d: -f6)"

# Repository
REPO_URL="${REPO_URL:-https://github.com/oratio-capstone/oratio.git}"
REPO_BRANCH="${REPO_BRANCH:-main}"
REPO_DIR="${REPO_DIR:-$APP_HOME/oratio-repo}"
VENV_DIR="${VENV_DIR:-$APP_HOME/oratio-venv}"

# Gateway service (process 1). Path is relative to REPO_DIR.
GATEWAY_SERVICE="${GATEWAY_SERVICE:-oratio-gateway}"
GATEWAY_ENTRY="${GATEWAY_ENTRY:-sw/udp_server.py}"

# Web service (process 2): static dashboard files. Path is relative to REPO_DIR.
WEB_SERVICE="${WEB_SERVICE:-oratio-web}"
WEB_DIR="${WEB_DIR:-sw/web}"
WEB_PORT="${WEB_PORT:-8080}"

# Access point
AP_CON="${AP_CON:-oratio-ap}"
AP_SSID="${AP_SSID:-oratio-gw}"
AP_IFACE="${AP_IFACE:-wlan0}"
AP_CHANNEL="${AP_CHANNEL:-6}"          # 1, 6 or 11; must be 2.4 GHz for the ESP32
AP_ADDR="${AP_ADDR:-192.168.4.1/24}"
AP_PRIORITY="${AP_PRIORITY:-10}"       # higher than the home Wi-Fi profile (0)

# Console keyboard layout
KEYBOARD_LAYOUT="${KEYBOARD_LAYOUT:-us}"

# --- helpers -------------------------------------------------------------

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

require_root() {
  [[ $EUID -eq 0 ]] || die "run this script with sudo: sudo $0"
  [[ -n "$APP_HOME" ]] || die "user '$APP_USER' does not exist (set APP_USER=...)"
}

as_app_user() { sudo -u "$APP_USER" -H "$@"; }
