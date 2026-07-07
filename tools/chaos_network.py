#!/usr/bin/env python3
"""Network chaos engineering toolkit for fly.board.

Simulates infrastructure-level failures (submarine cable cuts, earthquakes,
wars, flapping, DDoS congestion, and more) using Linux tc/iptables/ip.

All destructive operations require --yes. Default is dry-run.
"""

import argparse
import json
import os
import random
import re
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional

STATE_FILE = "/tmp/fly_board_chaos_state.json"
CHAOS_TAG = "fly_board_chaos"


# ---------------------------------------------------------------------------
# Scenario definitions
# ---------------------------------------------------------------------------

@dataclass
class Scenario:
    name: str
    title: str
    description: str
    params: Dict[str, any] = field(default_factory=dict)
    iptables_rules: List[str] = field(default_factory=list)
    route_rules: List[str] = field(default_factory=list)
    flap: bool = False
    flap_args: Dict[str, any] = field(default_factory=dict)


SCENARIOS = {
    "submarine_cable_cut": Scenario(
        name="submarine_cable_cut",
        title="Submarine cable cut (APAC-EU path)",
        description=(
            "A major undersea cable is severed. Traffic reroutes over a longer "
            "path, causing sustained high latency and moderate loss."
        ),
        params={
            "latency_ms": 850,
            "jitter_ms": 120,
            "loss_percent": 3.5,
            "corrupt_percent": 0.1,
            "bandwidth_kbit": 5000,
        },
    ),
    "earthquake_cable_damage": Scenario(
        name="earthquake_cable_damage",
        title="Earthquake + multiple cable breaks",
        description=(
            "Seismic activity damages several landing stations and cables. "
            "Severe jitter, bursty loss, corruption, and reordering."
        ),
        params={
            "latency_ms": 1200,
            "jitter_ms": 400,
            "loss_percent": 12.0,
            "corrupt_percent": 1.0,
            "duplicate_percent": 0.5,
            "reorder_percent": 8.0,
            "bandwidth_kbit": 2000,
        },
    ),
    "war_facility_paralysis": Scenario(
        name="war_facility_paralysis",
        title="War-induced facility paralysis",
        description=(
            "A key PoP/IXP goes dark. All routes through it become unreachable; "
            "remaining paths are overloaded and unstable."
        ),
        params={
            "latency_ms": 600,
            "jitter_ms": 200,
            "loss_percent": 25.0,
            "corrupt_percent": 2.0,
            "bandwidth_kbit": 1000,
        },
        route_rules=[
            "blackhole 203.0.113.0/24",
            "blackhole 198.51.100.0/24",
        ],
    ),
    "flapping": Scenario(
        name="flapping",
        title="BGP/route flapping",
        description=(
            "A router upstream is unstable: routes alternate between available "
            "and unreachable every few seconds."
        ),
        flap=True,
        flap_args={
            "period_seconds": 7.0,
            "jitter_seconds": 2.0,
            "drop_duration_seconds": 3.0,
            "drop_duration_jitter": 1.0,
        },
    ),
    "ddos_backbone_congestion": Scenario(
        name="ddos_backbone_congestion",
        title="DDoS causing backbone congestion",
        description=(
            "A volumetric attack saturates shared upstream capacity. Available "
            "bandwidth collapses and queues overflow into random drops."
        ),
        params={
            "bandwidth_kbit": 500,
            "latency_ms": 80,
            "jitter_ms": 60,
            "loss_percent": 7.0,
        },
    ),
    "dns_apocalypse": Scenario(
        name="dns_apocalypse",
        title="DNS resolver failure",
        description=(
            "All public DNS resolvers become unreachable. Applications that "
            "rely on hostname resolution hang or fail."
        ),
        iptables_rules=[
            "-p udp --dport 53 -j DROP",
            "-p tcp --dport 53 -j DROP",
            "-p udp --dport 853 -j DROP",
            "-p tcp --dport 853 -j DROP",
        ],
    ),
    "extreme": Scenario(
        name="extreme",
        title="Compound extreme scenario",
        description=(
            "Worst-case compound failure: cable cut + earthquake + congestion "
            "+ flapping. Very high latency, heavy loss/corruption, and "
            "intermittent blackouts."
        ),
        params={
            "latency_ms": 3000,
            "jitter_ms": 1000,
            "loss_percent": 35.0,
            "corrupt_percent": 3.0,
            "duplicate_percent": 1.0,
            "reorder_percent": 15.0,
            "bandwidth_kbit": 300,
        },
        flap=True,
        flap_args={
            "period_seconds": 5.0,
            "jitter_seconds": 1.5,
            "drop_duration_seconds": 2.0,
            "drop_duration_jitter": 1.0,
        },
    ),
    "solar_flare": Scenario(
        name="solar_flare",
        title="Solar flare / atmospheric disruption",
        description=(
            "Elevated radiation causes random bit flips and massive corruption "
            "across satellite and long-haul links."
        ),
        params={
            "latency_ms": 700,
            "jitter_ms": 250,
            "corrupt_percent": 8.0,
            "loss_percent": 5.0,
            "bandwidth_kbit": 1500,
        },
    ),
    "cascading_failure": Scenario(
        name="cascading_failure",
        title="Cascading datacenter failure",
        description=(
            "A cooling failure triggers rolling brownouts. Bandwidth and "
            "latency degrade in waves as traffic shifts."
        ),
        params={
            "latency_ms": 400,
            "jitter_ms": 300,
            "loss_percent": 6.0,
            "bandwidth_kbit": 3000,
        },
        flap=True,
        flap_args={
            "period_seconds": 20.0,
            "jitter_seconds": 5.0,
            "drop_duration_seconds": 5.0,
            "drop_duration_jitter": 2.0,
        },
    ),
}


