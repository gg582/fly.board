# TASFA chunk compression policy

`tasfa_compress_alloc` uses a bounded content probe before compressing buffers
of **128 KiB or larger**. This addresses issue #25's redundant whole-buffer
codec passes without changing the download/session or encryption APIs.

## Work and wire guarantees

- If no codec is advertised, or the input is at most the 1,024-byte minimum
  gain, no codec work is done.
- Large inputs are probed with Zstd level 1 using independent 16 KiB windows
  at the beginning, middle and end. Sampling stops at the first window whose
  encoded size plus 32 bytes is smaller than the window, or on a probe error.
  An error retains the old full-compression path rather than deciding that the
  content is incompressible.
- If all three windows fail that savings test, the dispatcher returns the
  original-data outcome: `false`, NULL output, zero output length and
  `TASFA_COMPRESS_NONE`. **At most three codec calls and 48 KiB of codec input**
  are used for these rejected large buffers, regardless of chunk/span size.
  The output scratch buffer is fixed-size (`ZSTD_COMPRESSBOUND(16 KiB)`);
  the probe does not allocate a request-sized buffer or concatenate samples.
- The Zstd probe is an internal heuristic, **not a transmitted encoding**.
  It also runs for gzip-only and Brotli-only clients; Zstd is already a required
  server dependency. Full-buffer/wire selection remains advertised
  **Brotli → Zstd → gzip**, with the unchanged strict 1,024-byte savings check.
- Smaller tails retain the old full-codec fallback policy. They can still
  incur up to three full passes, but each input is smaller than 128 KiB.
- A promising probe does **not** bound subsequent full-buffer work. It adds
  up to 48 KiB of probe work to the old policy. File reads, response copying,
  encryption and other request work are not covered by the compression bound.

There is no MIME denylist, session cache, mutable global policy state or
change to authentication, AES-GCM, HTP or scheduling. `session.c` still selects
encoding and uncompressed-length headers only on successful compression and
sets Content-Length from the actual plaintext/compressed/encrypted payload.

## False negatives and costs

Sampling is not proof of incompressibility. A file with random sampled windows
and a large compressible region elsewhere will be sent raw; repetition that
requires a dictionary larger than a sample, or that another codec exploits but
Zstd does not, can also be missed. Even savings smaller than 33 bytes per sample
can be worthwhile over a large file. Conversely, a compressible header/padding
window can admit a mostly incompressible buffer and all old full passes still
run. Any promising window is enough to favor preserving compression rather than
requiring every window to pass. Independent windows avoid manufacturing repeated
content by stitching samples together.

The selected constants trade a small fixed probe cost for avoiding full passes
on sampled high-entropy content. They are not a universal CPU/latency optimum.
Fast links may favor skipping more compression, while slow links may make false
negatives expensive. Endpoint timing is supporting evidence under its measured
transport and concurrency, not a claim of production or WAN throughput gains.

## Regression tests

```sh
make check-tasfa-compression
# Apple Silicon with Homebrew dependencies:
make check-tasfa-compression CC=/usr/bin/clang PKG_CONFIG=/opt/homebrew/bin/pkgconf
# Optional memory/undefined-behavior checks:
make check-tasfa-compression CC=/usr/bin/clang PKG_CONFIG=/opt/homebrew/bin/pkgconf \
  TASFA_COMPRESSION_TEST_CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined'
```

The standalone test includes the actual production `crypto.c`, substitutes only
its CWIST umbrella declarations/allocator, and wraps codec entry points to count
calls and input bytes while executing the real codec libraries. The small test
shim mirrors the compression enum/minimum-gain constant from `tasfa_internal.h`;
it must stay aligned if those declarations change. No server/CWIST archive is
needed. Deterministic assertions cover sample/tail boundaries, 8/32 MiB inputs,
all eight capability combinations (including none, gzip-only and all-codec),
byte-exact compress/decompress and AES-GCM encrypt/decrypt paths. Unit assertions
use work counts, not flaky timing thresholds. This is not a full native-server
integration test; response-header and endpoint measurements use the separate
loopback adapter below or a running CWIST server.

## Loopback response tests and reproducible benchmark

```sh
make check-tasfa-endpoint CC=/usr/bin/clang PKG_CONFIG=/opt/homebrew/bin/pkgconf
CC=/usr/bin/clang PKG_CONFIG=/opt/homebrew/bin/pkgconf \
  python3 tools/benchmark_tasfa_compression.py --baseline-ref HEAD \
  --sizes 8,32 --concurrency 1,4 --repeats 3 --output /tmp/tasfa-comparison.json
```

Use an explicit pre-fix commit for `--baseline-ref` after the change is committed.
Additional real media can be supplied as `--fixture jpeg=/path/to/image.jpg`
(and similarly WebP/video); the report records the actual sliced size and SHA-256,
not a padded/repeated nominal size. Default controls are deterministic random
bytes, repeated text, valid generated ZIP/PNG data, and an adversarial mixed
buffer with compressible content outside the sampled windows. Codec versions,
compile flags, and source hashes are included in the JSON output.

