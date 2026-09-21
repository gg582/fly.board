#!/usr/bin/env python3
"""Differential test: WASM contrast sampler vs the native core.

Feeds identical raw RGB images (edge sizes plus random) through the native
reference driver and the Emscripten module under node, requiring the three
L* values to agree within 1e-9 (same source; only libm could differ).
"""

import os
import random
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WASM_JS = os.path.join(ROOT, "build-wasm", "contrast.js")
NATIVE_REF = os.path.join(ROOT, "build-wasm", "contrast_ref")

NODE_DRIVER = r"""
const fs = require('fs');
const createContrastModule = require(process.argv[2]);
(async () => {
    const mod = await createContrastModule();
    const input = fs.readFileSync(process.argv[3]);
    const w = input.readInt32BE(0), h = input.readInt32BE(4);
    const px = input.length - 8;
    const inPtr = mod._malloc(px);
    mod.writeArrayToMemory(input.subarray(8), inPtr);
    const outPtr = mod._malloc(24);
    const rc = mod._fb_contrast_sample(inPtr, w, h, outPtr);
    mod._free(inPtr);
    if (rc !== 0) { console.error('sample failed'); process.exit(2); }
    const L = new Float64Array(mod.HEAPU8.buffer, outPtr, 3);
    const buf = Buffer.alloc(24);
    buf.writeDoubleLE(L[0], 0); buf.writeDoubleLE(L[1], 8); buf.writeDoubleLE(L[2], 16);
    process.stdout.write(buf);
    mod._free(outPtr);
})();
"""


def native(w, h, rgb):
    req = struct.pack(">II", w, h) + rgb
    proc = subprocess.run([NATIVE_REF], input=req, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"native ref failed: {proc.stderr[:300]}")
    return struct.unpack("<3d", proc.stdout)


def wasm(w, h, rgb):
    driver = os.path.join(ROOT, "build-wasm", "contrast_node_driver.js")
    with open(driver, "w") as fp:
        fp.write(NODE_DRIVER)
    inp = os.path.join(ROOT, "build-wasm", "contrast_input.bin")
    with open(inp, "wb") as fp:
        fp.write(struct.pack(">II", w, h) + rgb)
    proc = subprocess.run(["node", driver, WASM_JS, inp], capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"wasm module failed: {proc.stderr[:400]}")
    return struct.unpack("<3d", proc.stdout)


def gen_cases(rng):
    cases = []
    for w, h in [(1, 1), (3, 1), (1, 5), (2, 2), (7, 3), (64, 48), (129, 65)]:
        rgb = bytes(rng.randrange(256) for _ in range(w * h * 3))
        cases.append((f"{w}x{h}", w, h, rgb))
    # gradient (deterministic color regions)
    w, h = 90, 40
    rgb = bytearray(w * h * 3)
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 3
            rgb[i] = (x * 255) // w
            rgb[i + 1] = (y * 255) // h
            rgb[i + 2] = 128
    cases.append(("gradient", w, h, bytes(rgb)))
    return cases


def main():
    if not (os.path.exists(WASM_JS) and os.path.exists(NATIVE_REF)):
        sys.exit("build both first (make wasm-contrast-test)")
    rng = random.Random(20260921)
    worst = 0.0
    for name, w, h, rgb in gen_cases(rng):
        nat = native(w, h, rgb)
        was = wasm(w, h, rgb)
        for a, b in zip(nat, was):
            d = abs(a - b)
            worst = max(worst, d)
            assert d < 1e-9, f"FAIL {name}: delta {d}"
        print(f"PASS {name}")
    print(f"PASS: all cases within 1e-9 (worst delta {worst:.3g})")


if __name__ == "__main__":
    main()
