#!/usr/bin/env sh
set -eu

if [ -f server.crt ] && [ -f server.key ]; then
  echo "server.crt and server.key already exist"
  exit 0
fi

openssl req -x509 -newkey rsa:4096 \
  -keyout server.key \
  -out server.crt \
  -days 365 \
  -nodes \
  -subj "/CN=localhost"

echo "created server.crt and server.key"
