#!/bin/sh
# FlyWire auto-upgrade: bring this replica's code to origin/main and restart.
#
# Instance state (settings, certs, DBs, uploads) must survive the upgrade.
# git reset --hard alone would revert TRACKED settings files (blog.settings,
# admin.settings, fonts.settings, ...) to upstream defaults, so they are
# backed up before the reset and restored after it. Untracked state
# (data/*.db, public/uploads, secrets, certs) is never touched by git.
set -e
cd "$(dirname "$0")"
LOG="FlyWire upgrade: $(date -Is)"

# 1. Back up tracked instance files that must not be reverted.
KEEP="blog.settings admin.settings fonts.settings robots.settings backup.settings"
TMPDIR_KEEP=$(mktemp -d)
for f in $KEEP; do
    [ -f "$f" ] && cp -p "$f" "$TMPDIR_KEEP/$f"
done

# 2. Pull code (shallow or full checkout both work).
git fetch origin
git reset --hard origin/main

# 3. Restore instance files.
for f in $KEEP; do
    [ -f "$TMPDIR_KEEP/$f" ] && cp -p "$TMPDIR_KEEP/$f" "$f"
done
rm -rf "$TMPDIR_KEEP"

# 4. Rebuild and restart.
make -j2 fly_board mail-tools
systemctl restart "${FLYWIRE_SERVICE:-flyboard}"

echo "$LOG -> done (now $(git rev-parse --short HEAD))"
