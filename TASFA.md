# TASFA

TASFA is the file-transfer protocol used in this tree for uploads and downloads.

It is built on top of plain HTTP/XHR and adds per-chunk encryption, a file-level sequential queue, and a **server-authoritative 6-slot HTP (Hexagonal Tortoise Problem) recovery lattice** for integrity control.

## Routes

Upload:
- `POST /file/upload/init`
- `POST /file/upload/status`
- `POST /file/upload/renegotiate`
- `POST /file/upload`
- `POST /file/upload/complete`
- `POST /file/upload/cancel`

Download:
- `GET /file/download/:id/handshake`
- `GET /file/download/:id/chunk/:chunk_index`
- `GET /file/download/:id`
- `GET /assets/tasfa/img/:filename/handshake`
- `GET /assets/tasfa/img/:filename/chunk/:chunk_index`
- `GET /assets/tasfa/uploads/:filename/handshake`
- `GET /assets/tasfa/uploads/:filename/chunk/:chunk_index`

## Upload Protocol

The browser negotiates an upload session first, then sends **chunks** (default `16 MiB`, mobile `8 MiB`) with TASFA headers:

- `X-TASFA-Upload-ID`
- `X-TASFA-Upload-Token`
- `X-TASFA-Chunk-Index`
- `X-TASFA-Hash-Tag`
- `X-TASFA-Raw-Scalar`
- `X-TASFA-Magic-Scalar`

The server writes each chunk directly to the preallocated temp file at `chunk_index * chunk_size`. There are no transport blocks anymore.

If a normal chunk repeatedly fails, the browser sends that failed chunk through an AES-256-GCM fallback request with `X-TASFA-Stream-Mode: aes-256-gcm`. The server accepts the ciphertext plus the GCM authentication tag, and fallback chunks remain inside the adaptive parallel window. The fallback still carries the same HTP hash tag and balanced scalar headers.

### XOR parity chunks

Upload sessions carry one extra **parity chunk per group of up to 6 data chunks**: `chunk_count = data_chunks + ceil(data_chunks / 6)`. The parity chunk holds `P = C_0 xor ... xor C_n` for its group and is stored separately as `parity_<k>.bin` in the session directory instead of at `chunk_index * chunk_size`. Parity chunks count toward the received-chunk bitmap but are excluded from HTP line validation; they feed the server-side peeling recovery described below.

### Remainder (last partial chunk)

If the file size is not a multiple of the chunk size, the last chunk is a **remainder**. It is sent as a single blob with its exact byte range; the server writes it at the correct offset. No padding or splitting is performed.

### Session init response

The `init` endpoint returns, among other fields:

- `chunk_size` — negotiated chunk size
- `current_parallel_chunks` — the server-approved active upload window
- `max_parallel_chunks` — how many chunks the client may upload concurrently
- `dispatch_pacing_ms` — a small pacing delay only when the measured link is poor
- `upload_secret` — a secondary server-secret for encrypted stream verification
- `stream_key_hex`, `stream_iv_seed_hex`, `stream_mode` — session encryption keys (`aes-256-gcm`)
- `modulus_M` — the HTP modulus used for this session
- `group_count` — number of complete 6-slot HTP groups
- `client_stripes` — fixed value `32`, used by the client worker scheduler

The client treats `chunk_size` as a session contract because it defines each chunk's file offset. During an active upload it tunes concurrency and retry behavior, while the learned chunk-size hint is applied to the next TASFA session: after a sustained fast streak (3 clean chunks at `>= 8 Mbps` EWMA or sub-30-second chunk times) the next hint is doubled, and after failures it is reduced by a quarter (`* 0.75`, rounded to `1 MiB` steps, `8 MiB` floor).

### Upload chunk response

When a chunk is accepted, the server responds with `204 No Content` and headers:

- `X-TASFA-Accepted: 1`
- `X-TASFA-Chunk-Complete: 1`

If the chunk index is already marked complete in `state.bin` **and** the index is not in the server's `retry_targets`, the server returns `204` with the same headers but the chunk body is discarded.

## HTP Server-Authoritative Recovery Lattice

HTP is **not a transport protocol** and **not a cryptographic proof**. It is a server-side chunk-suspicion engine that ranks chunks by likelihood of corruption so the client only retransmits high-probability suspects instead of the whole file.

### Chunk grouping

Chunks are grouped into consecutive 6-element vertices:

