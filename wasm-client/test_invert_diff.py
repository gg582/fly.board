#!/usr/bin/env python3
"""Differential test: browser WASM image inversion vs the native core.

Feeds identical RGBA buffers (random plus edge-case colors) through the
native reference driver (build-wasm/invert_ref, same source compiled with
cc) and the Emscripten module (build-wasm/invert.js) under node, requiring
per-channel agreement within 1 LSB (libm float differences between hosts).
"""

import os
import random
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WASM_JS = os.path.join(ROOT, "build-wasm", "invert.js")
NATIVE_REF = os.path.join(ROOT, "build-wasm", "invert_ref")

NODE_DRIVER = r"""
const fs = require('fs');
const createInvertModule = require(process.argv[2]);
(async () => {
    const mod = await createInvertModule();
    const input = fs.readFileSync(process.argv[3]);
    const mode = input[0];
    const px = input.length - 1;
    const inPtr = mod._malloc(px);
    mod.writeArrayToMemory(input.subarray(1), inPtr);
    const outPtr = mod._fb_invert_rgba(inPtr, px / 4, mode);
    mod._free(inPtr);
    if (!outPtr) { console.error('invert failed'); process.exit(2); }
    const out = new Uint8Array(mod.HEAPU8.buffer, outPtr, px);
    process.stdout.write(Buffer.from(out));
    mod._fb_invert_free(outPtr);
})();
"""


def native_invert(mode, rgba):
    proc = subprocess.run([NATIVE_REF], input=bytes([mode]) + rgba, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"native ref failed: {proc.stderr[:300]}")
    return proc.stdout


def wasm_invert(mode, rgba):
    driver = os.path.join(ROOT, "build-wasm", "invert_node_driver.js")
    with open(driver, "w") as fp:
        fp.write(NODE_DRIVER)
    inp = os.path.join(ROOT, "build-wasm", "invert_input.bin")
    with open(inp, "wb") as fp:
        fp.write(bytes([mode]) + rgba)
    proc = subprocess.run(["node", driver, WASM_JS, inp], capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"wasm module failed: {proc.stderr[:400]}")
    return proc.stdout


def gen_cases(rng):
    edge = [0, 1, 127, 128, 254, 255]
    cases = []
    fixed = [(r, g, b, 255) for r in edge for g in edge for b in edge]
    buf = b"".join(bytes(c) for c in fixed)
    cases.append(("edge-grid", buf))
    n = 4096
    buf = bytearray(n * 4)
    for i in range(n):
        buf[i * 4] = rng.randrange(256)
        buf[i * 4 + 1] = rng.randrange(256)
        buf[i * 4 + 2] = rng.randrange(256)
        buf[i * 4 + 3] = rng.choice([0, 128, 255])
    cases.append(("random-4k", bytes(buf)))
    return cases


def main():
    if not (os.path.exists(WASM_JS) and os.path.exists(NATIVE_REF)):
        sys.exit("build both first (make wasm-img-test)")
    rng = random.Random(20260921)
    checked = 0
    for mode in (0, 1):
        for name, rgba in gen_cases(rng):
            nat = native_invert(mode, rgba)
            was = wasm_invert(mode, rgba)
            if len(nat) != len(was):
                sys.exit(f"FAIL mode={mode} {name}: length {len(nat)} != {len(was)}")
            worst = 0
            for a, b in zip(nat, was):
                d = abs(a - b)
                if d > worst:
                    worst = d
            checked += len(nat)
            if worst > 1:
                sys.exit(f"FAIL mode={mode} {name}: max channel delta {worst} > 1 LSB")
            print(f"PASS mode={'oklch' if mode else 'luminance'} {name} (max delta {worst} LSB)")
    print(f"PASS: {checked} bytes compared, WASM == native core within 1 LSB")


if __name__ == "__main__":
    main()
