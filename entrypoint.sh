#!/usr/bin/env sh
set -eu

# Generate self-signed TLS cert if missing
if [ ! -f /app/server.crt ] || [ ! -f /app/server.key ]; then
  openssl req -x509 -newkey rsa:4096 \
    -keyout /app/server.key \
    -out /app/server.crt \
    -days 365 \
    -nodes \
    -subj "/CN=localhost"
fi

# Create default admin settings if missing
if [ ! -f /app/admin.settings ]; then
  printf "admin\nfly.board\n" > /app/admin.settings
fi

exec stdbuf -oL -eL ./fly_board
