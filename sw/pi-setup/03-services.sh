#!/usr/bin/env bash
# Step 3: install and start the systemd services.
#   oratio-gateway  runs the gateway (UDP server, later + WebSocket server)
#   oratio-web      serves the static dashboard page (only once sw/web/ exists)
# Safe to re-run: units are rewritten only when their content changes.
set -euo pipefail
source "$(dirname "$0")/config.sh"
require_root

ENTRY_PATH="$REPO_DIR/$GATEWAY_ENTRY"
[[ -f "$ENTRY_PATH" ]] || die "gateway entry $ENTRY_PATH not found (set GATEWAY_ENTRY=... or check the branch)"
[[ -x "$VENV_DIR/bin/python" ]] || die "no virtual environment at $VENV_DIR; run 02-gateway.sh first"

# The old hand-made service binds the same UDP port, so the two can't run together.
if systemctl list-unit-files udp-server.service >/dev/null 2>&1 \
   && systemctl is-enabled --quiet udp-server.service 2>/dev/null; then
  if [[ "${REPLACE_OLD_SERVICE:-0}" == "1" ]]; then
    log "Stopping and disabling the old udp-server service"
    systemctl disable --now udp-server.service
  else
    die "the old 'udp-server' service is enabled and uses the same port. Re-run with: sudo REPLACE_OLD_SERVICE=1 $0"
  fi
fi

# Write a unit file only if it changed; return 0 if it was (re)written.
install_unit() {
  local name="$1" content="$2" path="/etc/systemd/system/$1.service"
  if [[ -f "$path" ]] && [[ "$(cat "$path")" == "$content" ]]; then
    log "$name.service unchanged"
    return 1
  fi
  log "Writing $path"
  printf '%s\n' "$content" > "$path"
  return 0
}

changed=0

gateway_unit="[Unit]
Description=Oratio gateway (UDP server)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$APP_USER
WorkingDirectory=$(dirname "$ENTRY_PATH")
ExecStart=$VENV_DIR/bin/python $ENTRY_PATH
Environment=PYTHONUNBUFFERED=1
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target"
install_unit "$GATEWAY_SERVICE" "$gateway_unit" && changed=1

web_enabled=0
if [[ -d "$REPO_DIR/$WEB_DIR" ]]; then
  web_enabled=1
  web_unit="[Unit]
Description=Oratio dashboard web server (static files)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$APP_USER
ExecStart=$VENV_DIR/bin/python -m http.server $WEB_PORT --directory $REPO_DIR/$WEB_DIR
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target"
  install_unit "$WEB_SERVICE" "$web_unit" && changed=1
else
  log "No $WEB_DIR yet; skipping $WEB_SERVICE (re-run this script once the dashboard exists)"
fi

systemctl daemon-reload

log "Enabling and (re)starting $GATEWAY_SERVICE"
systemctl enable "$GATEWAY_SERVICE" >/dev/null
if [[ $changed -eq 1 ]]; then systemctl restart "$GATEWAY_SERVICE"; else systemctl start "$GATEWAY_SERVICE"; fi

if [[ $web_enabled -eq 1 ]]; then
  log "Enabling and (re)starting $WEB_SERVICE"
  systemctl enable "$WEB_SERVICE" >/dev/null
  if [[ $changed -eq 1 ]]; then systemctl restart "$WEB_SERVICE"; else systemctl start "$WEB_SERVICE"; fi
fi

sleep 2
systemctl --no-pager --lines=0 status "$GATEWAY_SERVICE" || die "$GATEWAY_SERVICE is not running; check: journalctl -u $GATEWAY_SERVICE -n 50"
log "Services running. Live logs: journalctl -u $GATEWAY_SERVICE -f"
