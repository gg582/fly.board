# TASFA chunk compression policy

This follow-up replaces the sampling policy merged in PR #26 with one fast,
full-chunk compression attempt. It addresses issue #25's redundant codec passes
without making sampling decisions that can miss useful compression elsewhere
in a chunk.

## Selection, work and wire contract

For each buffer passed to `tasfa_compress_alloc`:

1. With valid output arguments, initialize the output pointer to NULL, output
   length to zero and type to `TASFA_COMPRESS_NONE`, including on empty or invalid
   input. Missing output arguments are rejected without dereferencing them.
2. Null input, input of at most 1,024 bytes, or no advertised codec returns the
   raw outcome without any codec work.
3. Select exactly one advertised codec: **Zstd level 1 → Brotli quality 1 → gzip
   `Z_BEST_SPEED`**. The order is a preference, not a fallback chain.
4. Compress the **entire eligible input once**, with no samples and no retry.
   Successful allocation/initialization leads to exactly one codec call consuming
   the original input pointer and its complete length. Random/incompressible and
   compressible data have the same one-call/full-input budget. Allocation or
   initialization failure can prevent that call entirely.
5. Accept only if the actual encoded length plus `TASFA_COMPRESS_MIN_GAIN_BYTES`
   (1,024) is **strictly less** than the input length. Exactly 1,024 bytes of
   savings is insufficient. The implementation uses subtraction to avoid adding
   to an encoded length near the size limit.
6. Insufficient gain, allocation failure, or codec error frees temporary output
   and returns the raw outcome. **Never try a different codec**, even when more
   codecs were advertised.

The budget concerns application-level compression calls and codec input bytes,
not internal algorithm scans or total request CPU. Compression output capacity
and codec workspace scale with input size; this is not the former fixed 48 KiB
probe budget. Tails and large buffers use the same policy.

Wire enum values, decoder implementations, AES-GCM, HTTP headers and the caller
are unchanged. `session.c` still selects encoding and uncompressed-length headers
only on successful compression and derives Content-Length from the actual
raw/compressed/encrypted payload. No MIME denylist, session cache, mutable policy
state, authentication or scheduling change is introduced.

## Trade-offs and limits

Full-input evaluation fixes the old deterministic missed-sampling fixture:
random bytes in the former prefix/middle/tail windows no longer hide large
compressible regions outside those windows. It does **not** promise that every
codec would agree about compressibility. A selected codec may fail the gain test
where an untried codec or a higher level would succeed.

**There is no guarantee that output bytes are less than or equal to the old
Brotli output.** Faster levels and Zstd-first negotiation can produce larger
payloads than the old Brotli-quality-4-first policy. Incompressible inputs now
receive a full pass instead of bounded probing. Those are explicit trade-offs,
not claims of universally lower CPU, latency, wire cost or faster transfers.
Slow-link performance depends on both encoding cost and transmitted bytes.

Previously recorded sampling-policy performance numbers are superseded and have
been removed from this document; they are not evidence for this follow-up.
Current benchmark results must identify the measured source hashes and explicit
baseline revision before drawing performance conclusions.

## Native regression tests

```sh
CC=/usr/bin/clang PKG_CONFIG=/opt/homebrew/bin/pkgconf make check-tasfa-compression
CC=/usr/bin/clang PKG_CONFIG=/opt/homebrew/bin/pkgconf make check-tasfa-compression \
  TASFA_COMPRESSION_TEST_CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer'
```

The standalone test includes production `crypto.c` with a CWIST declaration shim.
It preserves real codec and AES calls except for explicit fault-injection cases.
Wrappers record codec calls, complete input bytes/pointers, requested levels,
allocation attempts and ownership cleanup. The shim's enum/minimum-gain constant
must remain aligned with `tasfa_internal.h`.

Deterministic checks cover all eight capability masks on random and compressible
inputs, zero/tiny/tail and former probe boundaries, 8/32 MiB buffers, actual
already-compressed content, and useful compression inside/outside the former
sample windows. A real-codec boundary sweep verifies equality rejection and
acceptance beyond the exact minimum gain. Compression/decompression and nonempty
AES-GCM roundtrips are byte-exact. Injected errors for every selected codec,
output allocation and gzip initialization assert clean raw results and no
fallback; counters also detect leaked/double-freed application output buffers.

Encrypted zero-length responses are deliberately excluded: the pre-existing
AES helper retains the AAD update output length when it skips an empty plaintext
update. This follow-up neither changes that helper nor claims to fix the defect.
Empty compression dispatch itself is tested.

## Endpoint and performance validation

The separate loopback adapter and `tools/benchmark_tasfa_compression.py` exercise
the production response/codec path with substituted CWIST request/response and
session plumbing. They are not a full CWIST server, authenticated routing, TLS,
browser or WAN benchmark. Endpoint/rate-limited measurements and publication are
separate from the native policy change.

Use an explicit baseline revision and record codec versions, compile flags,
source hashes, fixture hashes/actual lengths, capabilities, encryption mode and
concurrency. Compare CPU and completion/TTFB with actual response bytes, including
slow-link runs: unconstrained loopback latency alone cannot establish a transfer
improvement. Timing is supporting evidence, not a flaky native-test threshold.

Use only public/non-sensitive fixtures: the adapter exposes file slices on an
unauthenticated loopback listener and uses public synthetic keys/nonces. Never
supply private files, real session credentials or sensitive data.

### Application-level pacing