```
Group g: [ v0 , v1 , v2 , v3 , v4 , v5 ]
         chunk g*6+0  ...  g*6+5
```

The last group may be partial; **incomplete groups are never padded and are excluded from HTP validation entirely**. Zero-padding would inject synthetic topology and is forbidden.

### Raw scalar

For every chunk the client computes the SHA-512 of the plaintext chunk, takes the first 8 bytes as a big-endian unsigned 64-bit integer `H`, and derives:

```
raw_scalar = H % modulus_M
```

### Magic line invariant

For a complete group, define three lines:

```
L1 = v0 + v1 + v2   (mod M)
L2 = v2 + v3 + v4   (mod M)
L3 = v4 + v5 + v0   (mod M)
```

The invariant requires `L1 == L2 == L3`. If the raw scalars do not satisfy this, the client **balances** them by adjusting only `v3` and `v5`:

```
delta2 = (L1 - L2) mod M
delta3 = (L1 - L3) mod M

v3_balanced = (v3_raw + delta2) mod M
v5_balanced = (v5_raw + delta3) mod M
```

All other vertices keep their raw scalar. The client sends both:

- `X-TASFA-Raw-Scalar` — the unmodified `raw_scalar`
- `X-TASFA-Magic-Scalar` — the balanced value (`v3_balanced` or `v5_balanced`, otherwise same as raw)

The server stores both scalars separately in `htp.bin` so that analysis can reference the original topology independently of the artificial balancing constraints.

### Why only v3 and v5?

The hexagonal lattice has two degrees of freedom. Fixing `v0,v1,v2,v4` and adjusting `v3,v5` uniquely satisfies the three line equations while keeping the delta minimal and local to the group.

## Server-Authoritative HTP Recovery

**The client is a dumb retransmission agent.** It does not compute repair algebra, does not evaluate cost thresholds, and does not derive suspect rankings. All of that lives on the server.

### Server validation flow

During `POST /file/upload/complete` the server:

1. Loads all per-chunk records from `htp.bin` (hash tag, raw scalar, and balanced scalar).
2. Validates only **complete 6-slot groups** (partial groups are skipped).
3. If the scalar order inside a 6-slot group is permuted (out-of-order), the server searches all 720 permutations, finds one that restores the invariant, and reorders the `htp.bin` records accordingly without requiring retransmission.
4. For every failing group, computes **suspicion scores** per slot by analyzing which line equations each slot participates in.

### Suspect confidence scoring (per group)

For a failing group the server evaluates each slot against the three line equations:

| Slot | Equations |
|------|-----------|
| v0   | L1, L3    |
| v1   | L1        |
| v2   | L1, L2    |
| v3   | L2        |
| v4   | L2, L3    |
| v5   | L3        |

The suspicion score for each slot is derived from topological syndrome analysis:
- When exactly two lines agree and one differs ($L_a == L_b \neq L_c$), the failing equation isolates two candidates: the exclusive side slot on $L_c$ (score `1.0`, dominant syndrome) and the opposite corner slot shared by $L_a, L_b$ (score `0.5`).
- The remaining 4 slots that do not touch the failed equation are **cleared** immediately (score `0.0`).
- When all three lines differ ($L_1 \neq L_2 \neq L_3$), scores are proportional to participation in the normalized residual magnitudes $|L_i - L_j|$.

Scores are aggregated across all failed groups; if a chunk appears in multiple groups, its maximum score is kept.

### Full HTP-XOR Peeling Decoder

To resolve chunk corruption or loss without expensive Galois Field $GF(2^8)$ matrix inversions, TASFA combines topological syndrome localization with an iterative peeling XOR decoder:
- **XOR Parity Generation**: Every group of up to 6 data chunks has a parity chunk $P = C_0 \oplus C_1 \oplus C_2 \oplus C_3 \oplus C_4 \oplus C_5$ uploaded with the session.
- **Zero-Cost Suspect Localization**: When a group fails integrity checks, HTP's line sums pinpoint the single corrupted chunk index $i$ in $O(1)$ integer operations.
- **Bounded Iterative Peeling Recovery**: The server reconstructs $C_i = P \oplus \bigoplus_{j \neq i} C_j$ using SIMD bitwise XOR. A group is recovered when it has exactly one suspect, or two suspects where the top score is `1.0` (dominant single-error syndrome); all suspects of a recovered group are then cleared. The scan repeats until no group makes progress, with at most 6 passes.
- **Missing-Chunk Reconstruction**: Independently of suspicion analysis, if the completion bitmap shows exactly one missing data chunk in a group and that group's parity chunk has been received, the server reconstructs the missing chunk the same way before validating HTP lines.
- **Multi-Erasure Fallback**: If two or more independent chunks in the same group cannot be isolated, the server falls back to ARQ retransmission via `retry_targets`.

