# TASFA File Transfer Protocol Analysis

## What Is TASFA

TASFA is the **HTTP/XHR-based file upload/download protocol** used by fly.board. It is not merely a file pipe; it is a protocol layer that integrates per-chunk encryption, integrity verification, resilience over unstable networks, and DoS mitigation.

## Core Components

### 1. Per-Chunk AES-256-GCM Streaming Encryption

- Each upload session generates a unique `stream_key` and `iv_seed`.
- Every chunk is encrypted with `AES-256-GCM`, using `upload_id:chunk_index` as AAD (Additional Authenticated Data).
- If tag verification fails, the chunk is immediately discarded.
- The server decrypts the ciphertext and writes the plaintext directly to a preallocated temp file at offset `chunk_index * chunk_size`.
- This encryption ensures **session isolation**: a compromised key for one session does not affect others. It is not a replacement for TLS but an additional session-level integrity layer.

### 2. 6-Slot HTP (Hexagonal Tortoise Problem) Local-Repair Lattice

- HTP is **not a transport protocol**; it is a **local-repair hint**.
- Chunks are grouped into consecutive 6-element vertices.
- For each chunk, the first 8 bytes of its SHA-512 are interpreted as a 64-bit integer `H`, and `raw_scalar = H % modulus_M` is computed.
- Three line invariants (`L1 == L2 == L3`) must hold for each complete group. If they do not, only `v3` and `v5` are adjusted (balanced) to satisfy the invariants with minimal delta.
- On upload completion, the server verifies the invariants for every group. A mismatch returns `400 Bad Request`.
- This provides a lightweight early-detection hint for **chunk-level substitution or corruption** that a whole-file hash alone cannot reveal until the very end.

### 3. File-Level Sequential Queue (Browser Connection Stability)

- **Only one file is uploaded at a time**.
- When multiple files are selected, they are enqueued in `FileUploadQueue`. The next file starts only after the active one succeeds or fails.
- This respects the browser's per-origin limit of 6 concurrent connections and prevents connection-pool exhaustion deadlocks.

### 4. Binary Bitmap for DoS Mitigation and State Tracking

- Upload progress is tracked in `state.bin` as a **dense binary bitmap** (one byte per chunk, `'0'` / `'1'`).
- The server rejects any chunk whose index is already marked `'1'`. This blocks DoS attacks that replay the same chunk to burn disk I/O.
- Updates use `pwrite(..., 1, chunk_index)` — an O(1) atomic write with **no JSON parsing on the hot path**.
- On the download side, a bitmap tracks fetched chunks and stores them in an `IndexedDB` cache keyed by `baseUrl + ':' + chunk_index`. This prevents re-requesting the same data during a retry storm.

### 5. Adaptive Pacing

- Instead of simply reducing parallelism on bad networks, TASFA **paces** chunk dispatches by injecting `setTimeout` delays based on link quality.

| Link score | Upload pacing | Download pacing | Coalesce |
|-----------|---------------|-----------------|----------|
| >= 85     | 0 ms          | 0 ms            | 64       |
| 65 – 84   | 15 ms         | 0 ms            | 48       |
| 45 – 64   | 15 ms         | 10 ms           | 32       |
| < 45      | 35 ms         | 30 ms           | 16       |

- This keeps the TCP window from collapsing on high-loss links, prevents buffer-bloat stalls, and preserves enough in-flight chunks that a single slow packet does not freeze the whole transfer.
- When the link score drops below 45, stall timeouts are lengthened (up to 12 s) and retry back-off is flattened (factor 1.3 instead of 1.6).

### 6. Session Hardening

- Upload IDs and tokens are 16-byte / 24-byte random hex strings.
- `flock` prevents racing bitmap updates from concurrent requests.
- Download sessions are TTL-bound (`TASFA_DOWNLOAD_TTL = 86400`) and discarded when expired.

## Is the RPS Sacrifice Worth It?

> **Note**: The RPS trade-off below applies **only to file transfer endpoints** (`/file/upload`, `/file/download`, etc.). The page-view RPS measured in benchmarks (home, board list, etc.) is unaffected by TASFA.

TASFA clearly incurs an **RPS (Requests Per Second) penalty** compared to simple static-file serving. However, this penalty is an intentional trade-off to obtain the following values:

| What you sacrifice | What you gain |
|-------------------|---------------|
| Extra encryption/decryption overhead on the hot path | **Per-chunk integrity**: mismatched tags are detected immediately; corrupted chunks never hit disk |
| CPU spent on HTP scalar computation and group validation | **Early corruption detection**: errors that a whole-file hash would reveal only at the end are caught at the group level |
| Bitmap file I/O and `flock` session locks | **DoS mitigation**: blocks replay-chunk abuse and truncated-file attacks |
| Inability to upload multiple files simultaneously due to sequential queue | **Browser connection stability**: deadlock-free stable transfers within the per-origin 6-connection limit |
| Lower chunk dispatch rate due to pacing | **Unstable-link resilience**: sustained transmission without TCP window collapse on high-loss / high-latency links |
| Memory and disk usage for IndexedDB caching and session state | **Retry efficiency**: after network disruption, already-received chunks are not re-requested |

### Conclusion

TASFA does not pursue "the fastest possible file transfer." Instead, it is designed so that **files reach the destination completely and intact even under unstable networks and malicious clients**.

- **Upload perspective**: session-scoped encrypted chunk stream + HTP hints + bitmap DoS defense + sequential queue = stable and intact uploads.
- **Download perspective**: handshake-based session + bitmap tracking + IndexedDB caching + adaptive pacing = uninterrupted downloads.

While TASFA may be overkill for a plain blog, it has clear technical merit for a **file-heavy community board** where the RPS penalty is a justified trade-off.
