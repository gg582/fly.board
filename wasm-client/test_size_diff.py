#!/usr/bin/env python3
"""Differential test: WASM image-size probe vs the native stb build.

Both sides compile size_module.c, so this pins the Emscripten stb build to
the native one over valid PNGs (several sizes), truncated PNGs, and junk
input.  PNGs are crafted in-process (zlib + struct) to avoid fixtures.
"""

import os
import struct
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WASM_JS = os.path.join(ROOT, "build-wasm", "size.js")
NATIVE_REF = os.path.join(ROOT, "build-wasm", "size_ref")

NODE_DRIVER = r"""
const fs = require('fs');
const createSizeModule = require(process.argv[2]);
(async () => {
    const mod = await createSizeModule();
    const input = fs.readFileSync(process.argv[3]);
    const inPtr = mod._malloc(input.length);
    mod.writeArrayToMemory(input, inPtr);
    const outPtr = mod._malloc(8);
    const rc = mod._fb_image_size(inPtr, input.length, outPtr, outPtr + 4);
    mod._free(inPtr);
    if (rc !== 0) { console.error('probe failed'); process.exit(3); }
    const v = new Int32Array(mod.HEAPU8.buffer, outPtr, 2);
    const buf = Buffer.alloc(8);
    buf.writeInt32BE(v[0], 0); buf.writeInt32BE(v[1], 4);
    process.stdout.write(buf);
    mod._free(outPtr);
})();
"""


def make_png(w, h):
    """Minimal truecolor PNG."""
    def chunk(typ, data):
        c = struct.pack(">I", len(data)) + typ + data
        return c + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    raw = b"".join(b"\x00" + b"\x11\x22\x33" * w for _ in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def native(data):
    proc = subprocess.run([NATIVE_REF], input=data, capture_output=True)
    return proc.returncode, proc.stdout


def wasm(data):
    driver = os.path.join(ROOT, "build-wasm", "size_node_driver.js")
    with open(driver, "w") as fp:
        fp.write(NODE_DRIVER)
    inp = os.path.join(ROOT, "build-wasm", "size_input.bin")
    with open(inp, "wb") as fp:
        fp.write(data)
    proc = subprocess.run(["node", driver, WASM_JS, inp], capture_output=True)
    return proc.returncode, proc.stdout


def main():
    if not (os.path.exists(WASM_JS) and os.path.exists(NATIVE_REF)):
        sys.exit("build both first (make wasm-size-test)")
    cases = []
    for w, h in [(1, 1), (3, 2), (640, 480), (1920, 1080)]:
        cases.append((f"png-{w}x{h}", make_png(w, h), True))
    png = make_png(10, 10)
    cases.append(("truncated-png", png[:20], False))
    cases.append(("junk", b"not an image at all", False))
    cases.append(("empty", b"", False))

    for name, data, ok in cases:
        rc_n, out_n = native(data)
        rc_w, out_w = wasm(data)
        assert (rc_n == 0) == ok, f"native {name}: rc {rc_n} expected ok={ok}"
        assert (rc_w == 0) == ok, f"wasm {name}: rc {rc_w} expected ok={ok}"
        if ok:
            assert out_n == out_w, f"FAIL {name}: {out_n.hex()} != {out_w.hex()}"
        print(f"PASS {name}")
    print("PASS: size probe agrees on all cases")


if __name__ == "__main__":
    main()
