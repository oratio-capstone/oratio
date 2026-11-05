#!/usr/bin/env bash
# Step 4: turn the Pi's Wi-Fi into the oratio-gw access point.
#
# WARNING: once the AP starts, the Pi leaves its current Wi-Fi network.
# Run this over Ethernet or with a monitor and keyboard, not over Wi-Fi SSH.
#
# The password is asked for at run time (or read from AP_PSK) and is never
# stored in the repo. Safe to re-run: updates the existing profile.
set -euo pipefail
source "$(dirname "$0")/config.sh"
require_root

command -v nmcli >/dev/null || die "NetworkManager (nmcli) not found"

# Refuse to cut off an SSH session that is running over the Wi-Fi interface.
if [[ -n "${SSH_CONNECTION:-}" ]]; then
  ssh_local_ip="$(awk '{print $3}' <<<"$SSH_CONNECTION")"
  if ip -4 addr show dev "$AP_IFACE" 2>/dev/null | grep -q "inet $ssh_local_ip/"; then
    [[ "${FORCE:-0}" == "1" ]] || die "you are connected over $AP_IFACE and would be cut off. Use Ethernet or a monitor, or re-run with FORCE=1."
  fi
fi

if [[ -z "${AP_PSK:-}" ]]; then
  read -r -s -p "Password for $AP_SSID (8+ characters): " AP_PSK; echo
fi
[[ ${#AP_PSK} -ge 8 ]] || die "WPA2 passwords need at least 8 characters"

settings=(
  connection.interface-name "$AP_IFACE"
  wifi.ssid "$AP_SSID"
  wifi.mode ap
  wifi.band bg
  wifi.channel "$AP_CHANNEL"
  ipv4.method shared
  ipv4.addresses "$AP_ADDR"
  ipv6.method disabled
  wifi-sec.key-mgmt wpa-psk
  wifi-sec.psk "$AP_PSK"
  connection.autoconnect yes
  connection.autoconnect-priority "$AP_PRIORITY"
)

if nmcli -t -f NAME connection show | grep -qx "$AP_CON"; then
  log "Updating existing '$AP_CON' profile"
  nmcli connection modify "$AP_CON" "${settings[@]}"
else
  log "Creating '$AP_CON' profile (SSID $AP_SSID, channel $AP_CHANNEL, $AP_ADDR)"
  nmcli connection add type wifi con-name "$AP_CON" ifname "$AP_IFACE" ssid "$AP_SSID" >/dev/null
  nmcli connection modify "$AP_CON" "${settings[@]}"
fi

if [[ "${NO_ACTIVATE:-0}" == "1" ]]; then
  log "Profile saved; not activating (NO_ACTIVATE=1). It will start at next boot."
  exit 0
fi

log "Starting the access point"
nmcli connection up "$AP_CON"

sleep 2
ip -4 addr show dev "$AP_IFACE" | grep -q "inet ${AP_ADDR%/*}/" \
  || die "AP started but $AP_IFACE does not have ${AP_ADDR%/*}; check: nmcli dev status"
log "Access point '$AP_SSID' is up at ${AP_ADDR%/*}. Join it and run: ssh $APP_USER@${AP_ADDR%/*}"
