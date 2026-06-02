# TASFA Media Extensions

This document describes optional media-specific extensions layered on top of the core TASFA protocol.  
They cover native `Range`-based media playback, progressive chunk streaming for video/audio, and server-side media generation (thumbnails, previews).

These extensions do not change the baseline upload/download chunk contract or the HTP integrity lattice.

## Media Routes

- `GET /file/download/:id` — supports `Range` header with TASFA session authentication (`X-TASFA-Session-ID`, `X-TASFA-Session-Token`) for native media streaming.
- `GET /assets/tasfa/img/:filename/handshake`
- `GET /assets/tasfa/img/:filename/chunk/:chunk_index`
- `GET /assets/tasfa/uploads/:filename/handshake`
- `GET /assets/tasfa/uploads/:filename/chunk/:chunk_index`

## Range-Request Streaming

For large media files the client may bypass the full chunk-by-chunk download and request a byte range directly from `GET /file/download/:id` by sending:

- `Range: bytes=<start>-<end>`
- `X-TASFA-Session-ID: <session_id>`
- `X-TASFA-Session-Token: <session_token>`

The server validates the session token and returns `206 Partial Content` with `Content-Range`, `Content-Length`, and `Accept-Ranges` headers. The Service Worker caches the full response and can serve subsequent range requests from the cache without hitting the server.

For video/audio playback the client may perform a **handshake-only registration** (`fetchBlobViaTasfa(url, {handshakeOnly: true})`) and then set the media element's `src` to the direct download URL. The browser issues native `Range` requests for seek operations; the Service Worker intercepts them, adds the TASFA session headers, and serves `206` responses from cache when available.

## Progressive Chunk Streaming

For video and audio playback the TASFA protocol supports **progressive chunk streaming** without changing any URL format. The client:

1. Performs a normal handshake (`GET /.../handshake`) to obtain the session keys, chunk size, and chunk count.
2. Opens a `ReadableStream` in the Service Worker via `TASFA_STREAM_OPEN`. The stream is exposed at `/__tasfa_stream__/<streamId>`.
3. Uses a bounded scheduler to fetch chunks in parallel only inside a forward window from the next chunk that must be fed to the player (`span=1` per request), then stores them by `chunk_index`.
4. Once the initial threshold is reached (by default the first 2 chunks or at least 2 MiB, whichever is larger), the client starts the media player pointing at the SW stream URL.
5. Feeds only the next contiguous decrypted chunk into the SW stream via `TASFA_STREAM_CHUNK`, preserving byte order even when later chunks arrive first.
6. Requeues short reads, timeouts, HTTP retry responses, and network errors for the same chunk until it is received. A failed chunk records a `retryAt` time and releases its network slot, allowing the scheduler to fetch another eligible chunk in the same forward window instead of idling.
7. Remaining chunks continue to download in the background and are fed into the stream. When the final chunk is pushed, the client sends `TASFA_STREAM_CLOSE` so the browser sees an EOF.

This makes progressive playback behave like a video service buffer: the browser-side assembler is strictly sequential, while actual network traffic keeps adaptive, bounded parallelism in the near-future chunk window. The player may wait at the current playback edge while the missing chunk is retried, but TASFA does not advance past that missing byte range or close the stream early. Successful retries keep the player attached to the same `/__tasfa_stream__/<streamId>` URL.

The server advertises support via `supports_progressive_streaming: true` in the handshake response. Existing clients that do not implement progressive streaming can ignore the field and continue to use full download or range-request streaming.

Download chunk responses include:

- `X-TASFA-Chunk-Index` and `X-TASFA-Chunk-Count`
- `X-TASFA-Predicted-Remaining-Ms` when RTT samples are available
- `X-TASFA-Stream-Mode: aes-256-gcm` and encrypted payload when encryption is active
- `X-TASFA-Hash-Tag` and `X-TASFA-Magic-Scalar` when pre-calculated HTP metadata exists

## Media Processing Integration

Server-generated media (thumbnails, audio previews) are first-class TASFA assets:

- **HTP Metadata Pre-calculation**: When the server generates a thumbnail or preview, it immediately computes HTP scalars and SHA-256 tags for its chunks and stores them in `data/tasfa/media_htp`. Note: the server-side media generator uses **SHA-256** for this purpose, while the upload client still uses **SHA-512** for user-uploaded chunks.
- **Reliable Media Transfer**: Media is served via the `/assets/tasfa/...` routes, which support the full TASFA protocol including chunk-level integrity verification using the pre-calculated HTP metadata.
- **Concurrency Control**: Media generation (`ffmpeg`) is limited to 4 concurrent processes to protect server resources.
- **Unified Media Insertion**: Both auto-insert and file-browser insertion use native HTML `<video>` or `<audio>` tags with `controls` and `playsinline` attributes. The client detects media type by MIME type or file extension and falls back to extension-based detection when the MIME type is generic (`application/octet-stream`).
