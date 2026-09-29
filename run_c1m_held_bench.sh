#!/bin/bash

# True C1M benchmark for fly_board: 1,000,000 simultaneously held
# connections (the classic C10K/C1M meaning), unlike run_c1m_bench.sh which
# measures 1,000,000 requests over 100,000 held connections.
#
# h2load costs ~60 KB of client memory per connection, so a same-host
# 1M run needs a lighter client: tools/connhold keeps one small slot per
# connection, sends one GET, then re-sends it every PING seconds so the
# server keep-alive timer never fires. Nothing is retried; a connection the
# server closes stays closed and is counted.
#
# Three runs, each against a fresh server:
#   plain  HTTP/1.1 cleartext (cwist event-driven reactor path)
#   tls    TLS 1.3 + HTTP/1.1 (ALPN http/1.1) on the HTTPS path
#   h2     TLS 1.3 + HTTP/2 (ALPN h2), one new stream per keep-alive ping
# The TLS runs raise the idle budgets above PING (CWIST_HTTPS_IDLE_TIMEOUT_MS
# for HTTP/1.1; HTTP/2's default idle timeout is already 300 s).
#
# Linux connect() hands out even ephemeral ports first and falls back to a
# slow odd-port search once they run out, so each destination VIP gives
# ~32k fast connections (half of 1024-65535). VIPS must be at least
# CONNS / 32000; with 24 VIPs every run stalled near 774k.

set -euo pipefail

SERVER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKERS=12
CONNS=1000000
VIPS=48
PROCS=8
PLAIN_PORT=18080
TLS_PORT=8888
PLAIN_RATE=40000   # new connections per second
TLS_RATE=8000     # full TLS handshakes per second; at 20000/s ~37% missed the
                  # 45 s handshake budget, at 8000/s none did
PING=120           # seconds between keep-alive GETs per connection
HOLD=120           # seconds to hold after the ramp finishes
MEM_GUARD_MIB=6144 # stop opening connections below this MemAvailable
RESULTS="${SERVER_DIR}/benchmark_c1m_held.results"
WORK="$(mktemp -d /tmp/flyboard_c1m_held.XXXXXX)"

log() { printf '[%s] %s\n' "$(date +%T)" "$1"; }

check_prerequisites() {
    ulimit -n 1050000
    local ct fmax
    ct=$(cat /proc/sys/net/netfilter/nf_conntrack_max 2>/dev/null || echo 0)
    fmax=$(cat /proc/sys/fs/file-max)
    if [[ ${ct} -gt 0 && ${ct} -lt 2200000 ]]; then
        log "nf_conntrack_max=${ct} caps loopback connections; raise it first:"
        log "  sudo sysctl -w net.netfilter.nf_conntrack_max=4194304"
        exit 1
    fi
    if [[ ${fmax} -lt 4200000 ]]; then
        log "fs.file-max=${fmax}: 1M connections need 2M fds (client + server); raise it first:"
        log "  sudo sysctl -w fs.file-max=8388608"
        exit 1
    fi
    make -C "${SERVER_DIR}" -s tools/connhold
}

wait_sockets_drained() {
    # Leftover TIME_WAIT sockets and conntrack entries from a previous run
    # skew the next one; wait until the stack is quiet.
    until [[ $(cat /proc/sys/net/netfilter/nf_conntrack_count 2>/dev/null || echo 0) -lt 20000 &&
             $(ss -Htn state time-wait | wc -l) -lt 20000 ]]; do
        sleep 5
    done
}

stop_server() {
    pkill -TERM -x fly_board 2>/dev/null || true
    for _ in $(seq 240); do pgrep -x fly_board >/dev/null || return 0; sleep 0.5; done
    pkill -9 -x fly_board 2>/dev/null || true
}

server_pss_kib() {
    local total=0 v
    for p in $(pgrep -x fly_board); do
        v=$(awk '/^Pss:/{print $2}' "/proc/${p}/smaps_rollup" 2>/dev/null || echo 0)
        total=$((total + ${v:-0}))
    done
    echo "${total}"
}