### Repair cost threshold

Before requesting any repair, the server evaluates whether contraction is cheaper than direct retry:

```
repair_worthwhile(suspect_count, total_chunks, chunk_size, rtt_ms):
    if suspect_count < 12               → false  (too few for topology)
    retry_cost  = suspect_count * chunk_size * rtt_factor(rtt_ms)
    repair_cost = metadata_bytes + server_cpu_cost + extra_rtt_cost
    return retry_cost > repair_cost
```

The abstract cost model compares the bytes the client would retransmit against the server-side analysis overhead. Large chunks or high latency make contraction more attractive, while many small suspects make direct retry cheaper. Concrete numbers are server-side implementation details, not protocol constants.

If the threshold rejects repair, the server returns `needs_retry` with **all** suspect chunks as retry targets. The client retransmits them through the normal upload endpoint.

### Server-side recursive contraction

If repair is worthwhile, the server performs **group-level contraction**: each original complete 6-slot group is collapsed to a single scalar encoding its **invariant signature**. The server computes the three line values `L1, L2, L3` for the group, derives the residuals `r12 = (L1-L2) mod M` and `r23 = (L2-L3) mod M`, and sets the group aggregate to `(r12 * r23) mod M`. A passing group has `r12 = r23 = 0`, so its aggregate is `0`; a failing group receives a non-zero deterministic signature that preserves the topology of the line inconsistencies. These group aggregates become the vertices of a higher-level HTP lattice. Consecutive groups of 6 such aggregates form level-1 super-groups, and the same line invariants are re-evaluated:

- If a level-1 super-group passes, suspects from its underlying level-0 groups are cleared (the failure pattern is consistent at the group level).
- If a level-1 super-group fails, suspects from its underlying level-0 groups are kept.
- If contraction narrows the suspect set (fewer chunks), the server stores the narrowed targets and returns `needs_retry` with the reduced list.
- If contraction does not narrow the set, the server falls back to direct retry of the original suspects.
- Contraction level is incremented in session metadata so the client can report diagnostics.

The client never sees contraction groups or computes them. It only receives `retry_targets`.

### Retransmission acceptance

When the client retransmits a chunk that is already marked received, the normal upload endpoint **accepts the retransmission only if that chunk index is currently in the server's `retry_targets` list**. After the retransmitted chunk is stored, the server removes it from `retry_targets`.

### Protocol-visible repair response

When HTP fails and the server decides repair or retry is needed, the `complete` endpoint returns `409` with:

- `htp_status`: `"needs_retry"`
- `retry_targets`: array of chunk indices to retransmit (ordered by suspicion score descending)
- `suspicion_scores`: array of `{chunk_index, score}` objects
- `contraction_level`: how many server-side contraction passes have been applied
- `retry_reason`: human-readable explanation (e.g. `"htp group inconsistency detected"`)

If the cost threshold says direct retry is cheaper, `retry_targets` contains the full suspect list and `contraction_level` stays at `0`.

If the server successfully narrows suspects through contraction, `retry_targets` contains the narrowed list and `contraction_level` is incremented.

After all suspects are retransmitted and validated successfully, the next `complete` call proceeds to SHA-256 finalization.

## File-Level Sequential Queue

Only **one file is uploaded at a time**. When multiple files are selected:

1. Each file gets its own asset, preview card, and HTP session.
2. Files are enqueued in `FileUploadQueue`.
3. When the active file finishes (success or failure), the queue automatically advances to the next file.
4. The batch "Upload queued files" button is always enabled; clicking it enqueues all pending files and starts the pump.

This prevents browser connection pool exhaustion and keeps stall detection reliable.

## Runtime Settings

