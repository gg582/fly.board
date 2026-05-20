#!/bin/bash

# C100K Benchmark Script for fly_board
# Targeted for 100,000 concurrent connections

set -euo pipefail

# Configuration
WORKERS=12
SERVER_DIR="/home/yjlee/fly.board"
H2LOAD_CONN=5000
H2LOAD_REQ=10000
H2LOAD_RATE=1000
VIP_COUNT=20
VIP_BASE="127.0.0"
PORT=8888
DURATION_ESTIMATE=60  # seconds

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

    log_info "Pre-flight checks passed"
}

start_server() {
    log_info "Starting server with ${WORKERS} workers..."
    cd "${SERVER_DIR}" || exit 1
    rm -f server_exec.log

    # Start server wrapped with /usr/bin/time -v
    # Using stdbuf to avoid buffering issues
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
            ${target} > "h2load_c100k_${i}.log" 2>&1 &
        pids="${pids} $!"
    done

    log_info "Load generation started, waiting for all processes to finish..."
    
    # Wait for all h2load processes
    for pid in ${pids}; do
        wait ${pid} || log_warn "h2load process ${pid} exited with error"
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
        local logfile="h2load_c100k_${i}.log"
        if [[ ! -f ${logfile} ]]; then continue; fi

        # Correct fields for h2load 1.43.0+
        # 2: requests, 8: succeeded, 10: failed, 12: errored, 14: timeout
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
    echo "C100K BENCHMARK RESULTS"
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
    
    # Also output to a file
    {
        echo "C100K Benchmark Result - $(date)"
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
    } > benchmark_c100k.results
    
    log_info "Results saved to benchmark_c100k.results"
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
