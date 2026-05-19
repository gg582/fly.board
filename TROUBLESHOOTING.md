# TASFA Upload Troubleshooting

This document records the findings from the investigation into file upload hangs and performance issues.

## Identified Issues

### 1. Chunk Size Mismatch
- **Symptoms**: "chunk store failed" error in server logs.
- **Cause**: The initialization payload (`/file/upload/init`) specified a 4MB chunk size, but the actual upload requests sent smaller (e.g., 1MB) chunks.
- **Root Cause**: The server strictly validates that the received body size matches the `expected_size` (calculated from `chunk_size` and `chunk_index`). Any mismatch leads to a 500 error.
- **Resolution**: Ensure the client-side chunking logic matches the `chunk_count` and `total_size` sent during initialization.

### 2. High Concurrency Resource Contention
- **Symptoms**: Uploads appear to hang or slow down significantly when using high concurrency (e.g., 32+ simultaneous requests).
- **Analysis**:
    - **Disk I/O**: Simultaneous writes to the same sparse file can lead to filesystem lock contention.
    - **Memory Limits**: CWIST has internal limits on memory usage per connection and overall app memory (`cwist_app_set_max_memspace`). Default settings may be too low for many large concurrent chunks.
    - **TLS Overhead**: TLS 1.3 / HTTP/3 handshakes and record processing add latency. High-frequency small chunks exacerbate this.

### 3. Bitmap Synchronization Overhead
- **Symptoms**: Increasing latency as the upload progresses.
- **Cause**: Loading the full binary bitmap from disk on *every* chunk ACK was causing unnecessary I/O.
- **Resolution**: Optimized to only return the full bitmap every 32 chunks or when specifically needed for synchronization.

### 4. Logging and Monitoring
- **Observation**: Standard logging was too coarse to see individual chunk progress under load.
- **Improvement**: Added millisecond precision to logs to verify that the server was still processing requests even when it appeared "hung" to the client.

### 5. Client-Side Transport Slot Serialization
- **Symptoms**: Uploads looked parallel on paper, but the wire would effectively drain one chunk at a time after an initial burst.
- **Cause**: The browser counted chunks that were still being preprocessed or compressed as active transport slots before `xhr.send()` had actually happened.
- **Resolution**: Split scheduler accounting into reservation slots and real network in-flight slots. Only chunks that have actually entered transport now consume `activeRequests`.

## Recommendations

1.  **Tune Concurrency**: Balance concurrency based on network conditions. For local testing, 4-8 concurrent chunks proved more stable than 32.
2.  **Verify Chunking**: Always use the `chunk_size` returned by the server or defined in `src/handlers/tasfa.c` (`TASFA_CHUNK_SIZE`, currently `1 MiB`).
3.  **App Limits**: If high concurrency is required, increase `cwist_app_set_max_memspace` and `cwist_app_configure_bdr` in `src/main.c`.
4.  **Filesystem**: Use a filesystem that handles sparse files efficiently (like XFS or Ext4 with `posix_fallocate`).
5.  **Do Not Conflate Prep With Wire Time**: Compression/HMAC staging should not block network admission control. Keep separate counters for prepared, reserved, and actively transmitting chunks.

---
*Created on 2026-05-19 after investigation.*