- upload chunk size: `16 MiB` desktop, `8 MiB` mobile
- adaptive upload chunk-size hint: `8 MiB` minimum, up to `32 MiB` desktop / `16 MiB` mobile at the server negotiation (client preference may reach `64 MiB` / `32 MiB`); success doubles the hint after a fast streak, failure reduces it by `* 0.75` in `1 MiB` steps
- download chunk size: `8 MiB` desktop, `6 MiB` mobile, up to `32 MiB` when the client hints a larger session
- default browser upload parallelism: `8`, success `*1.15` (`*1.3` during a fast-recovery window), failure `*0.85`
- max browser upload parallelism: `max_upload_parallel_chunks` in `blog.settings`, capped at `40`
- max concurrent upload sessions: `max_total_parallel_uploads` in `blog.settings`, capped at `512`
- max upload size: `max_upload_size` in `blog.settings`
- max browser download sessions: server-defined, currently up to `48` chunk requests per session
- download coalesce (span group size): success `*1.2`, failure `*0.8`; the server profile hints up to `16` chunks, and the client span may grow to `64` chunks bounded by a `128 MiB` per-request cap
- image previews: request a DPR-aware 128px size bucket (minimum `256px`, maximum `1920px`) so image transfers do not inflate to a desktop-sized preview when rendered small
- video/audio playback: prewarm only the TASFA session handshake on pointer/focus intent, then retain native range-request playback; no media payload is fetched early and the progressive scheduler remains unchanged
- upload xhr timeout: adaptive idle watchdog scaled by transfer size, `30-120 s`
- upload session fetch timeout: `120 s`

TASFA is tuned for general high-bandwidth server deployments, not an embedded/aerospace low-bandwidth profile. The browser's per-origin HTTP connection limit is still respected naturally by the worker pool.

### Client adaptation

The upload client measures chunk completion time, retries, timeouts, and Network Information API hints when available. It sends those inputs to `/file/upload/init` and `/file/upload/renegotiate`; the server answers with a current parallel window and max window. Clean completions increase the active window by `*1.15` up to the negotiated max (`*1.3` inside a 5-second fast-recovery window right after a drop), while repeated, predictable failure patterns reduce it by `*0.85` down to a floor of `4` mobile / `8` desktop on good links. AES-GCM fallback also runs inside the adaptive window.

A per-chunk absolute timeout prevents stalls caused by connections that receive partial data but never close. A watchdog samples upload activity every 2 seconds: at a soft stall (`30 s` without chunk progress) it performs **soft recovery** — renegotiation, a parallelism bump, and a 5-second fast-recovery window — without tearing down live connections; at a hard stall (`90 s`) it performs an **aggressive restart**: in-flight XHRs are aborted and the upload resumes from the server bitmap, renegotiating from the full `maxParallel` window (high-performance transfer profiles also reset `targetParallel` to `maxParallel` and `dispatchPacingMs` to `0`). The learned chunk-size hint is kept **session-local** and is not immediately persisted to `localStorage`; it is only applied to the next TASFA session.

Download uses the same high-throughput bias: the handshake carries the client's preferred chunk-size hint, and active downloads grow `span` and parallelism by `*1.2` after successful chunk groups. On failure both are reduced by `*0.8`. Short reads, timeouts, and network errors are requeued by chunk index; a failed group does not fail the whole download until the same chunk has exhausted a high retry budget.

For progressive video/audio playback, TASFA is stricter: the client assembles and feeds chunks sequentially, while a bounded scheduler keeps adaptive parallelism only inside a forward window from the next chunk the player needs. Failed or timed-out chunks record a retry time and release the network slot, so the scheduler can keep filling other eligible chunks in the same window. Playback can stall at the buffer edge, but it never skips a byte range, leaves a missing chunk unresolved, or closes the media stream early.

### Tail RTT Prediction (Lagrange Extrapolation)

For large files (sessions with many chunks) the server accumulates per-chunk RTT samples reported by the client, up to a maximum of 8. Once 3 or more samples have been collected, a Lagrange polynomial extrapolation estimates the RTT at the last chunk index, and the remaining chunk count is multiplied to derive the estimated time left.

- Upload: after each chunk finishes, the client may attach an `X-TASFA-Chunk-RTT` header (milliseconds) to the next chunk request.
- Download: the client may separately report the previous chunk's RTT via the `chunk_rtt_ms` query parameter.

The server adds `predicted_remaining_ms` to the status response JSON, and the download chunk response carries an `X-TASFA-Predicted-Remaining-Ms` header. Math is intentionally kept light: sample cap of 8, simple O(n²) Lagrange, and predictions are clipped to a 30-second ceiling.

