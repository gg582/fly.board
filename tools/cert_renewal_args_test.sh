#!/usr/bin/env bash
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d /tmp/flyboard_cert_args.XXXXXX)"
PID=""
CURRENT=""

cleanup() {
    if [ -n "$PID" ]; then
        kill -TERM -- -"$PID" 2>/dev/null
        sleep 1
        kill -KILL -- -"$PID" 2>/dev/null
        wait "$PID" 2>/dev/null
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

make_cert() {
    dir="$1"
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$dir/ca.key" -out "$dir/ca.crt" -days 2 -subj /CN=Test-CA >/dev/null 2>&1
    openssl req -newkey rsa:2048 -nodes \
        -keyout "$dir/site.key" -out "$dir/site.csr" -subj /CN=test.local >/dev/null 2>&1
    printf 'subjectAltName=DNS:test.local\n' > "$dir/site.ext"
    openssl x509 -req -in "$dir/site.csr" -CA "$dir/ca.crt" -CAkey "$dir/ca.key" \
        -CAcreateserial -out "$dir/site.crt" -days 1 -extfile "$dir/site.ext" >/dev/null 2>&1
}

run_case() {
    mode="$1"
    expected="$2"
    port="$3"
    acme_port="$4"
    dir="$TMP/$mode"
    mkdir -p "$dir/data"
    CURRENT="$dir"
    ln -s "$ROOT/public" "$dir/public"
    ln -s "$ROOT/posts" "$dir/posts"
    ln -s "$ROOT/legal" "$dir/legal"
    ln -s "$ROOT/img" "$dir/img"
    make_cert "$dir"
    cp "$dir/site.crt" "$dir/server.crt"
    cp "$dir/site.key" "$dir/server.key"
    cat > "$dir/blog.settings" <<EOF
title=Cert Args $mode
root_url=https://test.local:$port
port=$port
use_tls=true
use_http2=false
use_http3=false
use_tasfa=false
use_rss=false
EOF
    cat > "$dir/fake-lego" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > args.txt
if [ "$1" = run ]; then
    echo v5 > mode.txt
elif [ "$FAKE_MODE" = v5 ]; then
    exit 1
else
    echo legacy > mode.txt
fi
mkdir -p .lego/certificates
cp site.crt .lego/certificates/test.local.crt
cp site.key .lego/certificates/test.local.key
exit 0
EOF
    chmod +x "$dir/fake-lego"
    cd "$dir" || exit 1
    FAKE_MODE="$mode" FLY_CERT_RENEWAL=true FLY_CERT_HTTP_PORT="$acme_port" \
        FLY_CERT_LEGO_BIN="$dir/fake-lego" ASAN_OPTIONS="detect_leaks=0:abort_on_error=1" \
        setsid "$ROOT/fly_board" > server.log 2>&1 &
    PID=$!
    for _ in $(seq 1 50); do
        [ -f mode.txt ] && break
        if ! kill -0 "$PID" 2>/dev/null; then
            echo "cert-args: $mode server exited" >&2
            cat server.log >&2
            exit 1
        fi
        sleep 0.2
    done
    actual="$(cat mode.txt 2>/dev/null)"
    if [ "$actual" != "$expected" ]; then
        echo "cert-args: $mode selected '$actual', expected '$expected'" >&2
        cat server.log >&2
        exit 1
    fi
    if [ ! -f .lego/webroot/.well-known/acme-challenge ] && [ ! -d .lego/webroot/.well-known/acme-challenge ]; then
        echo "cert-args: $mode webroot was not created" >&2
        exit 1
    fi
    case "$(cat args.txt)" in
        *--http.webroot*--http.delay*) ;;
        *) echo "cert-args: $mode missing webroot/delay" >&2; exit 1;;
    esac
    if [ "$mode" = v5 ] && ! grep -q -- '--renew-days' args.txt; then
        echo "cert-args: v5 renewal threshold missing" >&2
        exit 1
    fi
    if [ "$(stat -c %a server.key)" != 600 ]; then
        echo "cert-args: $mode installed an insecure private-key mode" >&2
        exit 1
    fi
    echo "cert-args: $mode invocation and certificate installation OK"
    kill -TERM -- -"$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
    PID=""
}

run_case legacy legacy 18447 19094
run_case v5 v5 18448 19095
