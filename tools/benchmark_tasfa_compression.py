#!/usr/bin/env python3
"""Loopback benchmark of the actual TASFA slice-response and codec source.

No CWIST server/authentication/routing/TLS coverage is implied. Only dependency
adapters are substituted. Requires Python 3.9+, a C compiler and pkg-config
packages libbrotlienc, libbrotlidec, libzstd, openssl and zlib.

Example: CC=clang python3 tools/benchmark_tasfa_compression.py \
    --baseline-ref HEAD --sizes 8,32 --repeats 3 --output /tmp/tasfa-bench.json
The working tree is the candidate; git objects provide the immutable baseline.
"""
import argparse
import concurrent.futures
import ctypes
import hashlib
import http.server
import io
import json
import math
import os
import platform
from pathlib import Path
import random
import re
import resource
import shlex
import shutil
import statistics
import struct
import subprocess
import tempfile
import threading
import time
import urllib.parse
import urllib.request
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]


def require(condition, detail):
    # Unlike assert, integrity gates must survive PYTHONOPTIMIZE / python -O.
    if not condition:
        raise AssertionError(detail)


class Response(ctypes.Structure):
    _fields_ = [
        ("body", ctypes.c_void_p), ("length", ctypes.c_size_t),
        ("encoding", ctypes.c_char * 16), ("uncompressed_length", ctypes.c_size_t),
        ("encrypted", ctypes.c_int), ("headers_ok", ctypes.c_int),
        ("codec_calls", ctypes.c_uint64 * 3), ("codec_bytes", ctypes.c_uint64 * 3),
        ("codec_time_ns", ctypes.c_uint64 * 3),
    ]


def source(path, revision):
    if revision is None:
        return (ROOT / path).read_text()
    return subprocess.check_output(["git", "show", f"{revision}:{path}"], cwd=ROOT, text=True)


def build(directory, revision):
    directory.mkdir()
    header = source("src/handlers/tasfa/tasfa_internal.h", revision)
    enum = re.search(r"typedef enum\s*\{\s*TASFA_COMPRESS_NONE.*?\}\s*tasfa_compress_type_t;", header, re.S)
    if enum is None:
        raise RuntimeError("compression enum not found")
    constants = "\n".join(line for line in header.splitlines() if line.startswith("#define TASFA_COMPRESS_"))
    crypto = source("src/handlers/tasfa/crypto.c", revision)
    session = source("src/handlers/tasfa/session.c", revision)
    start = session.index("bool send_file_slice_response(")
    end = session.index("\nbool resolve_asset_scope_path(", start)
    response = session[start:end]
    (directory / "tasfa_fixture_crypto.inc").write_text(constants + "\n" + enum[0] + "\n" + crypto)
    (directory / "tasfa_fixture_response.inc").write_text(response)
    pc = os.environ.get("PKG_CONFIG") or shutil.which("pkg-config") or shutil.which("pkgconf")
    if not pc and Path("/opt/homebrew/bin/pkgconf").is_file():
        pc = "/opt/homebrew/bin/pkgconf"
    if not pc:
        raise RuntimeError("pkg-config/pkgconf is required")
    flags = shlex.split(subprocess.check_output([pc, "--cflags", "--libs", "libbrotlienc", "libbrotlidec", "libzstd", "openssl", "zlib"], text=True))
    compiler = shlex.split(os.environ.get("CC", "cc"))
    library = directory / "fixture.so"
    command = compiler + ["-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
        "-I" + str(directory), "-I" + str(ROOT / "src/handlers/tasfa"),
        str(ROOT / "tests/tasfa_endpoint_fixture.c"), "-o", str(library)] + flags
    subprocess.run(command, check=True)
    lib = ctypes.CDLL(str(library))
    lib.fixture_serve.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_size_t,
        ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(Response)]
    lib.fixture_serve.restype = ctypes.c_int
    lib.fixture_free.argtypes = [ctypes.c_void_p]
    lib.fixture_free.restype = None
    lib.fixture_verify.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p,
        ctypes.c_int, ctypes.c_char_p, ctypes.c_size_t]
    lib.fixture_verify.restype = ctypes.c_int
    return lib, {"revision": revision or "working-tree", "command": command,
        "platform": platform.platform(),
        "codec_versions": subprocess.check_output([pc, "--modversion", "libbrotlienc", "libbrotlidec", "libzstd", "openssl", "zlib"], text=True).splitlines(),
        "adapter_sha256": hashlib.sha256((ROOT / "tests/tasfa_endpoint_fixture.c").read_bytes()).hexdigest(),
        "crypto_sha256": hashlib.sha256(crypto.encode()).hexdigest(),
        "response_sha256": hashlib.sha256(response.encode()).hexdigest()}


