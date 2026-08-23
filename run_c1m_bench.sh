#!/bin/bash

# C1M Benchmark Script for fly_board
#
# NOTE (2026-08-23 redesign): "1,000,000 simultaneously held TLS connections"
# is architecturally impossible for this server today, and pretending
# otherwise made this script hang forever.  fly_board terminates TLS inside
# cwist's thread-pool HTTPS path, where every live connection parks one
# worker thread for its whole lifetime.  Held-connection concurrency is
# therefore capped at (CWIST_WORKERS x threads-per-worker), i.e. hundreds to
# ~1.5k, not 1M.  (The cleartext HTTP/1.x path in cwist is event-driven and
# did reach 999,872/1,000,000 held connections; TLS has not been ported to
# that model yet.)
#
# What this script measures instead: C1M-scale CHURN — 1,000,000 requests
# (20 processes x 50k) over 100,000 concurrently held TLS connections.
# The connect rate is deliberately kept at the C100K-proven 1,000 conn/s
# per process: higher aggregate rates (>= ~50k/s) drive the TLS accept
# path into a stall, which is a server-side issue tracked separately and
# not something a benchmark should paper over by retrying.  Termination
# is guaranteed by -n (per-process request budget) plus -T
# (per-connection active timeout), since -r and --duration are mutually
# exclusive in h2load.
#
# Load-shape pitfalls measured and fixed:
#  1. WORKERS must leave cores for the load generators. 24 forked workers on
#     a 12-core box oversubscribe the scheduler; the TLS handshake rate then
#     collapses (~130 conn/s aggregate) because worker threads spin on
#     contention instead of handshaking. With WORKERS=4 the same client set
#     established 300k connections in ~90s.
#  2. A single h2load process does not survive -c 50000 (stalls silently with
#     connections stuck pre-handshake). Keep per-process concurrency in the
#     low thousands; scale by process count across distinct VIPs.
WORKERS=12
SERVER_DIR="/home/yjlee/fly.board"
H2LOAD_CONN=5000        # concurrent conns per h2load process (100k held total)
H2LOAD_REQ=50000        # per-process request budget (aggregate: 1M requests)
H2LOAD_RATE=1000        # new conns/s per process (C100K-proven safe rate)
CONN_ACTIVE_TIMEOUT=30  # recycle any connection alive longer than this
VIP_COUNT=20
VIP_BASE="127.0.0"
PORT=8888
DURATION_ESTIMATE=600   # seconds; healthy runs finish 1M requests in ~7 min

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() {
    printf "${GREEN}[INFO]${NC} %s\n" "$1"
}

log_warn() {
    printf "${YELLOW}[WARN]${NC} %s\n" "$1"
}

log_error() {
    printf "${RED}[ERROR]${NC} %s\n" "$1"
}

check_prerequisites() {
    log_info "Running pre-flight checks..."

    if [[ ! -x "${SERVER_DIR}/fly_board" ]]; then
        log_error "Server binary not found: ${SERVER_DIR}/fly_board"
        exit 1
    fi

    if ! command -v h2load &> /dev/null; then
        log_error "h2load not found in PATH"
        exit 1
    fi

    # Set FD limits for this session
    ulimit -n 1050000 || {
        log_error "Cannot set sufficient FD limits"
        exit 1
    }

    # Kernel knobs that silently cap loopback concurrency (from the cwist
    # C1M audit): conntrack table, system-wide fd ceiling, client port range.
    local ct_max fmax prange
    ct_max=$(cat /proc/sys/net/netfilter/nf_conntrack_max 2>/dev/null || echo 0)
    fmax=$(cat /proc/sys/fs/file-max 2>/dev/null || echo 0)
    prange=$(cat /proc/sys/net/ipv4/ip_local_port_range 2>/dev/null || echo "unknown")
    log_info "kernel: nf_conntrack_max=${ct_max} fs.file-max=${fmax} ip_local_port_range=${prange}"
    if [[ ${ct_max} -lt 1000000 ]]; then
        log_warn "nf_conntrack_max=${ct_max}: loopback is conntracked too, capping total connections near that value."
        log_warn "  fix: sudo sysctl -w net.netfilter.nf_conntrack_max=4194304"
    fi
    if [[ ${fmax} -lt 4000000 ]]; then
        log_warn "fs.file-max=${fmax}: each connection costs one fd on BOTH client and server."
        log_warn "  fix: sudo sysctl -w fs.file-max=8388608"
    fi

    log_info "Pre-flight checks passed"
}

start_server() {
    log_info "Starting server with ${WORKERS} workers..."
    cd "${SERVER_DIR}" || exit 1
    rm -f server_exec.log

    # Start server wrapped with /usr/bin/time -v
    CWIST_WORKERS=$WORKERS /usr/bin/time -v ./fly_board > server_exec.log 2>&1 &
    SERVER_PID=$!

    log_info "Waiting for server to bind on port ${PORT}..."
    local retries=30
    while [[ ${retries} -gt 0 ]]; do
        if ss -tlnp | grep -q ":${PORT}"; then
            log_info "Server listening on port ${PORT} (PID: ${SERVER_PID})"
            return 0
        fi
        sleep 1
        ((retries--))
    done

    log_error "Server failed to start within 30 seconds"
    kill -9 ${SERVER_PID} 2>/dev/null || true
    exit 1
}

