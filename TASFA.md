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
- `GET /assets/tasfa/img/:filename/handshake`
- `GET /assets/tasfa/img/:filename/chunk/:chunk_index`
- `GET /assets/tasfa/uploads/:filename/handshake`
- `GET /assets/tasfa/uploads/:filename/chunk/:chunk_index`

## Upload Protocol

The browser negotiates an upload session first, then sends **chunks** (default `8 MiB`, mobile `4 MiB`) with TASFA headers:

- `X-TASFA-Upload-ID`
- `X-TASFA-Upload-Token`
- `X-TASFA-Chunk-Index`
- `X-TASFA-Hash-Tag`
- `X-TASFA-Magic-Scalar`

The server writes each chunk directly to the preallocated temp file at `chunk_index * chunk_size`. There are no transport blocks anymore.

If a normal chunk repeatedly fails, the browser serializes that failed chunk through an AES-256-GCM fallback request with `X-TASFA-Stream-Mode: aes-256-gcm`. The fallback still carries the same HTP hash tag and balanced scalar headers.

### Remainder (last partial chunk)

If the file size is not a multiple of the chunk size, the last chunk is a **remainder**. It is sent as a single blob with its exact byte range; the server writes it at the correct offset. No padding or splitting is performed.

### Session init response

The `init` endpoint returns, among other fields:

- `chunk_size` — negotiated chunk size
- `max_parallel_chunks` — how many chunks the client may upload concurrently

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

All other vertices keep their raw scalar. The balanced values are sent as `X-TASFA-Magic-Scalar`.

### Why only v3 and v5?

The hexagonal lattice has two degrees of freedom. Fixing `v0,v1,v2,v4` and adjusting `v3,v5` uniquely satisfies the three line equations while keeping the delta minimal and local to the group.

## Server-Authoritative HTP Recovery

**The client is a dumb retransmission agent.** It does not compute repair algebra, does not evaluate cost thresholds, and does not derive suspect rankings. All of that lives on the server.

### Server validation flow

During `POST /file/upload/complete` the server:

1. Loads all per-chunk `magic_scalars` from `htp.bin`.
2. Validates only **complete 6-slot groups** (partial groups are skipped).
3. For every failing group, computes **suspicion scores** per slot by analyzing which line equations each slot participates in.

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

A slot receives a higher suspicion score when:
- It appears in **all failing equations** (score `0.95`).
- It appears in **multiple failing equations** but no passing equations (score `0.95`).
- It appears in **multiple failing equations** with some passing equations (score `0.85`, reduced if a passing equation exists and a consensus value is present).
- It appears in a **single failing equation** (base score `in_fail / total_fail`).

If a slot only appears in passing equations, it is **cleared** from the suspect list.

Scores are aggregated across all failed groups; if a chunk appears in multiple groups, its maximum score is kept.

### Repair cost threshold

Before requesting any repair, the server evaluates whether contraction is cheaper than direct retry:

```
repair_worthwhile(suspect_count, total_chunks):
    if suspect_count <= 2          → false  (too few for topology)
    if suspect_count > total / 3   → false  (cheaper to retry all)
    otherwise                      → true
```

If the threshold rejects repair, the server returns `needs_retry` with **all** suspect chunks as retry targets. The client retransmits them through the normal upload endpoint.

### Server-side recursive contraction

If repair is worthwhile, the server **internally** regroups the suspect chunks into fresh 6-slot HTP groups (a different topology from the original) and re-evaluates the line invariants on the existing scalar data:

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

- upload chunk size: `8 MiB` desktop, `4 MiB` mobile
- default browser upload parallelism: `4`
- max browser upload parallelism: `max_upload_parallel_chunks` in `blog.settings`
- max concurrent upload sessions: `max_total_parallel_uploads` in `blog.settings`
- max upload size: `max_upload_size` in `blog.settings`
- max browser download sessions: server-defined
- upload xhr timeout: `30 s`
- upload session fetch timeout: `12 s`

The browser's per-origin HTTP connection limit is respected naturally by the worker pool.

## Storage Model

Each upload session preallocates one temporary file:

- temp file: `data/tasfa/uploads/<upload_id>/upload.bin.part`
- metadata: `data/tasfa/uploads/<upload_id>/meta.json`
- fast binary meta: `data/tasfa/uploads/<upload_id>/meta.bin`
- state: `data/tasfa/uploads/<upload_id>/state.json`, `data/tasfa/uploads/<upload_id>/state.bin`
- HTP level-0: `data/tasfa/uploads/<upload_id>/htp.bin`

The server no longer maintains `blocks.bin` or `chunk_counts.bin`. Chunk completion is a single bitmap write.

Session metadata also stores per-vertex arrays:

- `hash_tags` — array of SHA-512 hex strings, one per chunk
- `magic_scalars` — array of balanced scalars, one per chunk
- `htp_retry_targets` — current server-issued retry target list
- `htp_suspicion_scores` — current suspicion ranking
- `htp_contraction_level` — number of server-side contraction passes applied

These are updated on every chunk upload and validated at completion.

## Delete PIN

Completed uploads receive a one-time delete PIN. The clear PIN is returned once; only its hash is stored.

## Download Protocol

1. Client requests a handshake.
2. Server returns `session_id`, `session_token`, `chunk_size`, `chunk_count`, and concurrency hints.
3. Client fetches chunk groups with `span=...` when supported.
4. Browser assembles the response into one contiguous buffer.

## DoS Mitigation via Bitmap

Both upload and download state are tracked with a **dense binary bitmap** (one byte per chunk, `'0'` / `'1'`).

### Upload side

- The server rejects any chunk whose index is already marked `'1'` in `state.bin`, **unless the chunk is explicitly in the server's retry target list**. An attacker cannot replay arbitrary chunks to burn disk I/O.
- `state.bin` is updated with `pwrite(..., 1, chunk_index)` — an O(1) atomic write. There is no JSON parsing on the hot path.
- The `complete` handler re-opens the bitmap, counts set bits, and refuses finalisation until `received_chunks == chunk_count`. This prevents truncated-file attacks.

### Session hardening

- Upload IDs and tokens are 16-byte / 24-byte random hex strings.
- Session locks (`flock`) prevent racing bitmap updates from concurrent requests.

## Self-Review Checklist

| Question | Answer |
|----------|--------|
| Q1: Does the client compute any repair algebra? | **No.** The client is a dumb retransmission agent. All suspect derivation, confidence scoring, cost thresholds, and contraction logic are server-side only. |
| Q2: Is repair cost threshold explicitly evaluated before contraction? | **Yes.** `htp_repair_worthwhile` rejects repair when `suspect_count <= 2` or `suspect_count > total_chunks / 3`, falling back to direct retry. |
| Q3: Are partial groups ever zero-padded? | **No.** Only complete 6-slot groups (`chunk_count / 6`) are validated. Incomplete final groups are excluded entirely. |
| Q4: Does the response contain suspicion scores, not just binary flags? | **Yes.** Every `needs_retry` response includes `suspicion_scores` as `{chunk_index, score}` objects. |
| Q5: Does contraction use a different grouping from the original? | **Yes.** `htp_contract_suspects` regroups suspect chunks into fresh 6-slot groups independent of original positions. |
| Q6: Are retry targets cleared on successful retransmission? | **Yes.** `handler_file_upload` removes the chunk from `htp_retry_targets` after accepting a retry retransmission. |
