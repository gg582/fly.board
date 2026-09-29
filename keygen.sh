#!/usr/bin/env sh
set -eu

if [ -f server.crt ] && [ -f server.key ]; then
  echo "server.crt and server.key already exist"
  exit 0
fi

# ECDSA P-256 by default: every full TLS handshake signs with the server
# key, and a P-256 signature costs a small fraction of an RSA-4096 one (the
# RSA sign dominated server CPU under connection churn). Set KEY_TYPE=rsa
# for clients that cannot do ECDSA.
KEY_TYPE="${KEY_TYPE:-ec}"
case "$KEY_TYPE" in
  ec)  NEWKEY="ec" ; KEYOPT="-pkeyopt ec_paramgen_curve:prime256v1" ;;
  rsa) NEWKEY="rsa:4096" ; KEYOPT="" ;;
  *)   echo "KEY_TYPE must be ec or rsa" >&2 ; exit 1 ;;
esac

# shellcheck disable=SC2086
openssl req -x509 -newkey "$NEWKEY" $KEYOPT \
  -keyout server.key \
  -out server.crt \
  -days 365 \
  -nodes \
  -subj "/CN=localhost"

echo "created server.crt and server.key ($KEY_TYPE)"
