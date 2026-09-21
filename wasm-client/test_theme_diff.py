#!/usr/bin/env python3
"""Differential test: browser WASM theme generation vs the native build.

Both sides compile the same production sources (src/render/theme/*.c) with
the same stubbed server globals; the test pins the Emscripten build to the
native build over all modes and a set of override combinations.  Divergence
would mean compiler-specific behavior (float formatting, qsort stability)
leaking into generated CSS/JSON.
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATIVE_REF = os.path.join(ROOT, "build-wasm", "theme_ref")
WASM_JS = os.path.join(ROOT, "build-wasm", "theme.js")

NODE_DRIVER = r"""
const fs = require('fs');
const createThemeModule = require(process.argv[2]);
(async () => {
    const mod = await createThemeModule();
    const input = fs.readFileSync(process.argv[3]);
    const mode = input[0];
    const overrides = input.slice(1).toString('utf8');
    const oPtr = overrides ? mod._malloc(overrides.length + 1) : 0;
    if (oPtr) mod.stringToUTF8(overrides, oPtr, overrides.length + 1);
    let outPtr;
    if (mode === 2) outPtr = mod._fb_theme_all_json(oPtr);
    else outPtr = mod._fb_theme_css(mode, oPtr);
    if (oPtr) mod._free(oPtr);
    if (!outPtr) { console.error('theme build failed'); process.exit(2); }
    process.stdout.write(mod.UTF8ToString(outPtr));
    mod._fb_theme_free(outPtr);
})();
"""

OVERRIDES = [
    "",
    '{"accent":"#AA3333"}',
    '{"accent":"#12ab34","roundness":0.5}',
    '{"roundness":2.25}',
    '{"use_special_modes":"ocean,sepia"}',
    '{"bg_full_light":"/assets/img/bg.png"}',
    '{"bg_full_dark":"/assets/img/bg-dark.png"}',
    '{"accent":"#000","roundness":0,"use_special_modes":"forest"}',
]


def native(mode, overrides):
    proc = subprocess.run([NATIVE_REF], input=bytes([mode]) + overrides.encode(),
                          capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"native ref exit {proc.returncode}: {proc.stderr[:300]}")
    return proc.stdout


def wasm(mode, overrides):
    driver = os.path.join(ROOT, "build-wasm", "theme_node_driver.js")
    with open(driver, "w") as fp:
        fp.write(NODE_DRIVER)
    inp = os.path.join(ROOT, "build-wasm", "theme_input.bin")
    with open(inp, "wb") as fp:
        fp.write(bytes([mode]) + overrides.encode())
    proc = subprocess.run(["node", driver, WASM_JS, inp], capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"wasm module failed: {proc.stderr[:400]}")
    return proc.stdout


def main():
    if not (os.path.exists(NATIVE_REF) and os.path.exists(WASM_JS)):
        sys.exit("build both first (make wasm-theme-test)")
    modes = {0: "css-light", 1: "css-dark", 2: "all-json"}
    cases = 0
    for mode, label in modes.items():
        for ov in OVERRIDES:
            nat = native(mode, ov)
            was = wasm(mode, ov)
            if nat != was:
                sys.exit(f"FAIL {label} overrides={ov!r}: {len(nat)} != {len(was)} bytes")
            cases += 1
        print(f"PASS {label} x{len(OVERRIDES)}")
    print(f"PASS: {cases} cases, WASM module == native theme build byte for byte")


if __name__ == "__main__":
    main()
