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

- upload chunk size: `1 MiB`
- default browser upload parallelism: `8`
- max browser upload parallelism: `40`
- worker pool cap for upload preprocessing: `10`
- client stripe bucket count for chunk interleaving: `32`

Server-side negotiated upload window:

- strong links: initial `14`, max `40`
- medium links: initial `10`, max `40`
- weaker links: initial `7`, max `32`
- unstable links: initial `5`, max `20`

Server-side negotiated download profile:

- strong links: initial `52`, max `128`, coalesce `32`
- medium links: initial `42`, max `96`, coalesce `24`
- weaker links: initial `32`, max `72`, coalesce `20`
- unstable links: initial `20`, max `40`, coalesce `12`, pacing `1 ms`

## Resume and Recovery

The client treats the server bitmap as authoritative.

- `status` returns the current bitmap and negotiated window.
- `renegotiate` recalculates the current window from fresh link hints.
- `status` also returns `topology_closed_bitmap`, `topology_closure_complete`, and `client_stripes`.
- recoverable topology/signature failures return bitmap state plus a damage bitmap and rule label.
- the client rebuilds its pending queue as `damage -> frontier -> remaining`.
- stale callbacks from a superseded session generation are ignored on the browser side.
- reconnect/recovery now re-enters the session loop after `100 ms` instead of waiting whole seconds.
- concurrency is not reduced on every renegotiation; it is only suggested downward after accumulated retry/timeout pressure.
- upload preprocessing now runs in a prepare-ahead cache so compression/HMAC work overlaps active network transfers.

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