Use only public/non-sensitive fixtures: `--fixture` exposes the selected file
slices on an unauthenticated loopback listener. Synthetic fixed keys/nonces are
public test constants and provide no confidentiality; never use real session
credentials or private files. The adversarial mixed fixture is arranged for the
largest requested size, so in the default 8/32 MiB matrix only its 32 MiB row is
the deliberate missed-compression case.

The harness compiles the complete production `crypto.c` and the unmodified
`send_file_slice_response` function body from each revision. Only CWIST request,
response, session metadata and allocation dependencies are test substitutes.
The function serves real file slices over loopback HTTP, with optional real
AES-GCM. It checks encoding negotiation, compressed/encrypted Content-Length,
uncompressed-length headers and exact bytes after decryption/decompression.
Small chunks, threshold boundaries, nonzero offsets and partial tails are
covered by `--verify-only` / `make check-tasfa-endpoint` (102 cases).

One existing boundary is deliberately excluded: encrypted zero-length responses.
The pre-existing encryption helper retains the AAD update's output-length value
when it skips the empty plaintext update. The compression change does not alter
that helper. Empty **unencrypted** responses are tested; this harness does not
claim coverage or a fix for encrypted empty files.

Each benchmark cell has a warm-up and configurable repeated batches. It reports
request TTFB/completion, process CPU time, codec calls/input bytes/timing,
logical throughput and response byte counts (payload-wire throughput is derivable
from bytes/elapsed time and excludes HTTP/TCP overhead). Process CPU includes both the adapter and local client;
cryptographic round-trip validation occurs outside the timed batch. This is a
measurement of the actual response/codec path behind test adapters, **not a full
CWIST server, real authentication/routing, TLS, browser, or WAN benchmark**.
Tests assert byte/work contracts rather than wall-clock thresholds. Read timing
medians alongside output sizes, particularly for the deliberately unfavorable
mixed-data control; sampling can trade higher wire cost for lower codec CPU.

### Recorded comparison (2026-09-06)

macOS ARM64, Clang `-O2`, Brotli 1.2.0, Zstd 1.5.7, OpenSSL 3.6.3, zlib 1.2.12.
Base `3810f05afd24c51bfb96e8e0c43e46cfe1f450e1`; candidate crypto source SHA-256
`7b8c86edd7b0bb2ff6a5176a33806ccbd2c130180c50c6f3b9628b784e687ee7`.
The complete run had **192 configurations per revision**: eight fixtures,
two requested sizes, three capability sets, encryption off/on, concurrency 1/4.
Each cell had one warm-up and three measured batches. Both revisions also passed
102 response-header/byte checks. Baseline cells ran first; do not interpret small
percentage differences as statistically established gains. This run preceded
reporting-metadata/validation-guard additions to the harness; its production
crypto and response source hashes match the final candidate.

Representative all-codec, AES-GCM, concurrency-1 cells (medians, milliseconds):

| Fixture | Size | CPU before → after | TTFB before → after | Payload bytes before → after |
| --- | --- | --- | --- | --- |
| Random | 32 MiB | 454.47 → 14.80 | 445.41 → 6.83 | 33,554,448 → 33,554,448 |
| ZIP | 32 MiB | 457.79 → 13.64 | 448.03 → 6.29 | 33,554,448 → 33,554,448 |
| PNG | 32 MiB | 461.37 → 14.16 | 452.00 → 6.44 | 33,554,448 → 33,554,448 |
| WebP | 32 MiB | 469.01 → 13.38 | 459.72 → 6.19 | 33,554,448 → 33,554,448 |
| Text | 32 MiB | 20.79 → 20.75 | 20.73 → 20.70 | 123 → 123 |
| JPEG | 33,163,096 bytes | 135.67 → 134.23 | 127.06 → 125.48 | 32,041,654 → 32,041,654 |
| MP4 | 32 MiB | 198.82 → 184.45 | 191.06 → 176.28 | 30,050,024 → 30,050,024 |
| Adversarial mixed | 32 MiB | 20.61 → 14.06 | 20.57 → 6.40 | **49,278 → 33,554,448** |

Random/ZIP/PNG/WebP avoided three 32 MiB codec inputs (96 MiB total), using only
48 KiB of Zstd probe input. Text, JPEG and MP4 still selected Brotli and retained
identical output sizes. The actual JPEG/WebP/MP4 files were generated from a
4096×4096 deterministic noise PPM (Python `random.Random(2500).randbytes`):
`cjpeg -quality 100`, `cwebp -lossless -z 0`, and one-frame FFmpeg/libx264
`-preset ultrafast -qp 0`, respectively. These are synthetic encoded media, not
a representative media corpus.

**The mixed cell is an important regression in wire cost**, despite looking
faster on an unconstrained loopback connection. A slow real link can perform
much worse on such data. The policy is a measured heuristic, not a guarantee
that every compressible file retains compression or every transfer is faster.
