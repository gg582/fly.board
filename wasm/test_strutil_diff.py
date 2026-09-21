#!/usr/bin/env python3
"""Parity test: WASM string-util module vs the production implementations.

The module and the server share src/utils/strutil_pure.h (utils.c and
sql_escape.c are thin wrappers), so this test pins the framing and the
behavior on a corpus of edge cases: framing bugs and NUL handling are the
real risk, not the algorithms.
"""

import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WASM = os.path.join(ROOT, "build-wasm", "strutil.wasm")
WASMTIME = os.environ.get(
    "WASMTIME", os.path.expanduser("~/toolchains/wasmtime-v25.0.2-x86_64-linux/wasmtime")
)

sys.path.insert(0, os.path.join(ROOT, "wasm"))


def run(op, payload):
    proc = subprocess.run([WASMTIME, "run", WASM], input=bytes([op]) + payload,
                          capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"module exit {proc.returncode}: {proc.stderr[:300]}")
    return proc.stdout


def expect_slug(title):
    slug = []
    prev_dash = False
    for ch in title:
        if ch.isalnum() and ch.isascii():
            slug.append(ch.lower())
            prev_dash = False
        elif ch in " -_":
            # C: adds '-' when j==0 OR previous char is not '-'
            if not slug or not prev_dash:
                slug.append("-")
                prev_dash = True
    out = "".join(slug)
    if out.endswith("-"):
        out = out[:-1]
    return out if out else "post"


ESCAPE = {"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#x27;"}


def expect_escape(s):
    return "".join(ESCAPE.get(c, c) for c in s)


def expect_unescape(s):
    out = []
    i = 0
    while i < len(s):
        matched = False
        for ent, ch in [("&amp;", "&"), ("&lt;", "<"), ("&gt;", ">"), ("&gtl;", None),
                        ("&quot;", '"'), ("&#x27;", "'"), ("&#39;", "'")]:
            if ch and s.startswith(ent, i):
                out.append(ch)
                i += len(ent)
                matched = True
                break
        if not matched:
            out.append(s[i])
            i += 1
    return "".join(out)


def utf8_truncate_expected(s, max_bytes):
    data = s.encode()
    if len(data) <= max_bytes:
        return len(data)
    i = max_bytes
    while i > 0 and (data[i] & 0xC0) == 0x80:
        i -= 1
    return i


ALLOWED = ("public/uploads/", "public/profile/", "public/img/", "public/media/", "data/tasfa/")


def safe_path_expected(s):
    if not s or s.startswith("/") or s.startswith(".."):
        return False
    if not any(s.startswith(p) for p in ALLOWED):
        return False
    parts = s.split("/")
    return ".." not in parts


def sanitize_expected(s):
    base = s.replace("\\", "/").split("/")[-1]
    out = "".join(c for c in base if c not in "/\\\r\n" and ord(c) >= 0x20)[:255]
    return out


def main():
    if not os.path.exists(WASM):
        sys.exit("build the module first (make wasm-strutil-test)")

    titles = ["", "Hello World", "  Hello   World  ", "한글 제목 테스트", "a/b\\c",
              "UPPER_case-123", "---", "tab\tname", "x" * 300]
    for t in titles:
        got = run(1, t.encode() + b"\0").decode()
        want = expect_slug(t)
        assert got == want, f"slug({t!r}): got {got!r} want {want!r}"
    print(f"PASS slug x{len(titles)}")

    strings = ["", "ascii", "한글텍스트", "a" * 100, "mix한글abc", "\xed\x95"]
    for s in strings:
        data = s.encode("utf-8", "surrogateescape") if isinstance(s, str) else s
        for mb in (0, 1, 2, 3, 5, 50, 10_000):
            got = struct.unpack(">Q", run(2, struct.pack(">Q", mb) + data + b"\0"))[0]
            want = utf8_truncate_expected(data.decode("utf-8", "ignore"), mb)
            assert got == want, f"truncate({s!r}, {mb}): got {got} want {want}"
    print("PASS truncate")

    paths = ["", "/abs", "../evil", "public/uploads/a.png", "public/uploads/../evil",
             "public/img/x/../y", "data/tasfa/sess/f", "public/etc/x", "public/uploads/.."]
    for p in paths:
        got = run(3, p.encode() + b"\0")[0]
        want = 1 if safe_path_expected(p) else 0
        assert got == want, f"safe_path({p!r}): got {got} want {want}"
    print(f"PASS safe_path x{len(paths)}")

    names = ["", "a.png", "/tmp/x\\y.png", "bad\nname.png", " spaced .jpg", "x" * 300, "\\"]
    for n in names:
        got = run(4, n.encode() + b"\0").decode()
        want = sanitize_expected(n)
        assert got == want, f"sanitize({n!r}): got {got!r} want {want!r}"
    print(f"PASS sanitize x{len(names)}")

    esc_in = ["", "plain", "a&b<c>d\"e'f", "&lt;&gtl;&amp;&quot;&#x27;&#39;", "x" * 500]
    for s in esc_in:
        got = run(5, s.encode() + b"\0").decode()
        assert got == expect_escape(s), f"escape({s!r}): got {got!r}"
        back = run(6, got.encode() + b"\0").decode()
        assert back == expect_unescape(got), f"unescape({got!r}): got {back!r}"
    print(f"PASS sql_escape/unescape x{len(esc_in)}")

    print("PASS: strutil module matches production behavior on all cases")


if __name__ == "__main__":
    main()
