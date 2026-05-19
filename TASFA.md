# TASFA

TASFA is the file-transfer protocol used in this tree for uploads and downloads.

It is built on top of plain HTTP/XHR and adds per-chunk encryption, a file-level sequential queue, and a **6-slot HTP (Hexagonal Tortoise Problem) local-repair lattice** for integrity hints.

## Routes

Upload:
- `POST /file/upload/init`
- `POST /file/upload/status`
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

The browser negotiates an upload session first, then sends **chunks** (up to `16 MiB`) with TASFA headers:

- `X-TASFA-Upload-ID`
- `X-TASFA-Upload-Token`
- `X-TASFA-Chunk-Index`
- `X-TASFA-Stream-Mode: aes-256-gcm`
- `X-TASFA-Hash-Tag` — SHA-512 of the plaintext chunk (128 hex chars)
- `X-TASFA-Magic-Scalar` — balanced HTP scalar for this vertex

Each chunk is encrypted with a per-session `AES-256-GCM` stream key. The server decrypts and writes the chunk directly to the preallocated temp file at `chunk_index * chunk_size`. There are no transport blocks anymore.

### Remainder (last partial chunk)

If the file size is not a multiple of the chunk size, the last chunk is a **remainder**. It is sent as a single blob with its exact byte range; the server writes it at the correct offset. No padding or splitting is performed.

### Session init response

The `init` endpoint returns, among other fields:

- `modulus_M` — a random 64-bit unsigned integer (never zero)
- `group_count` — `(chunk_count + 5) / 6`

The client uses `modulus_M` to compute raw and balanced HTP scalars.

## HTP Local Repair Lattice

HTP is **not a transport protocol**. It is a 6-slot local repair hint that helps detect corruption or substitution at the chunk level without restarting the whole file.

### Chunk grouping

Chunks are grouped into consecutive 6-element vertices:

```
Group g: [ v0 , v1 , v2 , v3 , v4 , v5 ]
         chunk g*6+0  ...  g*6+5
```

The last group may be partial; missing slots are treated as zero during validation.

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

### Server validation

During `complete` the server reads the per-chunk `magic_scalars` array from session state and verifies the invariant for every group. A mismatch returns `400 Bad Request` with `"htp line sum mismatch"`. The hash tag is the authoritative integrity check; HTP is a lightweight hint.

### Why only v3 and v5?

The hexagonal lattice has two degrees of freedom. Fixing `v0,v1,v2,v4` and adjusting `v3,v5` uniquely satisfies the three line equations while keeping the delta minimal and local to the group.

## Client-side Streaming Hash Computation

For very large files, pre-computing the SHA-512 of every chunk before upload would introduce a full-file read delay. TASFA uses a **lazy per-group pipeline** instead:

1. When a chunk is about to be dispatched, the client reads its `Blob` into an `ArrayBuffer` and computes SHA-512.
2. The raw scalar is stored in `asset.chunkScalars[chunkIndex]`.
3. After each hash finishes, the client checks whether all chunks of the same group already have raw scalars.
4. As soon as a group is complete, its `magicScalars` are balanced and any dispatches waiting for that group proceed.

This means:
- No full-file pre-read is required.
- Hashing and uploading run in a natural pipeline: while group *g* is uploading, group *g+1* can be hashing.
- Memory pressure is bounded because only one chunk buffer is hashed at a time.
- Parallelism is still capped at 6 concurrent HTTP connections, so a group waiting for its last scalar does not stall unrelated groups.

## File-Level Sequential Queue

Only **one file is uploaded at a time**. When multiple files are selected:

1. Each file gets its own asset, preview card, and HTP session.
2. Files are enqueued in `FileUploadQueue`.
3. When the active file finishes (success or failure), the queue automatically advances to the next file.
4. The batch "Upload queued files" button is always enabled; clicking it enqueues all pending files and starts the pump.