# ---------------------------------------------------------------------------
# Low-level command helpers
# ---------------------------------------------------------------------------

def run(cmd: List[str], dry_run: bool = False, check: bool = True) -> subprocess.CompletedProcess:
    """Run a shell command, respecting dry-run mode."""
    cmd_str = " ".join(cmd)
    if dry_run:
        print(f"[DRY-RUN] {cmd_str}")
        return subprocess.CompletedProcess(cmd, returncode=0, stdout="", stderr="")
    print(f"[RUN] {cmd_str}")
    return subprocess.run(cmd, check=check, capture_output=True, text=True)


def is_root() -> bool:
    return os.geteuid() == 0


def require_root():
    if not is_root():
        print("ERROR: This tool manipulates network devices and requires root.", file=sys.stderr)
        print("       Run with sudo or as root.", file=sys.stderr)
        sys.exit(1)


def save_state(state: dict):
    with open(STATE_FILE, "w") as f:
        json.dump(state, f, indent=2)


def load_state() -> Optional[dict]:
    if not os.path.exists(STATE_FILE):
        return None
    with open(STATE_FILE, "r") as f:
        return json.load(f)


def remove_state():
    if os.path.exists(STATE_FILE):
        os.remove(STATE_FILE)


# ---------------------------------------------------------------------------
# tc / iptables / ip helpers
# ---------------------------------------------------------------------------

def get_existing_qdisc(interface: str) -> Optional[str]:
    """Return the current root qdisc type for an interface, or None."""
    try:
        result = subprocess.run(
            ["tc", "qdisc", "show", "dev", interface],
            capture_output=True, text=True, check=True,
        )
        # Example line: "qdisc noqueue 0: dev lo root refcnt 2"
        match = re.search(r"qdisc\s+(\w+)\s+[^:]*:\s+dev\s+" + re.escape(interface) + r"\s+root", result.stdout)
        if match:
            return match.group(1)
    except Exception:
        pass
    return None


def build_netem_cmd(params: Dict[str, any], parent: str = "root") -> List[str]:
    """Build tc netem command from scenario parameters."""
    parts = ["tc", "qdisc", "add", "dev", params["interface"], parent, "netem"]

    latency = params.get("latency_ms", 0)
    jitter = params.get("jitter_ms", 0)
    if latency > 0:
        if jitter > 0:
            parts.extend(["delay", f"{latency}ms", f"{jitter}ms", "25%"])
        else:
            parts.extend(["delay", f"{latency}ms"])

    loss = params.get("loss_percent", 0.0)
    if loss > 0:
        parts.extend(["loss", f"{loss}%"])

    corrupt = params.get("corrupt_percent", 0.0)
    if corrupt > 0:
        parts.extend(["corrupt", f"{corrupt}%"])

    duplicate = params.get("duplicate_percent", 0.0)
    if duplicate > 0:
        parts.extend(["duplicate", f"{duplicate}%"])

    reorder = params.get("reorder_percent", 0.0)
    if reorder > 0:
        parts.extend(["reorder", f"{reorder}%"])

    return parts