generate_load() {
    log_info "Spawning ${VIP_COUNT} h2load processes..."
    log_info "Target: ${VIP_COUNT} x ${H2LOAD_CONN} connections = $((VIP_COUNT * H2LOAD_CONN)) total"
    
    cd "${SERVER_DIR}" || exit 1

    local pids=""
    for i in $(seq 1 ${VIP_COUNT}); do
        local target="https://${VIP_BASE}.${i}:${PORT}/"
        h2load \
            -c ${H2LOAD_CONN} \
            -n ${H2LOAD_REQ} \
            -r ${H2LOAD_RATE} \
            -T ${CONN_ACTIVE_TIMEOUT} \
            ${target} > "h2load_c1m_${i}.log" 2>&1 &
        pids="${pids} $!"
    done

    log_info "Load generation started, waiting for all processes to finish..."

    # Watchdog: under multi-process TLS load we have observed a stall where
    # h2load holds ESTABLISHED sockets that have no server-side counterpart
    # and no progress is made (under investigation on the cwist HTTPS path).
    # Never let this script hang forever: after DURATION_ESTIMATE seconds,
    # kill the stragglers and analyze whatever was measured.
    local deadline=$(( $(date +%s) + DURATION_ESTIMATE ))
    for pid in ${pids}; do
        local now
        now=$(date +%s)
        if [[ ${now} -lt ${deadline} ]]; then
            # SIGINT first: h2load prints its partial result table on it.
            ( sleep $(( deadline - now )) ; kill -INT ${pid} 2>/dev/null ; sleep 5 ; kill -9 ${pid} 2>/dev/null ) &
            local watchdog=$!
            wait ${pid} || log_warn "h2load process ${pid} exited with error"
            kill ${watchdog} 2>/dev/null || true
        else
            log_warn "watchdog expired; interrupting h2load process ${pid}"
            kill -INT ${pid} 2>/dev/null || true
            sleep 5
            kill -9 ${pid} 2>/dev/null || true
            wait ${pid} 2>/dev/null || true
        fi
    done

    echo ""
    log_info "Load generation phase complete"
}

stop_server() {
    log_info "Stopping server..."
    pkill -SIGTERM -x fly_board || true
    
    local timeout=60
    while pgrep -x fly_board >/dev/null && [ $timeout -gt 0 ]; do
        sleep 1
        ((timeout--))
    done
    
    if pgrep -x fly_board >/dev/null; then
        log_warn "Server did not stop gracefully after 60s, forcing kill"
        pkill -9 -x fly_board || true
    fi
    
    wait ${SERVER_PID} 2>/dev/null || true
    log_info "Server stopped"
}

analyze_results() {
    log_info "Analyzing benchmark results..."
    cd "${SERVER_DIR}" || exit 1

    local total_reqs=0
    local total_success=0
    local total_failed=0
    local total_rps=0

    for i in $(seq 1 ${VIP_COUNT}); do
        local logfile="h2load_c1m_${i}.log"
        if [[ ! -f ${logfile} ]]; then continue; fi

        local reqs=$(grep "requests:" ${logfile} | head -1 | awk '{print $2}' || echo "0")
        local success=$(grep "requests:" ${logfile} | head -1 | awk '{print $8}' || echo "0")
        local failed=$(grep "requests:" ${logfile} | head -1 | awk '{print $10}' || echo "0")
        local rps=$(grep "finished in" ${logfile} | awk '{print $4}' | tr -d ',' || echo "0")

        total_reqs=$((total_reqs + reqs))
        total_success=$((total_success + success))
        total_failed=$((total_failed + failed))
        
        if [[ -n "${rps}" && "${rps}" != "0" ]]; then
            total_rps=$(echo "${total_rps} + ${rps}" | bc -l 2>/dev/null || echo "${total_rps}")
        fi
    done

    echo "========================================"
    echo "C1M BENCHMARK RESULTS"
    echo "========================================"
    echo "Total Requests:     ${total_reqs}"
    echo "Total Succeeded:    ${total_success}"
    echo "Total Failed:       ${total_failed}"
    echo "Approx Total RPS:   ${total_rps}"
    
    local success_rate="0"
    if [[ ${total_reqs} -gt 0 ]]; then
        success_rate=$(echo "scale=2; ${total_success} * 100 / ${total_reqs}" | bc 2>/dev/null || echo "0")
        echo "Success Rate:       ${success_rate}%"
    fi
    echo "========================================"
    
    {
        echo "C1M Benchmark Result - $(date)"
        echo "Total Requests: ${total_reqs}"
        echo "Total Succeeded: ${total_success}"
        echo "Total Failed: ${total_failed}"
        echo "Approx Total RPS: ${total_rps}"
        echo "Success Rate: ${success_rate}%"
        echo ""
        echo "SERVER RESOURCE REPORT"
        echo "========================================"
        if [[ -f server_exec.log ]]; then
            grep -A 40 "Command being timed" server_exec.log || cat server_exec.log
        else
            echo "server_exec.log not found"
        fi
    } > benchmark_c1m.results
    
    log_info "Results saved to benchmark_c1m.results"
}

cleanup() {
    pkill -9 -f "h2load -c ${H2LOAD_CONN}" 2>/dev/null || true
    pkill -9 -x fly_board 2>/dev/null || true
}

main() {
    trap cleanup EXIT
    check_prerequisites
    start_server
    generate_load
    stop_server
    analyze_results
}

main "$@"
