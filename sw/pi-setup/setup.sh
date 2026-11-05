#!/usr/bin/env bash
# Run every setup step in order. Each step is safe to re-run on its own.
#   sudo ./setup.sh
set -euo pipefail
cd "$(dirname "$0")"
source ./config.sh
require_root

./01-base.sh
./02-gateway.sh
./03-services.sh

echo
warn "The next step turns Wi-Fi into the $AP_SSID access point and disconnects the Pi from its current Wi-Fi."
read -r -p "Continue with the access point now? [y/N] " answer
if [[ "$answer" =~ ^[Yy]$ ]]; then
  ./04-access-point.sh
else
  log "Skipped. Run 'sudo ./04-access-point.sh' later over Ethernet or with a monitor."
fi
