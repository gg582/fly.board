# TASFA

TASFA is the only file-transfer path in the current tree. Legacy direct file download and legacy multipart upload fallback are intentionally disabled.

## Routes

Upload session lifecycle:

- `POST /file/upload/init`
- `POST /file/upload/status`
- `POST /file/upload/renegotiate`
- `POST /file/upload`
- `POST /file/upload/complete`
- `POST /file/upload/cancel`

Download session lifecycle:

- `GET /file/download/:id/handshake`
- `GET /file/download/:id/chunk/:chunk_index`
- `GET /assets/tasfa/img/:filename/handshake`
- `GET /assets/tasfa/img/:filename/chunk/:chunk_index`
- `GET /assets/tasfa/uploads/:filename/handshake`
- `GET /assets/tasfa/uploads/:filename/chunk/:chunk_index`

Compatibility guardrails:

- `GET /file/download/:id` does not stream bytes directly.
- `GET /assets/uploads/:filename` does not stream bytes directly.
- `POST /file/upload` rejects requests that do not carry TASFA headers.

## Upload Protocol

The browser negotiates an upload session first, then sends raw binary chunks with TASFA headers:

- `X-TASFA-Upload-ID`
- `X-TASFA-Upload-Token`
- `X-TASFA-Chunk-Index`
- `X-TASFA-Block-Offset`
- `X-TASFA-Nonce`
- `X-TASFA-Vertex-Id`
- `X-TASFA-Magic-Sum`
- `X-TASFA-Chunk-Signature`

If a chunk compresses well, the worker may send it with `Content-Encoding: gzip`. Otherwise it stays `identity`.

Chunk verification currently checks:

- upload token match
- HMAC-SHA256 signature
- expected block offset
- deterministic topology vertex
- deterministic topology magic sum

Chunk acceptance is bitmap-based. The server stores session state under `public/uploads/.chunks/<upload_id>/`.

Current transport behavior:

- topology ownership still lives at the TASFA chunk level
- upload chunks are currently `1 MiB`
- client-side scheduling now distinguishes `dispatchReservations` from real network `activeRequests`
- only requests that have actually crossed into `xhr.send()` consume transport slots
- preprocessing, compression, and HMAC staging are intentionally prevented from serializing the wire

Planned breaking-change direction:

- keep the hexagonal topology semantics at the chunk index layer
- move the transport below that layer toward chunk-internal streaming / block framing
- allow a single topology chunk to be emitted as multiple ordered transport blocks instead of one monolithic body
- preserve resumability and topology validation without requiring whole-chunk wire stalls

## Download Protocol

Downloads are also session-based:

1. Client requests a handshake.
2. Server returns `session_id`, `session_token`, `chunk_size`, `chunk_count`, and download concurrency hints.
3. Client fetches chunk groups with `span=...`.
4. Browser assembles the response into one contiguous buffer.

No direct file bytes are served from the public file endpoints anymore.

## Runtime Settings

Current tuned values in this tree:

- upload chunk size: `1 MiB`
- default browser upload parallelism: `16`
- max browser upload parallelism: `64`
- max browser download sessions: `6` (multi-TASFA)
- worker pool cap for upload preprocessing: `12`
- browser-side prepared upload budget: `96 MiB`
- browser-side active upload budget: `128 MiB`
- upload rollover cadence: steady-state `700 ms`, startup `8000 ms`
- stall thresholds: foreground steady-state `1500 ms`, background `10000 ms`, startup `3000 ms`

Server-side negotiated upload window:

- strong links: initial `16`, max `64`
- medium links: initial `14`, max `64`
- weaker links: initial `10`, max `32`
- unstable links: initial `4`, max `16`

Server-side negotiated download profile:

- strong links: initial `112`, max `256`, coalesce `64`
- medium links: initial `96`, max `224`, coalesce `48`
- weaker links: initial `64`, max `160`, coalesce `32`
- unstable links: initial `32`, max `96`, coalesce `24`

## Resume and Session Rollover

The client treats the server bitmap as authoritative for uploads, while using local IndexedDB for download persistence.

- `status` returns the current bitmap and negotiated window.
- `renegotiate` recalculates the current window from fresh link hints, with an aggressive acceleration bias (+8 chunks or +50% per successful step).
- `status` also returns `topology_closed_bitmap`, `topology_closure_complete`, and `client_stripes`.
- the client rebuilds its pending queue as `damage -> frontier -> remaining`.
- when chunk validation or transport state goes bad, the browser rolls the upload into a fresh negotiated session using the existing authoritative bitmap.
- upload defaults are intentionally conservative now because sparse-file write contention and TLS churn were outperforming raw request fan-out in the bad direction.
- the current tuning keeps `1 MiB` chunks for responsiveness, but restores aggregate in-flight byte volume through higher parallel slot counts so burst throughput does not collapse.
- upload scheduling now separates `dispatchReservations` from real network `activeRequests`, so chunk preprocessing cannot falsely consume transport slots and serialize the wire.
- **Atomic State Persistence**: Server-side state writes (`meta.json`, `state.json`) use temporary files and `rename()` to prevent corruption during concurrent access or crashes.
- **IndexedDB Download Cache**: The browser stores downloaded chunks in `tasfa_cache` (IndexedDB). Handshakes check this cache to enable seamless resume even after tab closure or page refresh.
- **Visibility-Aware Scheduling**: The client scheduler monitors `visibilityState`. Background tabs use relaxed stall timeouts (10s+) to accommodate browser throttling, while foreground tabs stay aggressive.
- **Automatic Wake-up**: A `visibilitychange` listener ensures the transfer loop resumes immediately when a tab returns to the foreground.

## Multi-TASFA Download

When the browser has enough spare CPU, memory, and link budget, the client now opens more than one TASFA download session for the same file.

- the first handshake still defines the canonical file shape: `chunk_size`, `chunk_count`, `total_size`
- extra handshakes are admitted only if they report the same file shape
- each session gets its own `session_id` and `session_token`, but they all write into the same client-side destination buffer
- chunk groups are reserved from one shared cursor, so sessions never intentionally duplicate the same span
- session fan-out is capped at `6` and only activates on larger files plus stronger clients.
- retries stay short and local to the lane that failed; successful lanes keep draining the remaining queue instead of collapsing the whole download window first

## Page Integration

The browser now boots TASFA directly instead of painting legacy direct URLs first.

- download links render as `data-tasfa-download-link`
- streamed media render as `data-tasfa-download`
- the global TASFA client upgrades initial DOM and future dynamic DOM mutations
- the editor preview path re-runs TASFA affordances after each markdown refresh
- post/file attachment binding uses explicit `media_meta`, not broad orphan adoption by `user_id`

## Storage Model

Each upload session preallocates one temporary file:

- temp file path: `public/uploads/.chunks/<upload_id>/upload.bin.part`
- metadata path: `public/uploads/.chunks/<upload_id>/meta.json`

Chunks are written into fixed offsets with `pwrite()`. After all chunks are confirmed, the temp file is renamed into `public/uploads/` and inserted into the `files` table.

## Delete PIN

Completed TASFA file uploads now issue a one-time delete PIN:

- the clear PIN is returned once in the upload-complete response
- only the hash is stored in the database
- `/file/delete` accepts normal owner/admin deletion or matching `delete_pin`

## Current Limits

This implementation is production-oriented but still intentionally simple:

- upload session state is file-backed JSON, not a dedicated in-memory coordinator
- direct file endpoints are disabled instead of transparently proxying into TASFA
- anonymous post delete PIN support is not part of this file-transfer layer
