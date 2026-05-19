# TASFA

TASFA is the file-transfer protocol used in this tree for uploads and downloads.

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

The browser negotiates an upload session first, then sends **chunks** (up to `16 MiB`) with TASFA headers:

- `X-TASFA-Upload-ID`
- `X-TASFA-Upload-Token`
- `X-TASFA-Chunk-Index`
- `X-TASFA-Stream-Mode: aes-256-gcm`

Each chunk is encrypted with a per-session `AES-256-GCM` stream key. The server decrypts and writes the chunk directly to the preallocated temp file at `chunk_index * chunk_size`. There are no transport blocks anymore.

### Remainder (last partial chunk)

If the file size is not a multiple of the chunk size, the last chunk is a **remainder**. It is sent as a single blob with its exact byte range; the server writes it at the correct offset. No padding or splitting is performed.

### Final checks

- **init** issues the stream envelope and a PQC-signed session signature.
- **complete** verifies that the chunk bitmap is fully closed, runs a final topology check, and computes a SHA-256 checksum of the final file.

## Download Protocol

1. Client requests a handshake.
2. Server returns `session_id`, `session_token`, `chunk_size`, `chunk_count`, and concurrency hints.
3. Client fetches chunk groups with `span=...`.
4. Browser assembles the response into one contiguous buffer.

## Runtime Settings

- upload chunk size: `16 MiB`
- default browser upload parallelism: `16`
- max browser upload parallelism: `64`
- max browser download sessions: `6`

## Storage Model

Each upload session preallocates one temporary file:

- temp file: `data/tasfa/uploads/<upload_id>/upload.bin.part`
- metadata: `data/tasfa/uploads/<upload_id>/meta.json`
- fast binary meta: `data/tasfa/uploads/<upload_id>/meta.bin`
- state: `data/tasfa/uploads/<upload_id>/state.json`, `data/tasfa/uploads/<upload_id>/state.bin`

The server no longer maintains `blocks.bin` or `chunk_counts.bin`. Chunk completion is a single bitmap write.

## Delete PIN

Completed uploads receive a one-time delete PIN. The clear PIN is returned once; only its hash is stored.

---

## Chunk Topology

TASFA assigns every chunk index to a vertex on a 2-D square grid. The grid column count is the smallest integer `cols` such that `cols * cols >= chunk_count`.

For a chunk index `i`:
- `q = i % cols` (column)
- `r = i / cols` (row)

Each vertex has up to **six** axial neighbours (the six directions of a pointy-topped hex lattice projected onto the square grid):

```
{ 1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, -1}, {-1, 1}
```

### Magic sum

The *magic sum* of a vertex is the sum of its own 1-based index and the 1-based indices of all existing neighbours:

```
magic(i) = (i + 1) + sum( (n + 1) )  for every valid neighbour n of i
```

During `init` and `complete` the server verifies that the client-supplied `vertex_id` (`q:r`) and `magic_sum` match the expected grid geometry. This binds the chunk stream to a concrete spatial layout and prevents out-of-order substitution attacks.

## Parallel Sessions

TASFA is explicitly designed around **parallel session transport**.

### Upload — single session, parallel chunks

A single upload session keeps one encrypted stream context, but the client dispatches **multiple chunks concurrently** within that session. The server returns an `initial_parallel_chunks` window and a `max_parallel_chunks` ceiling. The client may open that many XMLHttpRequest / fetch streams at once, each carrying a distinct `X-TASFA-Chunk-Index`. The server validates every chunk independently against the session token and writes directly to the preallocated file via `pwrite`.

### Download — multi-session, parallel lanes

Downloads open **multiple TASFA sessions** for the same file when the link quality and device resources allow it. Each session is a *lane* with its own `session_id` / `session_token`. Lanes compete for chunk groups from a shared bitmap. This gives the browser more TCP connections, better throughput on high-BDP links, and graceful degradation when individual sessions stall.

Session multiplicity is capped by:
- `DOWNLOAD_MULTI_SESSION_CAP = 6`
- Device downlink, CPU cores, and memory heuristics
- The coalesced group budget (`chunk_count / coalesce_chunks`)

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
- Tokens are rotated on every `renegotiate` call.
- Session locks (`flock`) prevent racing bitmap updates from concurrent requests.

## Adaptive Pacing for Poor Links

TASFA does not simply drop parallelism on bad networks; it **paces** the dispatch instead.

| Link score | Upload pacing | Upload initial / max | Download pacing | Download initial / max | Coalesce |
|-----------|---------------|----------------------|-----------------|------------------------|----------|
| >= 85     | 0 ms          | 16 / 64              | 0 ms            | 112 / 256              | 64       |
| 65 – 84   | 15 ms         | 10 / 32              | 0 ms            | 96 / 224               | 48       |
| 45 – 64   | 15 ms         | 10 / 32              | 10 ms           | 64 / 160               | 32       |
| < 45      | 35 ms         | 6 / 24               | 30 ms           | 48 / 96                | 16       |

Pacing adds a small `setTimeout` between chunk dispatches. This keeps the TCP window from collapsing on high-loss links, prevents buffer-bloat stalls, and preserves enough in-flight chunks that a single slow packet does not freeze the whole transfer. On the client side, stall timeouts are lengthened (up to 12 s) and retry back-off is flattened (factor 1.3 instead of 1.6) when the link score drops below 45.
