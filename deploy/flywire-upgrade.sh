#!/bin/sh
# FlyWire auto-upgrade template (deploy/ only — the operator reviews, copies
# to the site root as ./flywire-upgrade.sh, chmod +x, and restarts).
#
# Invoked detached by fly_board on a replica when the primary advertises a
# different git version and flywire.settings has auto_upgrade=1. The C code
# never runs git/make itself; this script owns the whole upgrade.
#
# Override the systemd unit name with FLYWIRE_SERVICE if it is not "flyboard".
set -e

log() { echo "[flywire-upgrade] $*"; }

cd "$(dirname "$0")"
log "starting in $(pwd)"

if [ -d .git ]; then
    log "git fetch origin"
    if ! git fetch origin; then
        log "ERROR: git fetch failed"
        exit 1
    fi
    log "git reset --hard origin/main"
    if ! git reset --hard origin/main; then
        log "ERROR: git reset failed"
        exit 1
    fi
else
    log "not a git checkout; skipping git update"
fi

log "build"
if ! make -j2 fly_board mail-tools; then
    log "ERROR: build failed"
    exit 1
fi

SERVICE="${FLYWIRE_SERVICE:-flyboard}"
log "restart $SERVICE"
if ! systemctl restart "$SERVICE"; then
    log "ERROR: systemctl restart $SERVICE failed"
    exit 1
fi

log "done"
