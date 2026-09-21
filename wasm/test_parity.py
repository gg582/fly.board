#!/usr/bin/env python3
"""Parity driver for the TASFA crypto WASM module.

Cross-checks the sandboxed WASI module (build-wasm/tasfa_crypto.wasm run under
wasmtime) against the native OpenSSL EVP reference (build-wasm/tasfa_crypto_ref):

  - module encrypt == reference encrypt (byte-identical ciphertext+tag)
  - module decrypt(reference output) == original plaintext
  - reference decrypt(module output) == original plaintext
  - module rejects tampered ciphertext/tags

Uses deterministic pseudo-random cases so failures are reproducible.
"""

import os
import random
import struct
import subprocess
import sys

OP_ENCRYPT = 1
OP_DECRYPT = 2

WASMTIME = os.environ.get(
    "WASMTIME", os.path.expanduser("~/toolchains/wasmtime-v25.0.2-x86_64-linux/wasmtime")
)
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WASM = os.path.join(ROOT, "build-wasm", "tasfa_crypto.wasm")
REF = os.path.join(ROOT, "build-wasm", "tasfa_crypto_ref")


def frame(op, key, iv_seed, chunk_index, sid, data):
    return (
        struct.pack(">B", op)
        + key
        + iv_seed
        + struct.pack(">i", chunk_index)
        + struct.pack(">H", len(sid))
        + sid
        + struct.pack(">I", len(data))
        + data
    )


def run(binpath, payload, use_wasmtime=False):
    cmd = [binpath] if not use_wasmtime else [WASMTIME, "run", binpath]
    proc = subprocess.run(cmd, input=payload, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(f"{'wasm' if use_wasmtime else 'ref'} exited {proc.returncode}: {proc.stderr[:400]}")
    out = proc.stdout
    if len(out) < 5:
        raise RuntimeError(f"short response: {out.hex()}")
    status = out[0]
    out_len = struct.unpack(">I", out[1:5])[0]
    assert len(out) == 5 + out_len, f"response length mismatch: {len(out)} != {5 + out_len}"
    return status, out[5:]


def one_case(rng, i):
    key = rng.randbytes(32)
    iv_seed = rng.randbytes(12)
    chunk_index = rng.choice([0, 1, 255, 65536, rng.getrandbits(31)])
    sid = rng.choice(
        [b"", b"sess-abc", bytes(rng.choice(b"abcdefghij0123456789-") for _ in range(rng.randrange(1, 40)))]
    )
    pt = rng.randbytes(rng.choice([0, 1, 15, 16, 17, 100, 4096, rng.randrange(0, 8192)]))

    enc_req = frame(OP_ENCRYPT, key, iv_seed, chunk_index, sid, pt)
    st_w, ct_w = run(WASM, enc_req, use_wasmtime=True)
    st_r, ct_r = run(REF, enc_req)
    assert st_w == 0 and st_r == 0, f"case {i}: encrypt status wasm={st_w} ref={st_r}"
    assert ct_w == ct_r, f"case {i}: ciphertext mismatch\n wasm={ct_w.hex()}\n ref ={ct_r.hex()}"

    dec_req = frame(OP_DECRYPT, key, iv_seed, chunk_index, sid, ct_r)
    st, pt_w = run(WASM, dec_req, use_wasmtime=True)
    assert st == 0 and pt_w == pt, f"case {i}: module decrypt of ref output failed"
    st, pt_r = run(REF, dec_req)
    assert st == 0 and pt_r == pt, f"case {i}: ref decrypt failed"

    tampered = bytearray(ct_r)
    if tampered:
        tampered[rng.randrange(len(tampered))] ^= 0x01
    st, _ = run(WASM, frame(OP_DECRYPT, key, iv_seed, chunk_index, sid, bytes(tampered)),
                use_wasmtime=True)
    assert st == 1, f"case {i}: module accepted tampered ciphertext"

    st, _ = run(
        WASM,
        frame(OP_DECRYPT, key, iv_seed, chunk_index, sid[:-1] if len(sid) > 1 else b"x", ct_r),
        use_wasmtime=True,
    )
    assert st == 1, f"case {i}: module accepted wrong AAD"


def main():
    if not (os.path.exists(WASM) and os.path.exists(REF)):
        sys.exit("build the module and reference first (make wasm-tasfa-crypto-test)")
    rng = random.Random(20260921)
    n = int(os.environ.get("PARITY_CASES", "200"))
    for i in range(n):
        one_case(rng, i)
    print(f"PASS: {n} parity cases, module == OpenSSL EVP (both directions, tamper rejected)")


if __name__ == "__main__":
    main()