run_one() {
    local mode=$1 port=$2 rate=$3 tls_flag=$4
    stop_server
    wait_sockets_drained
    log "${mode}: starting server (${WORKERS} workers, port ${port})"
    if [[ ${mode} == plain ]]; then
        mkdir -p "${WORK}/plain/data"
        for d in public posts legal img; do ln -sfn "${SERVER_DIR}/${d}" "${WORK}/plain/${d}"; done
        printf 'title=C1M held\nport=%s\nuse_tls=false\nuse_http2=false\nuse_http3=false\nuse_tasfa=false\nuse_rss=false\n' \
            "${port}" > "${WORK}/plain/blog.settings"
        (cd "${WORK}/plain" && CWIST_WORKERS=${WORKERS} CWIST_HTTP_KEEP_ALIVE_TIMEOUT=300 \
            setsid "${SERVER_DIR}/fly_board" > "${WORK}/${mode}_server.log" 2>&1 &)
        until curl -sf -o /dev/null "http://127.0.0.1:${port}/robots.txt"; do sleep 0.5; done
    else
        (cd "${SERVER_DIR}" && CWIST_WORKERS=${WORKERS} CWIST_HTTPS_IDLE_TIMEOUT_MS=300000 \
            setsid ./fly_board > "${WORK}/${mode}_server.log" 2>&1 &)
        until curl -sk -o /dev/null "https://127.0.0.1:${port}/robots.txt"; do sleep 0.5; done
    fi

    # Sample server PSS (all worker processes) at the peak of held
    # connections. The file is rewritten at every new peak, so killing the
    # sampler when connhold exits never loses the measurement.
    ( peak=0
      while :; do
          e=$(ss -Htn state established "( sport = :${port} )" | wc -l)
          if [[ ${e} -gt ${peak} ]]; then
              peak=${e}
              echo "peak_server_established=${peak} server_pss_kB=$(server_pss_kib)" \
                  > "${WORK}/${mode}_mem.log"
          fi
          sleep 3
      done ) &
    local sampler=$!

    log "${mode}: connhold ${CONNS} connections at ${rate}/s over ${VIPS} VIPs"
    "${SERVER_DIR}/tools/connhold" ${tls_flag} --conns "${CONNS}" --procs "${PROCS}" \
        --rate "${rate}" --vips "${VIPS}" --port "${port}" --ping "${PING}" --hold "${HOLD}" \
        --mem-guard "${MEM_GUARD_MIB}" --path /robots.txt > "${WORK}/${mode}.log" 2>&1 || true
    kill "${sampler}" 2>/dev/null || true
    wait "${sampler}" 2>/dev/null || true
    stop_server

    {
        echo "== ${mode} (port ${port}, ${WORKERS} workers, ${rate} conn/s, ${VIPS} VIPs, ping ${PING}s, cwist $(git -C "${CWIST_ROOT:-${SERVER_DIR}/../cwist}" rev-parse --short HEAD 2>/dev/null))"
        tail -2 "${WORK}/${mode}.log"
        cat "${WORK}/${mode}_mem.log" 2>/dev/null || true
        echo
    } >> "${RESULTS}"
}

main() {
    trap stop_server EXIT
    check_prerequisites
    {
        echo "C1M Held-Connection Benchmark - $(date)"
        echo "live = connections open and past TLS (if any); served = got at least one full"
        echo "response; responses = total responses including keep-alive pings."
        echo
    } > "${RESULTS}"
    run_one plain "${PLAIN_PORT}" "${PLAIN_RATE}" ""
    run_one tls "${TLS_PORT}" "${TLS_RATE}" "--tls"
    run_one h2 "${TLS_PORT}" "${TLS_RATE}" "--h2"
    log "results saved to ${RESULTS}"
    rm -rf "${WORK}"
}

main "$@"
