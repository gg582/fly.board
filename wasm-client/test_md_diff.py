#!/usr/bin/env python3
"""Differential test: browser WASM markdown render vs the native pipeline.

Runs every fixture in wasm-client/fixtures/*.md through both
build-wasm/md_native_ref (native render_markdown_to_html, linked against
libcwist.a) and the Emscripten module (build-wasm/md_render.js) under node,
and requires byte-identical HTML modulo the image-dimension attributes that
only the server can inject (get_image_dimensions is stubbed in the module).

The width/height/aspect-ratio attribute block injected server-side is
normalized away on both sides before comparison; loading/decoding attributes
are injected by both paths.
"""

import difflib
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURES = os.path.join(ROOT, "wasm-client", "fixtures")
NATIVE_REF = os.path.join(ROOT, "build-wasm", "md_native_ref")

DIM_ATTRS = re.compile(
    r' width="\d+" height="\d+" style="aspect-ratio:\d+/\d+;background:var\(--hover\)"'
)

NODE_DRIVER = r"""
const fs = require('fs');
const createMdModule = require(process.argv[2]);
(async () => {
    const mod = await createMdModule();
    const fixture = fs.readFileSync(process.argv[3], 'utf8');
    const inPtr = mod._malloc(fixture.length + 1);
    mod.stringToUTF8(fixture, inPtr, fixture.length + 1);
    const outPtr = mod._fb_md_render(inPtr);
    mod._free(inPtr);
    if (!outPtr) { console.error('render failed'); process.exit(2); }
    process.stdout.write(mod.UTF8ToString(outPtr));
    mod._fb_md_free(outPtr);
})();
"""


def native_render(path):
    proc = subprocess.run([NATIVE_REF], input=open(path, "rb").read(), capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"native ref failed on {path}: {proc.stderr[:300]}")
    return proc.stdout.decode()


def wasm_render(path):
    driver = os.path.join(ROOT, "build-wasm", "md_node_driver.js")
    with open(driver, "w") as fp:
        fp.write(NODE_DRIVER)
    proc = subprocess.run(["node", driver, os.path.join(ROOT, "build-wasm", "md_render.js"), path],
                          capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"wasm module failed on {path}: {proc.stderr[:400]}")
    return proc.stdout.decode()


def normalize(html):
    return DIM_ATTRS.sub("", html)


def main():
    if not os.path.exists(NATIVE_REF):
        sys.exit("build the native reference first (make wasm-md-test)")
    names = sorted(n for n in os.listdir(FIXTURES) if n.endswith(".md"))
    if not names:
        sys.exit(f"no fixtures in {FIXTURES}")
    failed = 0
    for name in names:
        nat = normalize(native_render(os.path.join(FIXTURES, name)))
        was = normalize(wasm_render(os.path.join(FIXTURES, name)))
        if nat == was:
            print(f"PASS {name}")
        else:
            failed += 1
            print(f"FAIL {name}")
            for line in list(difflib.unified_diff(nat.splitlines(), was.splitlines(), "native", "wasm"))[:40]:
                print(line)
    if failed:
        sys.exit(f"{failed} fixture(s) diverged")
    print(f"PASS: {len(names)} fixtures, WASM module == native pipeline")


if __name__ == "__main__":
    main()
