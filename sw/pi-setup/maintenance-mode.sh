#!/usr/bin/env bash
# Switch the Pi's Wi-Fi between the field AP and a normal network with internet.
#
#   sudo ./maintenance-mode.sh home [profile]   join a normal Wi-Fi network (for git pull, apt)
#   sudo ./maintenance-mode.sh ap               go back to hosting oratio-gw
#   ./maintenance-mode.sh status                show which profile is active
#
# Switching drops any SSH session running over Wi-Fi; reconnect on the new network.
# At boot the Pi always returns to the AP (it has the higher autoconnect priority).
set -euo pipefail
source "$(dirname "$0")/config.sh"

mode="${1:-status}"

case "$mode" in
  status)
    nmcli -t -f NAME,TYPE,DEVICE connection show --active
    ;;
  ap)
    require_root
    log "Switching to access point '$AP_SSID'"
    nmcli connection up "$AP_CON"
    ;;
  home)
    require_root
    profile="${2:-}"
    if [[ -z "$profile" ]]; then
      # First saved Wi-Fi profile that isn't the AP.
      profile="$(nmcli -t -f NAME,TYPE connection show \
        | awk -F: -v ap="$AP_CON" '$2=="802-11-wireless" && $1!=ap {print $1; exit}')"
    fi
    [[ -n "$profile" ]] || die "no saved Wi-Fi profile found; pass one: $0 home <profile>"
    log "Switching to '$profile' (the AP will go down until you run: $0 ap)"
    nmcli connection up "$profile"
    ;;
  *)
    die "usage: $0 {status|ap|home [profile]}"
    ;;
esac
