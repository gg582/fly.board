#!/usr/bin/env bash
# ASan/LSan leak-detection stress test for CI.
#
# Boots an ASan-instrumented fly_board with a throwaway config (same
# pattern as tools/smoke_test.sh), fires a burst of requests at the main
# read endpoints to exercise allocation/free paths, then asks the server to
# shut down gracefully and waits for it to exit on its own so LeakSanitizer
# gets to run its end-of-process leak scan and print a report before the
# process is gone. Fails if AddressSanitizer/LeakSanitizer reported
# anything, or if the server had to be killed instead of exiting cleanly.
#
# Expects an ASan-instrumented ./fly_board binary in the repo root (build
# with e.g. `make EXTRA_CFLAGS="-fsanitize=address -fno-omit-frame-pointer -O0 -g"`).
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${ASAN_STRESS_PORT:-18089}"
REQUESTS="${ASAN_STRESS_REQUESTS:-2000}"
CONCURRENCY="${ASAN_STRESS_CONCURRENCY:-8}"
SHUTDOWN_TIMEOUT="${ASAN_STRESS_SHUTDOWN_TIMEOUT:-30}"
TMP="$(mktemp -d /tmp/flyboard_asan.XXXXXX)"
PID=""
EXITED_CLEANLY=0

cleanup() {
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        echo "asan-stress: server still running during cleanup, force killing" >&2
        kill -KILL -- -"$PID" 2>/dev/null
        wait "$PID" 2>/dev/null
    fi
    if [ "${KEEP_TMP:-0}" != "1" ]; then
        rm -rf "$TMP"
    else
        echo "asan-stress: keeping scratch dir $TMP (KEEP_TMP=1)" >&2
    fi
}
trap cleanup EXIT

ln -s "$ROOT/public" "$TMP/public"
ln -s "$ROOT/posts" "$TMP/posts"
ln -s "$ROOT/legal" "$TMP/legal"
ln -s "$ROOT/img" "$TMP/img"
mkdir -p "$TMP/data"

cat > "$TMP/blog.settings" <<EOF
title=ASan Stress Test
port=$PORT
use_tls=false
use_http2=false
use_http3=false
use_tasfa=false
use_rss=false
EOF

cd "$TMP" || exit 1

# detect_leaks=1 is the Linux default already, set explicitly for clarity.
# abort_on_error=0 + exitcode=1 so a detected leak/error produces a normal
# non-zero exit this script can observe instead of a core dump.
export ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:exitcode=1:${ASAN_OPTIONS:-}"

setsid "$ROOT/fly_board" > server.log 2>&1 &
PID=$!

ready=0
for _ in $(seq 1 50); do
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "asan-stress: server exited during startup; log follows" >&2
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
    echo "asan-stress: server did not become ready on port $PORT" >&2
    cat server.log >&2
    exit 1
fi

echo "asan-stress: firing $REQUESTS requests (concurrency $CONCURRENCY) at read endpoints"
# xargs runs its -I{} command through /bin/sh, which on many systems (e.g.
# Debian/Ubuntu, Arch) is dash and doesn't support bash array syntax -
# explicitly invoke bash per request instead of relying on the default sh.
seq 1 "$REQUESTS" | xargs -P "$CONCURRENCY" -I{} bash -c '
    paths=(/ /robots.txt /login /register /boards)
    p=${paths[$(( RANDOM % ${#paths[@]} ))]}
    curl -s -o /dev/null --max-time 5 "http://127.0.0.1:'"$PORT"'$p"
' 2>/dev/null

if ! kill -0 "$PID" 2>/dev/null; then
    echo "asan-stress: server died under load; log follows" >&2
    cat server.log >&2
    exit 1
fi

echo "asan-stress: load done, requesting graceful shutdown (SIGTERM)"
kill -TERM -- -"$PID" 2>/dev/null

for _ in $(seq 1 "$SHUTDOWN_TIMEOUT"); do
    if ! kill -0 "$PID" 2>/dev/null; then
        EXITED_CLEANLY=1
        break
    fi
    sleep 1
done

if [ "$EXITED_CLEANLY" -ne 1 ]; then
    # Force it down so `wait` below can't block forever - a real leak can
    # wedge a worker badly enough that SIGTERM alone never lands cleanly,
    # and this check must still terminate and report FAIL, not hang the CI
    # job until it's killed by the runner's own timeout.
    echo "asan-stress: server still alive ${SHUTDOWN_TIMEOUT}s after SIGTERM, force killing" >&2
    kill -KILL -- -"$PID" 2>/dev/null
fi

wait "$PID" 2>/dev/null
STATUS=$?

echo "asan-stress: server log:"
cat server.log

if [ "$EXITED_CLEANLY" -ne 1 ]; then
    echo "asan-stress: FAIL - server did not exit within ${SHUTDOWN_TIMEOUT}s of SIGTERM" \
         "(LeakSanitizer never got to run its end-of-process scan)" >&2
    exit 1
fi

if [ "$STATUS" -ne 0 ]; then
    echo "asan-stress: FAIL - server exited with status $STATUS" >&2
    exit 1
fi

if grep -qE "ERROR: (Leak|Address)Sanitizer|SUMMARY: AddressSanitizer" server.log; then
    echo "asan-stress: FAIL - AddressSanitizer/LeakSanitizer reported an issue (see log above)" >&2
    exit 1
fi

echo "asan-stress: PASS - clean shutdown, no ASan/LSan reports"
