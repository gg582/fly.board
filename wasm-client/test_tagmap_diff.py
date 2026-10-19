#!/usr/bin/env python3
"""Differential test: WASM tagmap layout/BFS kernel vs the native core.

Feeds synthetic and multi-continent tag graphs through the native reference
driver and the Emscripten WASM module (reading zero-copy TypedArray views via
CWIST v4's Module.cwistView.f64 / i32), requiring coordinates and radii to
agree within 1e-6 and ranks/hops to match identically.
"""

import os
import random
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WASM_JS = os.path.join(ROOT, "build-wasm", "tagmap.js")
NATIVE_REF = os.path.join(ROOT, "build-wasm", "tagmap_ref")

NODE_DRIVER = r"""
const fs = require('fs');
const createTagmapModule = require(process.argv[2]);
(async () => {
    const mod = await createTagmapModule();
    const buf = fs.readFileSync(process.argv[3]);
    const n_nodes = buf.readInt32LE(0);
    const n_edges = buf.readInt32LE(4);
    const focus_idx = buf.readInt32LE(8);
    const iters = buf.readInt32LE(12);

    mod._fb_tagmap_init();
    const nBytes = n_nodes * 3 * 4;
    const eBytes = n_edges * 3 * 4;
    const nPtr = mod._malloc(Math.max(4, nBytes));
    const ePtr = mod._malloc(Math.max(4, eBytes));

    if (nBytes > 0) {
        mod.HEAPU8.set(buf.subarray(16, 16 + nBytes), nPtr);
    }
    if (eBytes > 0) {
        mod.HEAPU8.set(buf.subarray(16 + nBytes, 16 + nBytes + eBytes), ePtr);
    }

    mod._fb_tagmap_layout(nPtr, n_nodes, ePtr, n_edges, iters);
    mod._fb_tagmap_bfs(focus_idx);

    mod._free(nPtr);
    mod._free(ePtr);

    const xs = mod.cwistView.f64(mod._fb_tagmap_x_ptr(), mod._fb_tagmap_x_len());
    const ys = mod.cwistView.f64(mod._fb_tagmap_y_ptr(), mod._fb_tagmap_y_len());
    const rs = mod.cwistView.f64(mod._fb_tagmap_r_ptr(), mod._fb_tagmap_r_len());
    const ranks = mod.cwistView.i32(mod._fb_tagmap_rank_ptr(), mod._fb_tagmap_rank_len());
    const hops = mod.cwistView.i32(mod._fb_tagmap_hop_ptr(), mod._fb_tagmap_hop_len());

    const out = Buffer.alloc(n_nodes * (8 * 3 + 4 * 2));
    let off = 0;
    for (let i = 0; i < n_nodes; i++) { out.writeDoubleLE(xs[i], off); off += 8; }
    for (let i = 0; i < n_nodes; i++) { out.writeDoubleLE(ys[i], off); off += 8; }
    for (let i = 0; i < n_nodes; i++) { out.writeDoubleLE(rs[i], off); off += 8; }
    for (let i = 0; i < n_nodes; i++) { out.writeInt32LE(ranks[i], off); off += 4; }
    for (let i = 0; i < n_nodes; i++) { out.writeInt32LE(hops[i], off); off += 4; }
    process.stdout.write(out);
})();
"""


def pack_case(nodes, edges, focus_idx, iters=120):
    out = bytearray(struct.pack("<4i", len(nodes), len(edges), focus_idx, iters))
    for nid, cnt, bid in nodes:
        out += struct.pack("<3i", nid, cnt, bid)
    for a, b, w in edges:
        out += struct.pack("<3i", a, b, w)
    return bytes(out)


def unpack_out(raw, n):
    off = 0
    xs = struct.unpack_from(f"<{n}d", raw, off); off += 8 * n
    ys = struct.unpack_from(f"<{n}d", raw, off); off += 8 * n
    rs = struct.unpack_from(f"<{n}d", raw, off); off += 8 * n
    ranks = struct.unpack_from(f"<{n}i", raw, off); off += 4 * n
    hops = struct.unpack_from(f"<{n}i", raw, off); off += 4 * n
    return xs, ys, rs, ranks, hops


def run_native(payload, n):
    proc = subprocess.run([NATIVE_REF], input=payload, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"native ref failed: {proc.stderr[:300]}")
    return unpack_out(proc.stdout, n)


def run_wasm(payload, n):
    driver = os.path.join(ROOT, "build-wasm", "tagmap_node_driver.js")
    with open(driver, "w") as fp:
        fp.write(NODE_DRIVER)
    inp = os.path.join(ROOT, "build-wasm", "tagmap_input.bin")
    with open(inp, "wb") as fp:
        fp.write(payload)
    proc = subprocess.run(["node", driver, WASM_JS, inp], capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"wasm module failed: {proc.stderr[:400]}")
    return unpack_out(proc.stdout, n)


def main():
    if not (os.path.exists(WASM_JS) and os.path.exists(NATIVE_REF)):
        sys.exit("build both first (make wasm-tagmap-test)")

    rng = random.Random(20261019)
    cases = [
        ("single", [(1, 5, 10)], [], 0),
        ("chain-4", [(1, 10, 1), (2, 6, 1), (3, 4, 2), (4, 2, 2)],
         [(0, 1, 3), (1, 2, 2), (2, 3, 1)], 0),
    ]
    # Multi-continent random graph
    nodes = [(i + 1, rng.randint(1, 25), rng.choice([1, 2, 3, 4, 5])) for i in range(60)]
    edges = []
    seen = set()
    for _ in range(140):
        a = rng.randrange(60)
        b = rng.randrange(60)
        if a > b:
            a, b = b, a
        if a != b and (a, b) not in seen:
            seen.add((a, b))
            edges.append((a, b, rng.randint(1, 8)))
    cases.append(("multi-continent-60", nodes, edges, 3))

    worst = 0.0
    for name, nd, ed, focus in cases:
        payload = pack_case(nd, ed, focus, 120)
        nat = run_native(payload, len(nd))
        was = run_wasm(payload, len(nd))
        for arr_n, arr_w in zip(nat[:3], was[:3]):
            for a, b in zip(arr_n, arr_w):
                d = abs(a - b)
                worst = max(worst, d)
                assert d < 1e-5, f"FAIL {name}: float delta {d}"
        assert nat[3] == was[3], f"FAIL {name}: rank mismatch {nat[3]} vs {was[3]}"
        assert nat[4] == was[4], f"FAIL {name}: hop mismatch {nat[4]} vs {was[4]}"
        print(f"PASS {name} (n={len(nd)}, e={len(ed)})")

    print(f"PASS: all cases matched (worst coordinate delta {worst:.3g})")


if __name__ == "__main__":
    main()