This prevents browser connection pool exhaustion and keeps stall detection reliable.

## Runtime Settings

- upload chunk size: `16 MiB`
- default browser upload parallelism: `6`
- max browser upload parallelism: `12`
- max browser download sessions: `6`
- upload xhr timeout: `30 s`
- upload session fetch timeout: `12 s`

The hard limit of 6 concurrent HTTP connections per origin in browsers is respected. TASFA never exceeds this for uploads.

## Storage Model

Each upload session preallocates one temporary file:

- temp file: `data/tasfa/uploads/<upload_id>/upload.bin.part`
- metadata: `data/tasfa/uploads/<upload_id>/meta.json`
- fast binary meta: `data/tasfa/uploads/<upload_id>/meta.bin`
- state: `data/tasfa/uploads/<upload_id>/state.json`, `data/tasfa/uploads/<upload_id>/state.bin`

The server no longer maintains `blocks.bin` or `chunk_counts.bin`. Chunk completion is a single bitmap write.

Session metadata also stores per-vertex arrays:

- `hash_tags` — array of SHA-512 hex strings, one per chunk
- `magic_scalars` — array of balanced scalars, one per chunk

These are updated on every chunk upload and validated at completion.

## Delete PIN

Completed uploads receive a one-time delete PIN. The clear PIN is returned once; only its hash is stored.

## Download Protocol

1. Client requests a handshake.
2. Server returns `session_id`, `session_token`, `chunk_size`, `chunk_count`, and concurrency hints.
3. Client fetches chunk groups with `span=...`.
4. Browser assembles the response into one contiguous buffer.

## DoS Mitigation via Bitmap

Both upload and download state are tracked with a **dense binary bitmap** (one byte per chunk, `'0'` / `'1'`).

### Upload side

- The server rejects any chunk whose index is already marked `'1'` in `state.bin`. An attacker cannot replay the same chunk to burn disk I/O.
- `state.bin` is updated with `pwrite(..., 1, chunk_index)` — an O(1) atomic write. There is no JSON parsing on the hot path.
- The `complete` handler re-opens the bitmap, counts set bits, and refuses finalisation until `received_chunks == chunk_count`. This prevents truncated-file attacks.

### Download side

- Each download lane maintains a local `bitmap` array. Already-fetched chunks are skipped, and successful chunk groups are written into an `IndexedDB` cache keyed by `baseUrl + ':' + chunk_index`. This prevents the client from re-requesting the same data during a retry storm.
- The handshake endpoint rate-limits session creation implicitly through the bitmap state: old sessions are TTL-bound (`TASFA_DOWNLOAD_TTL = 86400`) and discarded.

### Session hardening

- Upload IDs and tokens are 16-byte / 24-byte random hex strings.
- Session locks (`flock`) prevent racing bitmap updates from concurrent requests.

## Adaptive Pacing for Poor Links

TASFA does not simply drop parallelism on bad networks; it **paces** the dispatch instead.

| Link score | Upload pacing | Upload initial / max | Download pacing | Download initial / max | Coalesce |
|-----------|---------------|----------------------|-----------------|------------------------|----------|
| >= 85     | 0 ms          | 6 / 12               | 0 ms            | 112 / 256              | 64       |
| 65 – 84   | 15 ms         | 6 / 12               | 0 ms            | 96 / 224               | 48       |
| 45 – 64   | 15 ms         | 6 / 12               | 10 ms           | 64 / 160               | 32       |
| < 45      | 35 ms         | 6 / 12               | 30 ms           | 48 / 96                | 16       |

Pacing adds a small `setTimeout` between chunk dispatches. This keeps the TCP window from collapsing on high-loss links, prevents buffer-bloat stalls, and preserves enough in-flight chunks that a single slow packet does not freeze the whole transfer. On the client side, stall timeouts are lengthened (up to 12 s) and retry back-off is flattened (factor 1.3 instead of 1.6) when the link score drops below 45.