### Wynn-Accelerated Predictive Guard

TASFA does not predict plaintext bytes or trust predicted chunk contents. Prediction is limited to the transfer-quality sequence observed by the client: completion time, throughput, retries, timeouts, and short reads. The client applies the Wynn epsilon transform in its Aitken `Delta^2` form to the latest quality samples:

```
S_hat = S0 - ((S1 - S0)^2 / (S2 - 2*S1 + S0))
```

`S_hat` is clamped to `[0, 1]` and used only as a guard signal for the next chunk. High predicted quality keeps the normal path and lets the adaptive window grow. Medium-low predicted quality marks the next chunk as `guarded`, trimming the active parallel window by one before dispatch. Very low predicted quality marks the next upload chunk for parallel-capable AES-GCM fallback, or trims download `span` and parallelism before issuing the next range group. This is a forward-looking congestion and failure defense, not a cryptographic oracle.

## Storage Model

Each upload session preallocates one temporary file:

- temp file: `data/tasfa/uploads/<upload_id>/upload.bin.part`
- metadata: `data/tasfa/uploads/<upload_id>/meta.json`
- fast binary meta: `data/tasfa/uploads/<upload_id>/meta.bin`
- state: `data/tasfa/uploads/<upload_id>/state.json`, `data/tasfa/uploads/<upload_id>/state.bin`
- HTP level-0: `data/tasfa/uploads/<upload_id>/htp.bin`
- session lock: `data/tasfa/uploads/<upload_id>/session.lock`

The server no longer maintains `blocks.bin` or `chunk_counts.bin`. Chunk completion is a single bitmap write.

### Optional S3-Compatible Object Storage

Completed files can additionally be stored in an S3-compatible bucket configured through the optional `s3.settings` file (all-empty means disabled and local disk storage is unchanged). In `mode=mirror` (default) the local file is kept and also PUT to the bucket; downloads keep serving from disk. In `mode=offload` the bytes are moved to S3 after upload, the file row is marked `s3://<key>`, and downloads are answered with a `302` redirect to a 1-hour presigned GET URL. Failed S3 PUTs fall back to local storage, and all delete paths clean both copies. See `S3.md` for the full key reference.

Session metadata also stores per-vertex arrays:

- `hash_tags` — array of SHA-512 hex strings, one per chunk
- `magic_scalars` — array of balanced scalars, one per chunk
- `htp_retry_targets` — current server-issued retry target list
- `htp_suspicion_scores` — current suspicion ranking
- `htp_contraction_level` — number of server-side contraction passes applied

These are updated on every chunk upload and validated at completion.

## Delete PIN

Completed uploads receive a one-time delete PIN. The clear PIN is a 12-character hex string (6 random bytes); it is returned once and only its hash is stored.

## Download Protocol

1. Client requests a handshake via `GET /.../handshake`.
2. Server returns `session_id`, `session_token`, `chunk_size`, `chunk_count`, concurrency hints, and session encryption keys (`stream_key_hex`, `stream_iv_seed_hex`).
3. Client fetches chunk groups with an adaptive `span=...`. All chunks are encrypted with **AES-256-GCM** when session keys are present.
4. Client decrypts chunks in the browser using the **Web Crypto API** and the session keys.
5. Browser assembles the response into one contiguous buffer.
6. **Payload Compression (Zstd/Brotli/Gzip Fallback)**:
   During downlinks, the client advertises supported decompression algorithms using the `X-TASFA-Accept-Encoding` header (e.g. `br, zstd, gzip`; the server falls back to `Accept-Encoding` when the TASFA header is absent). The header is a capability list, not a priority order: the server applies compression **before** GCM encryption using exactly one full-buffer pass of the supported codec it prefers most, ordered **Zstd (level 1) → Brotli (quality 1) → gzip (best speed)**; it is a preference order, not a fallback chain — if the selected codec does not save enough bytes, the raw payload is served and no other codec is tried. This avoids double-compression and ensures clients only receive payloads they can decompress natively (using `DecompressionStream`). If compression does not yield enough gains (`TASFA_COMPRESS_MIN_GAIN_BYTES`, 1,024 bytes; strictly less is required), the server skips compression and serves the raw binary payload directly. Compressed chunk responses carry `X-TASFA-Content-Encoding` and `X-TASFA-Uncompressed-Length` so the client can decompress before decryption.

