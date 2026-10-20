#!/usr/bin/env python3
"""Inline build-wasm/client_core.wasm into the client scripts that use it.

editor.js, tasfa-download.js and post-translate.js each carry the CWIST
WASM/WASI client core (src/utils/client_core.h via
wasm-client/client_core_module.c) as one base64 literal, `var b64 = '...';`,
so the kernel instantiates synchronously with no extra request.  The scripts
themselves are the source; this only swaps that literal for a fresh build.
Run through `make wasm-client-core`, then `make minjs`.
"""

import base64
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WASM_PATH = os.path.join(ROOT, "build-wasm", "client_core.wasm")
SCRIPTS = ("editor.js", "tasfa-download.js", "post-translate.js")
B64_RE = re.compile(r"var b64 = '[A-Za-z0-9+/=]+';")


def main():
    if not os.path.exists(WASM_PATH):
        sys.exit(f"Missing {WASM_PATH}")
    with open(WASM_PATH, "rb") as fp:
        literal = "var b64 = '" + base64.b64encode(fp.read()).decode("ascii") + "';"
    for name in SCRIPTS:
        path = os.path.join(ROOT, "public", "js", name)
        with open(path, "r", encoding="utf-8") as fp:
            code = fp.read()
        code, count = B64_RE.subn(lambda _m: literal, code)
        if count != 1:
            sys.exit(f"{path}: expected exactly one inlined client_core.wasm, found {count}")
        with open(path, "w", encoding="utf-8") as fp:
            fp.write(code)
        print(f"Inlined client_core.wasm into {path}")


if __name__ == "__main__":
    main()
