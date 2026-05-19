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

## Download Protocol

Downloads are also session-based:

1. Client requests a handshake.
2. Server returns `session_id`, `session_token`, `chunk_size`, `chunk_count`, and download concurrency hints.
3. Client fetches chunk groups with `span=...`.
4. Browser assembles the response into one contiguous buffer.

No direct file bytes are served from the public file endpoints anymore.

## Runtime Settings

Current tuned values in this tree:

- upload chunk size: `4 MiB`
- default browser upload parallelism: `16`
- max browser upload parallelism: `72`
- worker pool cap for upload preprocessing: `14`
- browser-side upload memory budget assumption: `512 MiB`
- client stripe bucket count for chunk interleaving: `32`

Server-side negotiated upload window:

- strong links: initial `18`, max `48`
- medium links: initial `12`, max `48`
- weaker links: initial `8`, max `36`
- unstable links: initial `5`, max `20`

Server-side negotiated download profile:

- strong links: initial `64`, max `160`, coalesce `40`
- medium links: initial `52`, max `120`, coalesce `32`
- weaker links: initial `40`, max `88`, coalesce `24`
- unstable links: initial `24`, max `48`, coalesce `16`, pacing `1 ms`

## Resume and Session Rollover

The client treats the server bitmap as authoritative.

- `status` returns the current bitmap and negotiated window.
- `renegotiate` recalculates the current window from fresh link hints, but the client now biases toward keeping or raising throughput instead of backing it down aggressively.
- `status` also returns `topology_closed_bitmap`, `topology_closure_complete`, and `client_stripes`.
- recoverable topology/signature failures return bitmap state plus a damage bitmap and rule label.
- the client rebuilds its pending queue as `damage -> frontier -> remaining`.
- stale callbacks from a superseded session generation are ignored on the browser side.
- when chunk validation or transport state goes bad, the browser rolls the upload into a fresh negotiated session using the existing authoritative bitmap instead of trying to limp along inside the old callback chain.
- rollover retries now stay on a short cadence instead of exponential recovery backoff.
- if the previous session already committed chunks, the next session continues from the remaining bitmap gap only.
- upload preprocessing now runs in a prepare-ahead cache so compression/HMAC work overlaps active network transfers.
- upload window refill now ticks at `10 ms`, can prepare up to `48` chunks ahead, and climbs by `+2` while still below half of the server ceiling.
- download chunk grouping now allows up to `64` chunk spans per request and grows the fetch window more aggressively before backing off.

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