def build_tbf_cmd(params: Dict[str, any]) -> List[str]:
    """Build tc tbf (bandwidth limit) command chained under netem."""
    bw = params.get("bandwidth_kbit", 0)
    if bw <= 0:
        return []
    # latency/limit tuned for HTTP; burst large enough for BDP at high RTT.
    burst = max(1600, int(bw * 1.5))
    limit = max(burst * 20, 32000)  # allow a small queue
    return [
        "tc", "qdisc", "add", "dev", params["interface"], "parent", "1:1",
        "tbf", "rate", f"{bw}kbit", "burst", f"{burst}B",
        "latency", "200ms", "limit", f"{limit}B",
    ]


def apply_tc(params: Dict[str, any], dry_run: bool) -> bool:
    """Apply tc netem (+ optional tbf) impairments."""
    interface = params["interface"]
    existing = get_existing_qdisc(interface)

    # We need a root handle 1: so we can attach a child tbf at 1:1.
    netem_cmd = build_netem_cmd(params)
    if "root" in netem_cmd:
        idx = netem_cmd.index("root")
        netem_cmd.insert(idx + 1, "handle")
        netem_cmd.insert(idx + 2, "1:")

    # If there is already a root qdisc, replace it instead of adding.
    if existing and existing != "netem":
        for i, word in enumerate(netem_cmd):
            if word == "add":
                netem_cmd[i] = "replace"
                break

    run(netem_cmd, dry_run=dry_run, check=False)

    tbf_cmd = build_tbf_cmd(params)
    if tbf_cmd:
        run(tbf_cmd, dry_run=dry_run, check=False)

    return True


def apply_iptables_rules(interface: str, rules: List[str], dry_run: bool,
                         target_port: Optional[int] = None,
                         target_host: Optional[str] = None) -> List[str]:
    """Apply iptables rules tagged for later cleanup. Returns list of applied rules."""
    applied = []
    for rule in rules:
        base = ["iptables", "-A", "INPUT", "-i", interface]
        if target_port:
            base.extend(["-p", "tcp", "--dport", str(target_port)])
        if target_host:
            base.extend(["-s", target_host])
        full = base + rule.split() + ["-m", "comment", "--comment", CHAOS_TAG]
        run(full, dry_run=dry_run, check=False)
        applied.append(" ".join(full))
    return applied


def apply_route_rules(rules: List[str], dry_run: bool) -> List[str]:
    """Apply ip route blackhole/unreachable rules."""
    applied = []
    for rule in rules:
        tokens = rule.split()
        if tokens[0] == "blackhole":
            cmd = ["ip", "route", "add", "blackhole", tokens[1]]
        elif tokens[0] == "unreachable":
            cmd = ["ip", "route", "add", "unreachable", tokens[1]]
        else:
            continue
        run(cmd, dry_run=dry_run, check=False)
        applied.append(" ".join(cmd))
    return applied


# ---------------------------------------------------------------------------
# Flapping worker
# ---------------------------------------------------------------------------

class FlapWorker:
    def __init__(self, interface: str, args: Dict[str, any], dry_run: bool,
                 target_port: Optional[int] = None, target_host: Optional[str] = None):
        self.interface = interface
        self.args = args
        self.dry_run = dry_run
        self.target_port = target_port
        self.target_host = target_host
        self.stop_event = threading.Event()
        self.thread: Optional[threading.Thread] = None

    def _add_drop(self):
        cmd = ["iptables", "-A", "INPUT", "-i", self.interface]
        if self.target_port:
            cmd.extend(["-p", "tcp", "--dport", str(self.target_port)])
        if self.target_host:
            cmd.extend(["-s", self.target_host])
        cmd.extend(["-j", "DROP", "-m", "comment", "--comment", CHAOS_TAG])
        run(cmd, dry_run=self.dry_run, check=False)

    def _remove_drop(self):
        # Remove matching DROP rule(s).
        while True:
            cmd = ["iptables", "-D", "INPUT", "-i", self.interface]
            if self.target_port:
                cmd.extend(["-p", "tcp", "--dport", str(self.target_port)])
            if self.target_host:
                cmd.extend(["-s", self.target_host])
            cmd.extend(["-j", "DROP", "-m", "comment", "--comment", CHAOS_TAG])
            result = run(cmd, dry_run=self.dry_run, check=False)
            if self.dry_run or result.returncode != 0:
                break

    def _loop(self):
        period = self.args.get("period_seconds", 5.0)
        jitter = self.args.get("jitter_seconds", 1.0)
        drop_dur = self.args.get("drop_duration_seconds", 2.0)
        drop_jitter = self.args.get("drop_duration_jitter", 0.5)

        while not self.stop_event.is_set():
            up_time = max(0.5, period + random.uniform(-jitter, jitter))
            print(f"[FLAP] link UP for {up_time:.1f}s")
            self._remove_drop()
            if self.stop_event.wait(up_time):
                break

            down_time = max(0.5, drop_dur + random.uniform(-drop_jitter, drop_jitter))
            print(f"[FLAP] link DOWN for {down_time:.1f}s")
            self._add_drop()
            if self.stop_event.wait(down_time):
                break

        self._remove_drop()
        print("[FLAP] worker stopped")

    def start(self):
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=5.0)


