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
root_url=https://oborona.zip
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
check /admin/moderation 302
check /flywire/anti-entropy 404

home="$(curl -s --max-time 5 "http://127.0.0.1:$PORT/")"
if [[ "$home" == *"<title>Smoke Test</title>"* && "$home" == *'rel="canonical" href="https://oborona.zip/"'* ]]; then
    echo "smoke: home title and canonical OK"
else
    echo "smoke: home title and canonical FAIL" >&2
    fail=1
fi
redirect="$(curl -s -o /dev/null -w '%{http_code} %{redirect_url}' --max-time 5 -H 'Host: ceversi.korokorok.com' "http://127.0.0.1:$PORT/post/example?x=1")"
if [ "$redirect" = "301 https://oborona.zip/post/example?x=1" ]; then
    echo "smoke: alias host canonical redirect OK"
else
    echo "smoke: alias host canonical redirect '$redirect' FAIL" >&2
    fail=1
fi

# Deferred responses must still be compressed: the compress middleware
# bails on res->deferred, and async_route used to leave the flag set.
ce="$(curl -s -D - -o /dev/null --max-time 5 -H 'Accept-Encoding: gzip' "http://127.0.0.1:$PORT/" | tr -d '\r' | awk 'BEGIN{IGNORECASE=1} /^Content-Encoding:/{print $2}')"
if [ "$ce" = "gzip" ]; then
    echo "smoke: GET / with Accept-Encoding -> Content-Encoding: gzip OK"
else
    echo "smoke: GET / with Accept-Encoding -> Content-Encoding: '$ce' (want gzip) FAIL" >&2
    fail=1
fi

# Privacy workflow: real authenticated routes, archive ownership, byte ranges,
# and immediate session invalidation after ghosting.
alice_input="aA1-$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')"
bob_input="bB2-$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')"
wrong_input="cC3-$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')"
curl -s -o /dev/null --max-time 5 -X POST --data "username=privacy-alice&email=alice%40example.com&password=$alice_input" "http://127.0.0.1:$PORT/register"
curl -s -c "$TMP/alice.cookies" -o /dev/null --max-time 5 -X POST --data "username=privacy-alice&password=$alice_input" "http://127.0.0.1:$PORT/login"
location="$(curl -s -b "$TMP/alice.cookies" -D - -o /dev/null --max-time 5 -X POST "http://127.0.0.1:$PORT/account/export" | tr -d '\r')"
export_id="$(printf '%s\n' "$location" | sed -n 's/.*export=\([a-f0-9]\{64\}\).*/\1/p')"
ready=0
if [ -n "$export_id" ]; then
    for _ in $(seq 1 50); do
        status="$(curl -s -b "$TMP/alice.cookies" --max-time 5 "http://127.0.0.1:$PORT/account/export/$export_id/status")"
        if [[ "$status" == *'"state":"ready"'* ]]; then ready=1; break; fi
        sleep 0.1
    done
fi
if [ "$ready" -eq 1 ]; then
    echo "smoke: authenticated asynchronous export OK"
else
    echo "smoke: authenticated asynchronous export FAIL" >&2
    fail=1
fi
if [ "$ready" -eq 1 ]; then
    archive_code="$(curl -s -b "$TMP/alice.cookies" -o "$TMP/export.tar.zst" -w '%{http_code}' --max-time 10 "http://127.0.0.1:$PORT/account/export/$export_id/download")"
    range_code="$(curl -s -b "$TMP/alice.cookies" -o "$TMP/export.range" -w '%{http_code}' --max-time 10 -H 'Range: bytes=0-15' "http://127.0.0.1:$PORT/account/export/$export_id/download")"
    dd if="$TMP/export.tar.zst" of="$TMP/export.first" bs=1 count=16 status=none
    if [ "$archive_code" = 200 ] && [ "$range_code" = 206 ] && cmp -s "$TMP/export.range" "$TMP/export.first"; then
        echo "smoke: export download and resumable range OK"
    else
        echo "smoke: export download and resumable range FAIL (full=$archive_code range=$range_code)" >&2
        fail=1
    fi
fi
curl -s -o /dev/null --max-time 5 -X POST --data "username=privacy-bob&email=bob%40example.com&password=$bob_input" "http://127.0.0.1:$PORT/register"
curl -s -c "$TMP/bob.cookies" -o /dev/null --max-time 5 -X POST --data "username=privacy-bob&password=$bob_input" "http://127.0.0.1:$PORT/login"
other_code="$(curl -s -b "$TMP/bob.cookies" -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:$PORT/account/export/$export_id/status")"
if [ "$other_code" = 404 ]; then
    echo "smoke: cross-account export authorization OK"
else
    echo "smoke: cross-account export authorization FAIL ($other_code)" >&2
    fail=1
fi
curl -s -b "$TMP/alice.cookies" -o /dev/null --max-time 5 -X POST --data "password=$wrong_input&confirmation=DELETE" "http://127.0.0.1:$PORT/account/delete"
still_valid="$(curl -s -b "$TMP/alice.cookies" -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:$PORT/account/export/$export_id/status")"
if [ "$still_valid" = 200 ]; then
    echo "smoke: deletion rejects wrong password OK"
else
    echo "smoke: deletion rejects wrong password FAIL ($still_valid)" >&2
    fail=1
fi
curl -s -b "$TMP/alice.cookies" -o /dev/null --max-time 5 -X POST --data "password=$alice_input&confirmation=DELETE" "http://127.0.0.1:$PORT/account/delete"
ghost_code="$(curl -s -b "$TMP/alice.cookies" -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:$PORT/account/export/$export_id/status")"
if [ "$ghost_code" = 302 ]; then
    echo "smoke: account ghosting invalidates session and export OK"
else
    echo "smoke: account ghosting invalidates session and export FAIL ($ghost_code)" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "smoke: FAILED" >&2
    exit 1
fi
echo "smoke: all checks passed"
