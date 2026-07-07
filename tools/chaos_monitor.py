#!/usr/bin/env python3
"""Monitor network state during chaos tests.

Logs interface counters, current tc qdisc, iptables rule counts, and RTT
measurements so you can correlate application behavior with impairment state.
"""

import argparse
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from typing import Dict, List, Optional


CHAOS_TAG = "fly_board_chaos"


def run(cmd: List[str], check: bool = False) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, check=check)


def get_iface_stats(interface: str) -> Dict[str, str]:
    """Read RX/TX bytes and packets from /proc/net/dev."""
    try:
        with open("/proc/net/dev", "r") as f:
            for line in f:
                if line.strip().startswith(interface + ":"):
                    parts = line.split(":")[1].split()
                    return {
                        "rx_bytes": parts[0],
                        "rx_packets": parts[1],
                        "rx_errors": parts[2],
                        "rx_dropped": parts[3],
                        "tx_bytes": parts[8],
                        "tx_packets": parts[9],
                        "tx_errors": parts[10],
                        "tx_dropped": parts[11],
                    }
    except Exception as exc:
        return {"error": str(exc)}
    return {"error": "interface not found"}


def get_tc_qdisc(interface: str) -> str:
    result = run(["tc", "qdisc", "show", "dev", interface])
    return result.stdout.strip()


def count_iptables_chaos_rules() -> Dict[str, int]:
    counts = {}
    for chain in ["INPUT", "OUTPUT", "FORWARD"]:
        result = run(["iptables", "-L", chain, "-n", "--line-numbers"])
        count = 0
        for line in result.stdout.splitlines():
            if CHAOS_TAG in line:
                count += 1
        counts[chain] = count
    return counts


def ping_rtt(host: str, count: int = 3, timeout: int = 5) -> Optional[float]:
    """Return average RTT in ms, or None if unreachable."""
    result = run(["ping", "-c", str(count), "-W", str(timeout), host])
    if result.returncode != 0:
        return None
    match = __import__("re").search(r"rtt min/avg/max/mdev = [\d.]+/([\d.]+)", result.stdout)
    if match:
        return float(match.group(1))
    return None


def sample(interface: str, ping_hosts: List[str]) -> Dict:
    return {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "interface": interface,
        "stats": get_iface_stats(interface),
        "tc_qdisc": get_tc_qdisc(interface),
        "iptables_chaos_rules": count_iptables_chaos_rules(),
        "ping_ms": {h: ping_rtt(h) for h in ping_hosts},
    }


def main():
    parser = argparse.ArgumentParser(description="Monitor network state during chaos tests.")
    parser.add_argument("--interface", default="lo", help="Interface to watch")
    parser.add_argument("--interval", type=float, default=1.0, help="Sampling interval in seconds")
    parser.add_argument("--duration", type=float, default=0.0, help="Total duration in seconds (0 = infinite)")
    parser.add_argument("--ping", action="append", default=["127.0.0.1", "8.8.8.8"],
                        help="Hosts to ping (can be repeated)")
    parser.add_argument("--output", help="Append JSON samples to this file")
    parser.add_argument("--pretty", action="store_true", help="Pretty-print JSON output")
    args = parser.parse_args()

    print(f"[*] Monitoring {args.interface} every {args.interval}s")
    print(f"[*] Ping targets: {args.ping}")
    if args.duration:
        print(f"[*] Will stop after {args.duration}s")

    start = time.time()
    try:
        while True:
            data = sample(args.interface, args.ping)
            if args.output:
                with open(args.output, "a") as f:
                    f.write(json.dumps(data) + "\n")
            if args.pretty:
                print(json.dumps(data, indent=2))
            else:
                rtts = ", ".join(f"{h}={v if v is not None else 'timeout'}ms" for h, v in data["ping_ms"].items())
                chaos_rules = sum(data["iptables_chaos_rules"].values())
                print(f"{data['timestamp']}  qdisc={data['tc_qdisc'].split()[1] if data['tc_qdisc'] else 'none'}  "
                      f"chaos_rules={chaos_rules}  rtt=[{rtts}]")

            if args.duration and (time.time() - start) >= args.duration:
                break
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\n[*] Monitoring stopped.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
