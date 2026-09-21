#!/usr/bin/env python3
"""Interop test: WASM brotli module vs the system libbrotli.

Cross-checks in both directions over randomized and pathological inputs:
  - module compress -> system decompress == original
  - system compress -> module decompress == original

The system side speaks the same 2-byte header protocol via
build-wasm/brotli_sys_ref, linked against the platform libbrotli.
"""

import os
import random
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WASM = os.path.join(ROOT, "build-wasm", "brotli.wasm")
SYS_REF = os.path.join(ROOT, "build-wasm", "brotli_sys_ref")
WASMTIME = os.environ.get(
    "WASMTIME", os.path.expanduser("~/toolchains/wasmtime-v25.0.2-x86_64-linux/wasmtime")
)


def run_wasm(op, payload, quality=11):
    req = bytes([op, quality]) + payload
    proc = subprocess.run([WASMTIME, "run", WASM], input=req, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"wasm module exit {proc.returncode}: {proc.stderr[:300]}")
    return proc.stdout


def run_sys(op, payload, quality=11):
    req = bytes([op, quality]) + payload
    proc = subprocess.run([SYS_REF], input=req, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"system ref exit {proc.returncode}: {proc.stderr[:300]}")
    return proc.stdout


def gen_cases(rng):
    cases = []
    cases.append(("empty", b""))
    cases.append(("one-byte", b"a"))
    cases.append(("zeros-64k", bytes(65536)))
    cases.append(("same-byte", b"z" * 100000))
    cases.append(("counter", bytes(i & 0xFF for i in range(65536))))
    words = [b"the", b"quick", b"brown", b"fox", b"jumps", b"over", b"lazy", b"dog"]
    text = b" ".join(rng.choice(words) for _ in range(20000))
    cases.append(("text-like", text))
    n = rng.randrange(1, 128 * 1024)
    cases.append(("random", rng.randbytes(n)))
    return cases


def main():
    if not (os.path.exists(WASM) and os.path.exists(SYS_REF)):
        sys.exit("build both first (make wasm-brotli-test)")
    rng = random.Random(20260921)
    total = 0
    for name, data in gen_cases(rng):
        for q in (1, 5, 11):
            ct = run_wasm(1, data, q)
            pt = run_sys(2, ct)
            assert pt == data, f"FAIL wasm-compress(q={q}) {name}: system round-trip mismatch"
            ct2 = run_sys(1, data, q)
            pt2 = run_wasm(2, ct2)
            assert pt2 == data, f"FAIL sys-compress(q={q}) {name}: wasm round-trip mismatch"
            total += len(data)
        print(f"PASS {name} ({len(data)} bytes, qualities 1/5/11 both directions)")
    print(f"PASS: {total} bytes across cases, module == system libbrotli both directions")


if __name__ == "__main__":
    main()
