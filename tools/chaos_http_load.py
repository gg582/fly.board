#!/usr/bin/env python3
"""HTTP load generator for chaos testing fly.board.

Reports success/failure counts, latency percentiles, and timeout/error types.
Works with both HTTP and HTTPS, with an option to ignore certificate errors.
"""

import argparse
import concurrent.futures
import ssl
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Dict, List


@dataclass
class Result:
    status: int = 0
    latency_ms: float = 0.0
    error: str = ""


@dataclass
class Stats:
    total: int = 0
    ok: int = 0
    errors: int = 0
    timeouts: int = 0
    statuses: Dict[int, int] = field(default_factory=dict)
    latencies: List[float] = field(default_factory=list)

    def add(self, result: Result):
        self.total += 1
        if result.error:
            self.errors += 1
            if "timeout" in result.error.lower():
                self.timeouts += 1
        else:
            self.ok += 1
            self.statuses[result.status] = self.statuses.get(result.status, 0) + 1
            self.latencies.append(result.latency_ms)

    def summary(self) -> str:
        lines = [
            f"total={self.total} ok={self.ok} errors={self.errors} timeouts={self.timeouts}",
            f"statuses={self.statuses}",
        ]
        if self.latencies:
            self.latencies.sort()
            p50 = self.latencies[int(len(self.latencies) * 0.5)]
            p95 = self.latencies[int(len(self.latencies) * 0.95)]
            p99 = self.latencies[min(len(self.latencies) - 1, int(len(self.latencies) * 0.99))]
            lines.append(f"latency_ms min={self.latencies[0]:.1f} p50={p50:.1f} p95={p95:.1f} p99={p99:.1f} max={self.latencies[-1]:.1f}")
        else:
            lines.append("latency_ms=none")
        return "\n".join(lines)


def fetch(url: str, timeout: float, ignore_cert: bool, headers: Dict[str, str]) -> Result:
    start = time.perf_counter()
    try:
        req = urllib.request.Request(url, headers=headers)
        ctx = ssl.create_default_context()
        if ignore_cert:
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
            _ = resp.read(1)  # minimal read
            return Result(status=resp.status, latency_ms=(time.perf_counter() - start) * 1000)
    except urllib.error.HTTPError as exc:
        return Result(status=exc.code, latency_ms=(time.perf_counter() - start) * 1000)
    except urllib.error.URLError as exc:
        return Result(error=str(exc.reason))
    except TimeoutError:
        return Result(error="timeout")
    except Exception as exc:
        return Result(error=str(exc))


def main():
    parser = argparse.ArgumentParser(description="HTTP load generator for chaos testing.")
    parser.add_argument("url", help="Target URL (e.g. https://localhost:8888/)")
    parser.add_argument("--requests", "-n", type=int, default=100, help="Total requests")
    parser.add_argument("--concurrency", "-c", type=int, default=10, help="Concurrent workers")
    parser.add_argument("--timeout", "-t", type=float, default=10.0, help="Request timeout in seconds")
    parser.add_argument("--interval", type=float, default=0.0, help="Delay between requests per worker")
    parser.add_argument("--ignore-cert", action="store_true", help="Ignore TLS certificate errors")
    parser.add_argument("--header", action="append", default=[], help="Extra header (Key:Value)")
    args = parser.parse_args()

    headers = {}
    for h in args.header:
        key, _, val = h.partition(":")
        headers[key.strip()] = val.strip()

    stats = Stats()

    def worker_task(_):
        result = fetch(args.url, args.timeout, args.ignore_cert, headers)
        if args.interval > 0:
            time.sleep(args.interval)
        return result

    print(f"[*] Loading {args.url} with {args.requests} requests and {args.concurrency} workers")
    start = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as executor:
        for result in executor.map(worker_task, range(args.requests)):
            stats.add(result)
    elapsed = time.time() - start

    print(f"[*] Finished in {elapsed:.1f}s")
    print(stats.summary())
    return 0 if stats.errors == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