## Optional WASM-Sandboxed Block Crypto

Block AES-256-GCM (chunk encrypt/decrypt) normally runs through the native OpenSSL EVP path. When the `TASFA_CRYPTO_WASM` environment variable points at a built WASI module (`make wasm-tasfa-crypto` produces `build-wasm/tasfa_crypto.wasm`), the server instead runs each block operation inside a sandbox: it spawns `wasmtime run <module>` as a subprocess and exchanges one framed request/response (protocol in `wasm/tasfa_crypto_protocol.h`) over stdin/stdout. The module performs the same IV derivation and AAD layout as the native path, and byte parity with OpenSSL EVP is enforced by `wasm/test_parity.py`. The backend is strictly opt-in: if `TASFA_CRYPTO_WASM` is unset or the module is unreadable, `tasfa_wasm_crypto_available()` returns false and the native path is used. The `wasmtime` binary can be overridden with `TASFA_WASMTIME`. This changes only where chunk crypto executes, never the wire format.

## DoS Mitigation via Bitmap

Both upload and download state are tracked with a **dense binary bitmap** (one byte per chunk, `'0'` / `'1'`).

### Upload side

- The server rejects any chunk whose index is already marked `'1'` in `state.bin`, **unless the chunk is explicitly in the server's retry target list**. An attacker cannot replay arbitrary chunks to burn disk I/O.
- `state.bin` is updated with `pwrite(..., 1, chunk_index)` — an O(1) atomic write. There is no JSON parsing on the hot path.
- The `complete` handler re-opens the bitmap, counts set bits, and refuses finalisation until `received_chunks == chunk_count`. This prevents truncated-file attacks.

### Session hardening

- Upload IDs and tokens are 16-byte / 24-byte random hex strings.
- Session locks (`flock` on `session.lock`) prevent racing bitmap updates from concurrent requests.

## Worker Scheduler

The server uses a sticky round-robin worker scheduler. The number of workers equals the CPU count (minimum 2, maximum 64). Chunks belonging to the same `upload_id` are always dispatched to the same worker for cache locality and ordering. Jobs are submitted via a per-worker queue and signalled with condition variables.

## Asynchronous Finalization

`POST /file/upload/complete` is processed asynchronously. The first call returns `202 Accepted` with `{"processing": true}` and schedules a background finalize worker on the engine pool, in an isolated `TTAK_TASK_DOMAIN_THREAD` scheduling domain so finalize work (media transcodes, SHA-256 checksum, database writes) never shares workers with the HTTP service loops. If the pool rejects the job (e.g. it is shutting down), the finalize record is marked `503` and the client retries instead of pinning an HTTP worker. Subsequent `complete` calls poll the finalize cache; when the worker finishes, the cached status and body are returned immediately. This prevents long-running finalization work from blocking the HTTP connection.

## Self-Review Checklist

| Question | Answer |
|----------|--------|
| Q1: Does the client compute any repair algebra? | **No.** The client is a dumb retransmission agent. All suspect derivation, confidence scoring, cost thresholds, and contraction logic are server-side only. |
| Q2: Is repair cost threshold explicitly evaluated before contraction? | **Yes.** `htp_repair_worthwhile` compares `retry_cost_estimate` (bytes × RTT) against `repair_cost_estimate` (server analysis overhead). It rejects repair when `suspect_count < 12` or retry is cheaper, falling back to direct retry. |
| Q3: Are partial groups ever zero-padded? | **No.** Only complete 6-slot groups (`data_chunks / 6`) are validated. Incomplete final groups are excluded entirely. |
| Q4: Does the response contain suspicion scores, not just binary flags? | **Yes.** Every `needs_retry` response includes `suspicion_scores` as `{chunk_index, score}` objects. |
| Q5: Does contraction preserve original group topology? | **Yes.** `htp_contract_groups` treats each original complete group as a single higher-level vertex; suspects are never reshuffled across groups. |
| Q6: Are retry targets cleared on successful retransmission? | **Yes.** `handler_file_upload` removes the chunk from `htp_retry_targets` after accepting a retry retransmission. |
| Q7: Does the server repair out-of-order scalars by swapping? | **Yes.** `htp_try_swap_repair` brute-forces all 720 permutations of a 6-slot group; if any permutation restores the invariant, the `htp.bin` records are reordered immediately and the group passes without retransmission. |
