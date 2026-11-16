#!/usr/bin/env bash
# Step 1: system packages and console keyboard layout.
# Safe to re-run.
set -euo pipefail
source "$(dirname "$0")/config.sh"
require_root

log "Updating system packages (this can take a few minutes)"
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get upgrade -y

log "Installing git, Python venv and pip"
DEBIAN_FRONTEND=noninteractive apt-get install -y git python3-venv python3-pip

log "Setting console keyboard layout to '$KEYBOARD_LAYOUT'"
if command -v raspi-config >/dev/null; then
  raspi-config nonint do_configure_keyboard "$KEYBOARD_LAYOUT"
else
  sed -i "s/^XKBLAYOUT=.*/XKBLAYOUT=\"$KEYBOARD_LAYOUT\"/" /etc/default/keyboard
  command -v setupcon >/dev/null && setupcon || true
fi

log "Base setup done"
