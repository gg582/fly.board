#!/bin/sh
set -eu

REPO_DIR=${1:-/root/fly.board}
VENV_DIR="$REPO_DIR/.translation-venv"

python3 -m venv "$VENV_DIR"
"$VENV_DIR/bin/pip" install --disable-pip-version-check --only-binary=:all: \
    -r "$REPO_DIR/deploy/translation-requirements.txt"
install -m 0644 "$REPO_DIR/deploy/flyboard-translation.service" \
    /etc/systemd/system/flyboard-translation.service
mkdir -p /var/lib/flyboard-translation
systemctl daemon-reload
systemctl enable --now flyboard-translation.service