`--rate-mbps` shares one payload-byte budget across the server's concurrent
responses. It uses scheduled deadlines while responses are active, so scheduler
sleep overshoot is recovered rather than accumulated at every 64 KiB write.
When all responses finish, idle time does not create credit for the next request.
Paced connections disable Nagle; the unpaced path retains its existing behavior.
This is **not** packet shaping or a WAN emulator: HTTP/TCP overhead, RTT, packet
loss and the full production server are outside this measurement boundary.

```sh
CC=/usr/bin/clang python3 tools/benchmark_tasfa_compression.py \
  --baseline-ref 490571bff6ead50c53305734ee8581b516f90ac7 \
  --sizes 32 --concurrency 1 --repeats 3 \
  --only-fixtures mixed,text,random --capabilities 'br, zstd, gzip' \
  --rate-mbps 100 --output /tmp/tasfa-paced.json
```

Use repeated `--capabilities` flags to compare client capability sets, including
`--capabilities gzip`. The production browser probes its own
`DecompressionStream` support; **the all-codec results are not representative of
every browser**. A gzip-only client must not be assigned the Zstd speedup.
The native gate checks 236 cases; the endpoint gate checks 170 responses, and
9 Python tests cover optimized-execution failure guards and pacing behavior.

### Local measurements (2026-09-06)

macOS 26.5.1 ARM64, `/usr/bin/clang -O2`, Brotli 1.2.0, Zstd 1.5.7,
OpenSSL 3.6.3 and zlib 1.2.12. The unpaced measurements predate the pacing-only
timer fixes; the unpaced writer/socket behavior and native source hashes were
unchanged. The corrected pacing runs below were measured separately.

The candidate production source SHA-256 is
`7d06dcdb7f093983afcd3b77995cbcf58142048ef5b311ebc8a87f18f54b968e`.
The pre-sampling baseline is
`b336c1c8eb4a5655d19e1c76c341647817c5ce31`; the merged-sampling baseline is
`490571bff6ead50c53305734ee8581b516f90ac7`.

The unpaced comparison covers **192 configurations per revision**: requested
8/32 MiB, concurrency 1/4, encryption off/on, all-codec/gzip-only/no-codec, and
noise, repeated text, mixed, generated ZIP/PNG and supplied generated
JPEG/WebP/MP4 fixtures. Each configuration has one warmup and three measured
batches. Below are encrypted, concurrency-one cases at requested 32 MiB.
Payload bytes include the 16-byte AES-GCM tag; CPU times are median process CPU
milliseconds per request. These are adapter measurements, not production claims.

| Fixture / accepted codecs | Old → new CPU ms | Old → new payload bytes |
| --- | ---: | ---: |
| Noise / all | 450.78 → 16.66 | 33,554,448 → 33,554,448 |
| Mixed / all | 20.56 → 4.52 | 49,278 → 50,453 |
| Text / all | 20.96 → 3.27 | 123 → 3,163 |
| MP4 / all | 181.47 → 34.30 | 30,050,024 → 30,235,540 |
| Noise / gzip only | 441.35 → 421.26 | 33,554,448 → 33,554,448 |
| Text / gzip only | 51.52 → 25.96 | 97,754 → 227,821 |
| MP4 / gzip only | 1,425.46 → 527.22 | 29,713,592 → 30,509,037 |

The full-pass policy avoids the sampled mixed-data miss, but is not a
wire-nonincreasing replacement for the older compressor settings. In particular,
the text and MP4 rows show why CPU savings alone cannot establish a slow-link
win. Full native-server integration remains unverified: CWIST is not available
through pkg-config and the required vendor submodules are uninitialized here.

#### Corrected paced comparisons

An 8 MiB raw control at the 100 Mbps setting achieved approximately 96–99 Mbps
including request overhead. Earlier runs that accumulated sleep overshoot were
rejected. These timings still include local scheduler jitter, especially for
small compressed responses; no statistical-significance or universal-speedup
claim is made.

Against merged sampling, requested 32 MiB, all codecs, encrypted, one worker,
100 Mbps payload budget, one warmup and three measured requests per configuration:

| Fixture | Sampling → full-pass payload bytes | Median batch wall ms (one worker) |
| --- | ---: | ---: |
| Mixed | 33,554,448 → 50,453 | 2,722.78 → 17.20 |
| Noise | 33,554,448 → 33,554,448 | 2,717.88 → 2,715.91 |
| MP4 | 30,050,024 → 30,235,540 | 2,593.33 → 2,459.20 |

This matrix contains eight configurations per revision: four fixtures and both
encryption modes. It demonstrates recovery of the sampled mixed-data miss,
**not** that a full pass is cheaper than sampling on truly incompressible data.

Against the pre-sampling policy, requested 8 MiB, encrypted, one worker,
20 Mbps payload budget, one warmup and two measured requests per configuration:

| Fixture / accepted codecs | Old → new payload bytes | Median batch wall ms (one worker) |
| --- | ---: | ---: |
| Mixed / all | 16,437 → 49,685 | 46.00 → 165.63 |
| Text / gzip only | 24,517 → 57,042 | 98.27 → 190.34 |
| MP4 / gzip only | 7,428,573 → 7,627,400 | 3,421.84 → 3,225.13 |

This matrix contains 16 configurations per revision: four fixtures, two
capability sets and both encryption modes. The mixed fixture has repeated noisy
windows that a stronger/larger-window codec can exploit: whole-input inspection
does not imply the same compression ratio. The slower mixed/text observations
are retained rather than selecting only wins. They show the remaining trade-off;
their small-response timing deltas should not be extrapolated to a real WAN.
