#!/usr/bin/env bash
# Convenience runner for fly.board chaos test sequences.
# Each sequence applies an impairment, runs a short HTTP load test, and resets.
#
# Usage:
#   sudo ./tools/chaos_runner.sh <interface> [scenario]
#   sudo ./tools/chaos_runner.sh eth0 submarine_cable_cut
#   sudo ./tools/chaos_runner.sh lo all

set -euo pipefail

INTERFACE="${1:-lo}"
SCENARIO="${2:-all}"
URL="${3:-https://localhost:8888/}"
DURATION="${4:-30}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NETWORK_PY="$SCRIPT_DIR/chaos_network.py"
LOAD_PY="$SCRIPT_DIR/chaos_http_load.py"
MONITOR_PY="$SCRIPT_DIR/chaos_monitor.py"

if [ "$EUID" -ne 0 ]; then
    echo "This runner must be run as root (uses tc/iptables)."
    exit 1
fi

run_scenario() {
    local name="$1"
    echo ""
    echo "========================================"
    echo " Scenario: $name"
    echo "========================================"

    echo "[*] Applying impairment..."
    python3 "$NETWORK_PY" --scenario "$name" --interface "$INTERFACE" --yes &
    local chaos_pid=$!

    echo "[*] Monitoring for ${DURATION}s..."
    python3 "$MONITOR_PY" --interface "$INTERFACE" --duration "$DURATION" --interval 1 &
    local monitor_pid=$!

    sleep 2
    echo "[*] Running HTTP load test..."
    python3 "$LOAD_PY" "$URL" --requests 50 --concurrency 5 --timeout 10 --ignore-cert || true

    wait "$monitor_pid" || true

    echo "[*] Resetting impairment..."
    python3 "$NETWORK_PY" --reset --interface "$INTERFACE" --yes
    kill "$chaos_pid" 2>/dev/null || true
    wait "$chaos_pid" 2>/dev/null || true
}

if [ "$SCENARIO" = "all" ]; then
    for name in submarine_cable_cut earthquake_cable_damage war_facility_paralysis \
                ddos_backbone_congestion flapping solar_flare cascading_failure extreme; do
        run_scenario "$name"
    done
else
    run_scenario "$SCENARIO"
fi

echo ""
echo "[*] All chaos tests completed."