def fixture_files(directory, max_mib, extra):
    directory.mkdir()
    size = max_mib * 1024 * 1024
    noise = random.Random(250024).randbytes(size + 8192)
    fixtures = {}
    def add(name, data, mime):
        path = directory / name
        path.write_bytes(data)
        fixtures[name] = (path, mime)
    add("random", noise, "application/octet-stream")
    text = b"TASFA deterministic compressible text control: abcdef0123456789\n"
    add("text", (text * (size // len(text) + 1))[:size], "text/plain")
    # Adversarial control: compressible contents outside all sampled windows.
    # A sampling policy can send this raw; report the cost, never hide it.
    mixed = bytearray(b"a" * size)
    window = 16384
    for offset in (0, (size - window) // 2, size - window):
        mixed[offset:offset + window] = noise[:window]
    add("mixed", mixed, "application/octet-stream")
    archive = io.BytesIO()
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as z:
        info = zipfile.ZipInfo("noise.bin", date_time=(2020, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(info, noise)
    add("zip", archive.getvalue(), "application/zip")
    # Valid grayscale PNG, containing high-entropy pixels rather than padded or
    # repeated encoded images. Each requested prefix is a real file slice.
    width = 4096
    height = (size + 4096) // width + 2
    pixels = random.Random(250025).randbytes(width * height)
    scanlines = b"".join(b"\0" + pixels[y * width:(y + 1) * width] for y in range(height))
    def png_chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    png = b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
    png += png_chunk(b"IDAT", zlib.compress(scanlines)) + png_chunk(b"IEND", b"")
    add("png", png, "image/png")
    for item in extra:
        name, path = item.split("=", 1)
        if not re.fullmatch(r"[A-Za-z0-9_-]+", name) or name in fixtures:
            raise ValueError("fixture names must be unique alphanumeric names")
        fixtures[name] = (Path(path).resolve(strict=True), "application/octet-stream")
    return fixtures


def cpu_seconds():
    r = resource.getrusage(resource.RUSAGE_SELF)
    return r.ru_utime + r.ru_stime


class PayloadPacer:
    """Shared payload-byte budget; an application pacer, not a WAN emulator."""
    def __init__(self, rate_mbps, clock=time.monotonic):
        if not math.isfinite(rate_mbps) or rate_mbps <= 0:
            raise ValueError("rate_mbps must be positive and finite")
        self.bytes_per_second = rate_mbps * 1000000 / 8
        self.clock = clock
        self.deadline = 0.0
        self.lock = threading.Lock()
        self.active = 0

    def begin(self):
        with self.lock:
            if self.active == 0:
                self.deadline = self.clock()
            self.active += 1

    def end(self):
        with self.lock:
            self.active -= 1

    def reserve(self, n):
        if n < 0:
            raise ValueError("negative payload size")
        if n == 0:
            return 0.0
        with self.lock:
            now = self.clock()
            if self.active == 0:
                self.deadline = max(now, self.deadline)
            self.deadline += n / self.bytes_per_second
            return max(0.0, self.deadline - now)


def write_paced(stream, body, pacer, sleep=time.sleep):
    if pacer is None:
        stream.write(body)
        return
    pacer.begin()
    try:
        for offset in range(0, len(body), 65536):
            block = memoryview(body)[offset:offset + 65536]
            delay = pacer.reserve(len(block))
            if delay > 0:
                sleep(delay)
            stream.write(block)
    finally:
        pacer.end()


def run_server(lib, fixtures, rate_mbps=None):
    pacer = PayloadPacer(rate_mbps) if rate_mbps is not None else None
    class Handler(http.server.BaseHTTPRequestHandler):
        # Avoid Nagle/delayed-ACK interactions with paced short writes.
        # Leave the unpaced path unchanged.
        disable_nagle_algorithm = pacer is not None
        protocol_version = "HTTP/1.0"
        def log_message(self, format, *args):
            pass
        def do_GET(self):
            params = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query, keep_blank_values=True)
            name = params["file"][0]
            path, mime = fixtures[name]
            n = int(params["n"][0]); offset = int(params.get("offset", ["0"])[0])
            encrypted = int(params["encrypted"][0]); accept = params["accept"][0]
            if (offset < 0 or n < 0 or n > 128 * 1024 * 1024 or
                    offset + n > path.stat().st_size or encrypted not in (0, 1) or
                    (n == 0 and encrypted)):
                self.send_error(400); return
            result = Response()
            if not lib.fixture_serve(os.fsencode(path), offset, n, mime.encode(), accept.encode(), encrypted, ctypes.byref(result)):
                self.send_error(500); return
            try:
                metrics = {"calls": list(result.codec_calls), "bytes": list(result.codec_bytes),
                    "ns": list(result.codec_time_ns), "headers_ok": bool(result.headers_ok),
                    "encoding": result.encoding.decode(), "encrypted": result.encrypted,
                    "uncompressed_length": result.uncompressed_length}
                self.send_response(200)
                self.send_header("Content-Length", str(result.length))
                self.send_header("X-Fixture-Metrics", json.dumps(metrics))
                self.end_headers()
                write_paced(self.wfile, ctypes.string_at(result.body, result.length), pacer)
            finally:
                lib.fixture_free(result.body)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    return server, worker


def request(url):
    start = time.perf_counter()
    with urllib.request.urlopen(url, timeout=60) as response:
        first = time.perf_counter()
        body = response.read()
        metrics = json.loads(response.headers["X-Fixture-Metrics"])
    return body, metrics, (first - start) * 1000, (time.perf_counter() - start) * 1000


def measure(lib, fixtures, sizes, concurrency, repeats, quick=False,
            rate_mbps=None, capability_sets=("br, zstd, gzip", "gzip", "")):
    server, worker = run_server(lib, fixtures, rate_mbps)
    results = []
    try:
        for name, (path, _) in fixtures.items():
            for mib in sizes:
                n = min(mib * 1024 * 1024, path.stat().st_size)
                with path.open("rb") as f:
                    expected = f.read(n)
                for caps in capability_sets:
                    for encrypted in ((0,) if quick else (0, 1)):
                        for workers in concurrency:
                            url = f"http://127.0.0.1:{server.server_port}/chunk?" + urllib.parse.urlencode({
                                "file": name, "n": n, "accept": caps, "encrypted": encrypted})
                            request(url)  # one untimed warm-up
                            batches = []
                            with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
                                for _ in range(repeats):
                                    cpu = cpu_seconds(); start = time.perf_counter()
                                    responses = list(pool.map(request, [url] * workers))
                                    elapsed = time.perf_counter() - start; cpu = cpu_seconds() - cpu
                                    # Verify after timing, so decoding is not charged to serving.
                                    for body, metrics, _, _ in responses:
                                        require(metrics["headers_ok"], metrics)
                                        require(not metrics["encoding"] or metrics["encoding"] in caps.split(", "), "unadvertised encoding")
                                        require(lib.fixture_verify(body, len(body), metrics["encoding"].encode(),
                                            encrypted, expected, n) == 1, (name, caps, encrypted))
                                    batches.append({"wall_ms": elapsed * 1000, "cpu_ms": cpu * 1000,
                                        "cpu_core_equivalents": cpu / elapsed,
                                        "throughput_mib_s": n * workers / elapsed / 1048576,
                                        "ttfb_ms": [r[2] for r in responses],
                                        "completion_ms": [r[3] for r in responses],
                                        "response_bytes": [len(r[0]) for r in responses],
                                        "codecs": [r[1] for r in responses]})
                            row = {"fixture": name, "bytes": n, "sha256": hashlib.sha256(expected).hexdigest(),
                                "capabilities": caps, "encrypted": encrypted, "concurrency": workers,
                                "payload_rate_mbps": rate_mbps,
                                "median_wall_ms": statistics.median(b["wall_ms"] for b in batches),
                                "median_cpu_ms": statistics.median(b["cpu_ms"] for b in batches),
                                "median_ttfb_ms": statistics.median(t for b in batches for t in b["ttfb_ms"]),
                                "batches": batches}
                            results.append(row)
                            print(json.dumps({k: v for k, v in row.items() if k != "batches"}), flush=True)
    finally:
        server.shutdown(); server.server_close(); worker.join()
    return results


def verify_endpoint(lib, fixtures):
    server, worker = run_server(lib, fixtures)
    count = 0
    try:
        for name in ("random", "text"):
            path, _ = fixtures[name]
            for offset, n in ((0, 0), (0, 1), (0, 1024), (0, 1025),
                              (0, 131071), (0, 131072), (0, 131073),
                              (17, 77777), (path.stat().st_size - 123, 123)):
                with path.open("rb") as f:
                    f.seek(offset)
                    expected = f.read(n)
                for caps in ("br, zstd, gzip", "zstd", "br", "gzip", ""):
                    for encrypted in (0, 1):
                        # Existing zero-length AES-GCM output-length bug is outside
                        # this compression change; tracked in the benchmark docs.
                        if n == 0 and encrypted:
                            continue
                        url = f"http://127.0.0.1:{server.server_port}/chunk?" + urllib.parse.urlencode({
                            "file": name, "n": n, "offset": offset,
                            "accept": caps, "encrypted": encrypted})
                        body, metrics, _, _ = request(url)
                        require(metrics["headers_ok"], metrics)
                        require(not metrics["encoding"] or metrics["encoding"] in caps.split(", "), "unadvertised encoding")
                        require(lib.fixture_verify(body, len(body), metrics["encoding"].encode(),
                                                  encrypted, expected, n) == 1, (name, offset, n, encrypted, caps, len(body), metrics))
                        count += 1
    finally:
        server.shutdown(); server.server_close(); worker.join()
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref", default="HEAD")
    parser.add_argument("--sizes", default="8,32")
    parser.add_argument("--concurrency", default="1,4")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--fixture", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--only-fixtures", help="comma-separated fixture names to benchmark")
    parser.add_argument("--capabilities", action="append", help="repeat for custom codec sets; empty string means raw")
    parser.add_argument("--rate-mbps", type=float, help="shared application payload pacing rate (decimal Mbps), not a WAN emulator")
    parser.add_argument("--quick", action="store_true", help="unencrypted cases only")
    parser.add_argument("--verify-only", action="store_true", help="candidate-only byte/header regression checks, no benchmark")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    sizes = [int(x) for x in args.sizes.split(",")]
    concurrency = [int(x) for x in args.concurrency.split(",")]
    if min(sizes + concurrency + [args.repeats]) < 1 or max(sizes) > 128 or max(concurrency) > 16:
        parser.error("positive sizes <=128 MiB and concurrency <=16 required")
    if args.rate_mbps is not None and (not math.isfinite(args.rate_mbps) or args.rate_mbps <= 0):
        parser.error("--rate-mbps must be positive and finite")
    capability_sets = [", ".join(t.strip() for t in raw.split(",") if t.strip())
                       for raw in (args.capabilities or ["br, zstd, gzip", "gzip", ""])]
    if any(t not in ("br", "zstd", "gzip") for caps in capability_sets for t in caps.split(", ") if t):
        parser.error("unsupported codec in --capabilities")
    base = None if args.verify_only else subprocess.check_output(["git", "rev-parse", "--verify", "--end-of-options", args.baseline_ref + "^{commit}"], cwd=ROOT, text=True).strip()
    report = {"boundary": "real production slice-response/codec/crypto, test CWIST adapters, loopback HTTP; no full-server/auth/TLS coverage", "runs": {}}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="tasfa-endpoint-") as tmp:
        tmp = Path(tmp)
        fixtures = fixture_files(tmp / "files", max(sizes), args.fixture)
        selected = args.only_fixtures.split(",") if args.only_fixtures else list(fixtures)
        if not selected or len(set(selected)) != len(selected) or any(name not in fixtures for name in selected):
            parser.error("--only-fixtures must name unique known fixtures")
        measured_fixtures = {name: fixtures[name] for name in selected}
        variants = (("candidate", None),) if args.verify_only else (("baseline", base), ("candidate", None))
        for name, revision in variants:
            lib, metadata = build(tmp / name, revision)
            checks = verify_endpoint(lib, fixtures)
            results = [] if args.verify_only else measure(lib, measured_fixtures, sizes, concurrency,
                args.repeats, args.quick, args.rate_mbps, capability_sets)
            report["runs"][name] = {"build": metadata, "integrity_checks": checks, "results": results}
            args.output.write_text(json.dumps(report, indent=2) + "\n")
            print(f"{name}: {checks} loopback header/byte integrity checks passed", flush=True)


if __name__ == "__main__":
    main()