# ---------------------------------------------------------------------------
# Reset / cleanup
# ---------------------------------------------------------------------------

def reset_chaos(interface: str, dry_run: bool):
    """Remove all chaos rules applied by this tool."""
    print(f"[*] Resetting chaos state on {interface}...")

    # Remove tagged iptables rules from INPUT.
    for chain in ["INPUT", "OUTPUT", "FORWARD"]:
        while True:
            cmd = ["iptables", "-D", chain, "-m", "comment", "--comment", CHAOS_TAG, "-j", "DROP"]
            result = run(cmd, dry_run=dry_run, check=False)
            if dry_run or result.returncode != 0:
                break

    # Remove any other tagged rules generically.
    for chain in ["INPUT", "OUTPUT", "FORWARD"]:
        for proto in ["udp", "tcp"]:
            for dport in ["53", "853"]:
                while True:
                    cmd = [
                        "iptables", "-D", chain,
                        "-p", proto, "--dport", dport,
                        "-m", "comment", "--comment", CHAOS_TAG,
                        "-j", "DROP",
                    ]
                    result = run(cmd, dry_run=dry_run, check=False)
                    if dry_run or result.returncode != 0:
                        break

    # Remove tc qdisc.
    run(["tc", "qdisc", "del", "dev", interface, "root"], dry_run=dry_run, check=False)

    # Remove blackhole routes we might have added.
    state = load_state()
    if state:
        for route_cmd in state.get("route_rules", []):
            tokens = route_cmd.split()
            if "blackhole" in tokens:
                idx = tokens.index("blackhole")
                prefix = tokens[idx + 1]
                run(["ip", "route", "del", "blackhole", prefix], dry_run=dry_run, check=False)
            elif "unreachable" in tokens:
                idx = tokens.index("unreachable")
                prefix = tokens[idx + 1]
                run(["ip", "route", "del", "unreachable", prefix], dry_run=dry_run, check=False)

    remove_state()
    print("[*] Reset complete.")


# ---------------------------------------------------------------------------
# Apply scenario
# ---------------------------------------------------------------------------

def apply_scenario(scenario: Scenario, args: argparse.Namespace) -> Optional[FlapWorker]:
    interface = args.interface
    dry_run = not args.yes

    params = dict(scenario.params)
    params["interface"] = interface

    # Command-line overrides.
    if args.latency is not None:
        params["latency_ms"] = args.latency
    if args.loss is not None:
        params["loss_percent"] = args.loss
    if args.jitter is not None:
        params["jitter_ms"] = args.jitter
    if args.corrupt is not None:
        params["corrupt_percent"] = args.corrupt
    if args.bandwidth is not None:
        params["bandwidth_kbit"] = args.bandwidth

    print(f"[*] Scenario: {scenario.title}")
    print(f"    {scenario.description}")
    print(f"[*] Interface: {interface}")
    if args.target_port:
        print(f"[*] Target port: {args.target_port}")
    if args.target_host:
        print(f"[*] Target host: {args.target_host}")
    if dry_run:
        print("[*] DRY-RUN mode: no changes will be applied. Use --yes to execute.")

    state = {
        "interface": interface,
        "scenario": scenario.name,
        "iptables_rules": [],
        "route_rules": [],
        "flap": scenario.flap,
    }

    # 1. tc impairments (if any params).
    if params:
        apply_tc(params, dry_run)

    # 2. iptables rules (DNS blocks, etc.).
    if scenario.iptables_rules:
        applied = apply_iptables_rules(
            interface, scenario.iptables_rules, dry_run,
            target_port=args.target_port, target_host=args.target_host,
        )
        state["iptables_rules"] = applied

    # 3. route rules (blackholes).
    if scenario.route_rules:
        applied = apply_route_rules(scenario.route_rules, dry_run)
        state["route_rules"] = applied

    if not dry_run:
        save_state(state)

    # 4. Start flapping worker if needed.
    flap_worker = None
    if scenario.flap:
        flap_worker = FlapWorker(
            interface, scenario.flap_args, dry_run,
            target_port=args.target_port, target_host=args.target_host,
        )
        flap_worker.start()
        if not dry_run:
            print("[FLAP] background flapping started; press Ctrl+C to stop.")

    return flap_worker


