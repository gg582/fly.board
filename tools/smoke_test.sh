#!/usr/bin/env bash
# Smoke test: boot fly_board with a throwaway config in a temp directory
# (so the real data/ and *.settings files are never touched), then check
# a few endpoints over plain HTTP.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${SMOKE_PORT:-18080}"
TMP="$(mktemp -d /tmp/flyboard_smoke.XXXXXX)"
PID=""

cleanup() {
    # cwist_app_listen() forks worker processes; kill the whole process
    # group so no orphaned workers keep the port.
    [ -n "$PID" ] && kill -TERM -- -"$PID" 2>/dev/null
    sleep 1
    [ -n "$PID" ] && kill -KILL -- -"$PID" 2>/dev/null
    [ -n "$PID" ] && wait "$PID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

# Assets must live next to the binary's working dir; symlink the read-only
# trees and give the server its own scratch data/ + settings.
ln -s "$ROOT/public" "$TMP/public"
ln -s "$ROOT/posts" "$TMP/posts"
ln -s "$ROOT/legal" "$TMP/legal"
ln -s "$ROOT/img" "$TMP/img"
mkdir -p "$TMP/data"

cat > "$TMP/blog.settings" <<EOF
title=Smoke Test
port=$PORT
use_tls=false
use_http2=false
use_http3=false
use_tasfa=false
use_rss=false
EOF

cd "$TMP" || exit 1
setsid "$ROOT/fly_board" > server.log 2>&1 &
PID=$!

# Wait for readiness (bounded so CI cannot hang).
ready=0
for _ in $(seq 1 50); do
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "smoke: server exited during startup; log follows" >&2
        cat server.log >&2
        exit 1
    fi
    if curl -sf -o /dev/null --max-time 2 "http://127.0.0.1:$PORT/"; then
        ready=1
        break
    fi
    sleep 0.3
done
if [ "$ready" -ne 1 ]; then
    echo "smoke: server did not become ready on port $PORT" >&2
    cat server.log >&2
    exit 1
fi

fail=0
check() {
    path="$1"; want="$2"
    got="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:$PORT$path")"
    if [ "$got" = "$want" ]; then
        echo "smoke: GET $path -> $got OK"
    else
        echo "smoke: GET $path -> $got (want $want) FAIL" >&2
        fail=1
    fi
}

check / 200
check /robots.txt 200
check /login 200
check /boards 200

if [ "$fail" -ne 0 ]; then
    echo "smoke: FAILED" >&2
    exit 1
fi
echo "smoke: all checks passed"
