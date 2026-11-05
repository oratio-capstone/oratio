#!/usr/bin/env bash
# Step 2: get the code and create the Python virtual environment.
# Safe to re-run: pulls the latest code instead of re-cloning.
set -euo pipefail
source "$(dirname "$0")/config.sh"
require_root

if [[ -d "$REPO_DIR/.git" ]]; then
  log "Repo found at $REPO_DIR, updating"
  if [[ -n "$(as_app_user git -C "$REPO_DIR" status --porcelain --untracked-files=no)" ]]; then
    die "$REPO_DIR has local edits. Commit or discard them first; code is changed in the repo, never on the Pi."
  fi
  as_app_user git -C "$REPO_DIR" fetch origin "$REPO_BRANCH"
  as_app_user git -C "$REPO_DIR" checkout "$REPO_BRANCH"
  as_app_user git -C "$REPO_DIR" pull --ff-only origin "$REPO_BRANCH"
else
  log "Cloning $REPO_URL ($REPO_BRANCH) into $REPO_DIR"
  as_app_user git clone --branch "$REPO_BRANCH" "$REPO_URL" "$REPO_DIR"
fi

python3 -c 'import sys; sys.exit(sys.version_info < (3, 11))' \
  || die "Python 3.11 or newer is required (found $(python3 --version))"

if [[ ! -x "$VENV_DIR/bin/python" ]]; then
  log "Creating virtual environment at $VENV_DIR"
  as_app_user python3 -m venv "$VENV_DIR"
else
  log "Virtual environment already exists at $VENV_DIR"
fi

# Dependencies are installed only after the code exists, since the list lives in the repo.
REQS="$REPO_DIR/sw/requirements.txt"
if [[ -f "$REQS" ]]; then
  log "Installing Python dependencies from sw/requirements.txt"
  as_app_user "$VENV_DIR/bin/pip" install --upgrade -r "$REQS"
else
  log "No sw/requirements.txt on this branch; skipping Python dependencies"
fi

log "Gateway code ready"
