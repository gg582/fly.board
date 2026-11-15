#!/usr/bin/env bash
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d /tmp/flyboard_acme_cluster.XXXXXX)"
PIDS=""

cleanup() {
    for pid in $PIDS; do kill -TERM -- -"$pid" 2>/dev/null; done
    sleep 1
    for pid in $PIDS; do kill -KILL -- -"$pid" 2>/dev/null; wait "$pid" 2>/dev/null; done
    rm -rf "$TMP"
}
trap cleanup EXIT

make_node() {
    name="$1"
    port="$2"
    acme_port="$3"
    self_url="$4"
    other_url="$5"
    dir="$TMP/$name"
    mkdir -p "$dir/data"
    ln -s "$ROOT/public" "$dir/public"
    ln -s "$ROOT/posts" "$dir/posts"
    ln -s "$ROOT/legal" "$dir/legal"
    ln -s "$ROOT/img" "$dir/img"
    cat > "$dir/blog.settings" <<EOF
title=ACME $name
root_url=https://localhost:$port
port=$port
use_tls=true
use_http2=false
use_http3=false
use_tasfa=false
use_rss=false
EOF
    cat > "$dir/flywire.settings" <<EOF
mode=primary
token=acme-test-token
nodes=$self_url,$other_url
self=$self_url
site_host=
poll_seconds=1
failover_seconds=30
EOF
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$dir/server.key" -out "$dir/server.crt" -days 1 \
        -subj /CN=localhost -addext subjectAltName=DNS:localhost,IP:127.0.0.1 \
        >/dev/null 2>&1
}

make_node a 18445 19092 https://127.0.0.1:18445 https://127.0.0.1:18446
make_node b 18446 19093 https://127.0.0.1:18446 https://127.0.0.1:18445

start_node() {
    name="$1"
    acme_port="$2"
    cd "$TMP/$name" || exit 1
    FLY_CERT_RENEWAL=true FLY_CERT_HTTP_PORT="$acme_port" \
        ASAN_OPTIONS="detect_leaks=0:abort_on_error=1" \
        setsid "$ROOT/fly_board" > server.log 2>&1 &
    PIDS="$PIDS $!"
}

start_node a 19092
start_node b 19093

ready=0
for _ in $(seq 1 50); do
    if curl -k -sf --max-time 2 https://127.0.0.1:18445/ >/dev/null && \
       curl -k -sf --max-time 2 https://127.0.0.1:18446/ >/dev/null; then
        ready=1
        break
    fi
    sleep 0.2
done
if [ "$ready" -ne 1 ]; then
    echo "acme-cluster: nodes failed to start" >&2
    tail -30 "$TMP/a/server.log" >&2
    tail -30 "$TMP/b/server.log" >&2
    exit 1
fi

code="$(curl -k -sS -o /dev/null -w '%{http_code}' --max-time 5 \
    -H 'X-FlyWire-Token: acme-test-token' -H 'Content-Type: application/json' \
    --data '{"token":"cluster-token_1","response":"cluster-token_1.shared-proof"}' \
    https://127.0.0.1:18445/flywire/acme-challenge)"
[ "$code" = 200 ] || { echo "acme-cluster: authenticated write returned $code" >&2; exit 1; }

synced=0
for _ in $(seq 1 50); do
    a="$(curl -sS --max-time 2 http://127.0.0.1:19092/.well-known/acme-challenge/cluster-token_1)"
    b="$(curl -sS --max-time 2 http://127.0.0.1:19093/.well-known/acme-challenge/cluster-token_1)"
    if [ "$a" = "cluster-token_1.shared-proof" ] && [ "$b" = "cluster-token_1.shared-proof" ]; then
        synced=1
        break
    fi
    sleep 0.2
done
if [ "$synced" -ne 1 ]; then
    echo "acme-cluster: challenge was not identical on both nodes" >&2
    tail -30 "$TMP/a/server.log" >&2
    tail -30 "$TMP/b/server.log" >&2
    exit 1
fi

unauthorized="$(curl -k -sS -o /dev/null -w '%{http_code}' --max-time 5 \
    -H 'Content-Type: application/json' --data '{"token":"bad-token","response":"bad"}' \
    https://127.0.0.1:18446/flywire/acme-challenge)"
[ "$unauthorized" = 401 ] || { echo "acme-cluster: unauthorized write returned $unauthorized" >&2; exit 1; }

echo "acme-cluster: shared HTTP-01 response and peer authentication OK"