# ---------------------------------------------------------------------------
# Custom scenario builder
# ---------------------------------------------------------------------------

def apply_custom(args: argparse.Namespace) -> Optional[FlapWorker]:
    """Apply a user-specified set of impairments without a named scenario."""
    interface = args.interface
    dry_run = not args.yes

    params = {
        "interface": interface,
        "latency_ms": args.latency or 0,
        "jitter_ms": args.jitter or 0,
        "loss_percent": args.loss or 0.0,
        "corrupt_percent": args.corrupt or 0.0,
        "duplicate_percent": args.duplicate or 0.0,
        "reorder_percent": args.reorder or 0.0,
        "bandwidth_kbit": args.bandwidth or 0,
    }

    print("[*] Custom impairment profile")
    print(f"    latency={params['latency_ms']}ms jitter={params['jitter_ms']}ms "
          f"loss={params['loss_percent']}% corrupt={params['corrupt_percent']}% "
          f"bandwidth={params['bandwidth_kbit']}kbit")
    if dry_run:
        print("[*] DRY-RUN mode: no changes will be applied. Use --yes to execute.")

    apply_tc(params, dry_run)

    if not dry_run:
        save_state({"interface": interface, "scenario": "custom", "iptables_rules": [], "route_rules": [], "flap": False})

    return None


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def list_scenarios():
    print("Available chaos scenarios:")
    for name, scenario in SCENARIOS.items():
        print(f"  {name:24s} - {scenario.title}")
        print(f"  {'':24s}   {scenario.description}")


def main():
    parser = argparse.ArgumentParser(
        description="Network chaos engineering toolkit for fly.board.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Dry-run submarine cable cut on loopback
  sudo python3 tools/chaos_network.py --scenario submarine_cable_cut --interface lo

  # Apply extreme chaos to eth0
  sudo python3 tools/chaos_network.py --scenario extreme --interface eth0 --yes

  # Target only the fly.board HTTPS port
  sudo python3 tools/chaos_network.py --scenario ddos_backbone_congestion --interface eth0 --target-port 8888 --yes

  # Custom impairment
  sudo python3 tools/chaos_network.py --interface eth0 --latency 500 --loss 5 --bandwidth 2000 --yes

  # Reset everything
  sudo python3 tools/chaos_network.py --reset --interface eth0 --yes
""",
    )
    parser.add_argument("--list", action="store_true", help="List available scenarios")
    parser.add_argument("--reset", action="store_true", help="Remove all chaos rules")
    parser.add_argument("--scenario", choices=list(SCENARIOS.keys()), help="Scenario to apply")
    parser.add_argument("--interface", default="lo", help="Network interface to impair (default: lo)")
    parser.add_argument("--yes", action="store_true", help="Actually apply changes (default is dry-run)")
    parser.add_argument("--target-port", type=int, help="Limit iptables effects to this destination port")
    parser.add_argument("--target-host", help="Limit iptables effects to this source address")

    # Custom overrides / custom profile knobs.
    parser.add_argument("--latency", type=int, help="Latency in milliseconds")
    parser.add_argument("--jitter", type=int, help="Latency jitter in milliseconds")
    parser.add_argument("--loss", type=float, help="Packet loss percentage")
    parser.add_argument("--corrupt", type=float, help="Packet corruption percentage")
    parser.add_argument("--duplicate", type=float, help="Packet duplication percentage")
    parser.add_argument("--reorder", type=float, help="Packet reordering percentage")
    parser.add_argument("--bandwidth", type=int, help="Bandwidth limit in kbit/s")

    args = parser.parse_args()

    if args.list:
        list_scenarios()
        return 0

    if args.yes:
        require_root()

    if args.reset:
        reset_chaos(args.interface, dry_run=not args.yes)
        return 0

    if args.scenario:
        scenario = SCENARIOS[args.scenario]
        flap_worker = None
        try:
            flap_worker = apply_scenario(scenario, args)
            if flap_worker and args.yes:
                # Keep main thread alive while flapping.
                while True:
                    time.sleep(1)
        except KeyboardInterrupt:
            print("\n[*] Interrupted, cleaning up...")
            if flap_worker:
                flap_worker.stop()
            reset_chaos(args.interface, dry_run=not args.yes)
        return 0

    # No scenario specified but knobs given -> custom profile.
    if any(v is not None for v in [args.latency, args.jitter, args.loss, args.corrupt,
                                    args.duplicate, args.reorder, args.bandwidth]):
        if args.yes:
            require_root()
        apply_custom(args)
        return 0

    parser.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
