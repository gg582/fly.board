#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${REALTIME_SMOKE_PORT:-18081}"
TMP="$(mktemp -d /tmp/flyboard_realtime.XXXXXX)"
PID=""
cleanup() {
    if [ -n "$PID" ]; then
        kill -TERM -- -"$PID" 2>/dev/null || true
        sleep 1
        kill -KILL -- -"$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

ln -s "$ROOT/public" "$TMP/public"
ln -s "$ROOT/posts" "$TMP/posts"
ln -s "$ROOT/legal" "$TMP/legal"
ln -s "$ROOT/img" "$TMP/img"
mkdir -p "$TMP/data"
cat > "$TMP/blog.settings" <<EOF
title=Realtime Smoke
root_url=http://127.0.0.1:$PORT
port=$PORT
use_tls=false
use_http2=false
use_http3=false
use_tasfa=false
use_rss=false
realtime_enabled=true
realtime_typing=true
EOF

cd "$TMP"
NATS_URL=nats://127.0.0.1:1 setsid "$ROOT/fly_board" > server.log 2>&1 &
PID=$!
for _ in $(seq 1 80); do
    if curl -sf -o /dev/null --max-time 2 "http://127.0.0.1:$PORT/"; then break; fi
    if ! kill -0 "$PID" 2>/dev/null; then cat server.log; exit 1; fi
    sleep 0.25
done
curl -sf -o /dev/null --max-time 2 "http://127.0.0.1:$PORT/"

curl -sf -D post.headers -o /dev/null --max-time 5 \
    --data 'title=Realtime+Smoke&content=body&post_action=publish' \
    "http://127.0.0.1:$PORT/post/new"
POST_PATH="$(awk 'tolower($1)=="location:" {gsub("\r", "", $2); print $2}' post.headers | tail -1)"
POST_PATH="${POST_PATH%%\?*}"
[ "$POST_PATH" = "/post/realtime-smoke" ]
PAGE="$(curl -sf --max-time 5 "http://127.0.0.1:$PORT$POST_PATH")"
[[ "$PAGE" == *"data-realtime-post='1'"* ]]
[[ "$PAGE" == *"<form action='/comment/new' method='post'"* ]]

SESSION=0123456789abcdef0123456789abcdef
EVENTS="$TMP/events"
curl -sf --max-time 5 \
    "http://127.0.0.1:$PORT/realtime/events?post_id=1&session=$SESSION&after=0" > "$EVENTS"
grep -q '^event:state' "$EVENTS"
grep -q '^retry:2000' "$EVENTS"
grep -q '"transport":"local"' "$EVENTS"
! grep -q '^event:comment' "$EVENTS"

curl -sf -o /dev/null --max-time 5 \
    --data 'target_type=post&target_id=1&parent_id=0&content=hello+realtime&author_name=Guest' \
    "http://127.0.0.1:$PORT/comment/new"
curl -sf --max-time 5 \
    "http://127.0.0.1:$PORT/realtime/events?post_id=1&session=$SESSION&after=0" > "$EVENTS"
grep -q '^event:comment' "$EVENTS"
grep -q '^id:1' "$EVENTS"
curl -sf --max-time 5 -H 'Last-Event-ID: 1' \
    "http://127.0.0.1:$PORT/realtime/events?post_id=1&session=$SESSION&after=0" > "$EVENTS"
! grep -q '^event:comment' "$EVENTS"
curl -sf --max-time 5 "http://127.0.0.1:$PORT/realtime/comments/1" | grep -q 'hello realtime'

code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
    --data "post_id=1&session=$SESSION&active=1" "http://127.0.0.1:$PORT/realtime/presence")"
[ "$code" = 204 ]
code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
    --data "post_id=1&session=$SESSION&active=1" "http://127.0.0.1:$PORT/realtime/typing")"
[ "$code" = 204 ]
code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
    --data "post_id=1&session=$SESSION&active=1" "http://127.0.0.1:$PORT/realtime/typing")"
[ "$code" = 429 ]

echo "realtime HTTP, outage, cursor reconnect, and typing tests passed"
