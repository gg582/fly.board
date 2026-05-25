# TASFA TODO

## Current Observations

- During 50 MB-class file uploads, throughput visibly drops after certain ranges, then recovers.
- The bottleneck does not look fully random, but fixed byte intervals are treated as observations only, not as TASFA policy inputs.
- Temporary bottlenecks coincide with changes in client-side buffering and inflight transfer state, so TASFA should measure buffers it directly owns instead of treating process RSS deltas as protocol signals.
- Raw upload throughput is acceptable outside the stalled ranges. The real issue is that the stall lasts long enough for users to interpret it as an upload failure.
- Based on current observations, TASFA chunk assembly, send/receive data consistency, and the HTP/Aitken algorithmic layer do not show an obvious correctness error.
- Running with the `max_upload_size` cap removed as Unlimited Upload does not directly conflict with the current symptoms. The file size cap is not treated as the primary cause of this bottleneck.

## Problem Definition

TASFA reacts too sensitively to transient network disturbance by lowering transfer speed, then takes too long to recover from the lowered state. This is especially harmful on reasonably good 5G mobile networks, where switching to a conservative mode too easily causes TASFA to occupy only part of the available bandwidth, increasing total upload time and worsening perceived UX.

The goal is to reliably achieve one of the following outcomes:

1. If quality degradation is genuinely necessary, keep the stream in a lower-quality state for about `30-60 s` while keeping progress indication and status messages alive.
2. Around renegotiation, aggressively recover parallelism and pacing so bandwidth recovery begins within about `5 s`.

## Hypotheses To Check

- [x] Record when negotiated `chunk_size`, `current_parallel_chunks`, `max_parallel_chunks`, and `dispatch_pacing_ms` change during a 50 MB upload.
- [x] Record enough data to separate whether bottlenecks align with browser progress events, TCP/HTTP connection pools, XHR timeout, HTP group boundaries, AES-GCM fallback prefetch, or server-side `pwrite`/fsync-class latency.
- [x] Align client/server logs around owned metrics such as inflight bytes, encrypted fallback cache bytes, HTP group cache activity, chunk timing, and metadata save latency instead of using process RSS deltas as a heuristic.
- [x] Check whether the previous `pending.pop()` LIFO order hurt progress display, then pin the upload queue to FIFO.
- [x] Measure how much parallelism drops immediately after `/file/upload/renegotiate`, and how many successful chunks are needed for recovery.
- [x] Reflect that the watchdog's `90 s` inactivity threshold is too late for UX by splitting recovery into `25 s` soft recovery and `90 s` hard recovery.

## Instrumentation TODO

- [x] Add a per-upload-session client ring buffer log: timestamp, chunk index, chunk size, request mode, duration, Mbps, retry count, predicted quality, target parallel, max parallel, pacing, and inflight count.
- [x] Include renegotiation requests/responses in the same log: score inputs, suggested parallelism, server current/max parallelism, and pacing.
- [x] Distinguish fallback entry reasons: retry threshold, Aitken quality, timeout, HTTP 429, and network error.
- [x] Do not expose internal states such as `stalled`, `recovering`, `fallback`, or `renegotiating` directly in the UI. Show only short progress states that indicate the upload is still alive.
- [x] Sample server-side latency by upload id: chunk accept latency, state/meta save latency, `pwrite` latency, and HTP scalar save latency.
- [x] Provide a trace summary so 50 MB, 100 MB, and 1 GB files can be compared with the same log format across Wi-Fi, wired, and 5G mobile networks.

## Recovery Policy TODO

- [x] Add hysteresis so one or two transient failures cannot cause a large parallelism drop.
- [x] On good 5G/4G links, do not sharply reduce `targetParallel` for short throughput drops unless a timeout occurs.
- [x] Reduce parallelism slowly and recover quickly. For example, experiment with `-1` on failure and `+2` or `+4` after clean successes.
- [x] If `current_parallel_chunks` drops below the previous value after renegotiation, give it a chance to rise again within at least `5 s`.
- [x] Since `dispatch_pacing_ms` can create long stalls, keep it at 0 on high-quality links and cap it tightly on low-quality links.
- [x] Make Tier 1 a send-as-much-as-possible strategy, and move to Tier 2 slowdown/guarded mode only after repeated and predictable error patterns are confirmed.
- [x] Set failure tolerance aggressively so parallelism is not reduced until the same chunk or same failure type repeats several times.
- [x] Run fallback prefetch only within a budget that does not harm parallel throughput. If encryption preparation blocks upload sending, apply a separate budget.
- [x] Add `15-30 s` soft recovery separately from the 90-second hard recovery. Soft recovery performs renegotiation, target parallel bump, and status refresh before aborting all XHRs.

## Aitken/Wynn Prediction TODO

- [x] The current quality sample was too coarse when based mostly on `mbps / 25` and timeout/failure constants. Include throughput slope, inflight changes, retry-free streak, and progress event silence.
- [x] When the Aitken result drops sharply, compute confidence instead of immediately slowing down. If the sample denominator is small or the sequence oscillates heavily, conservatively ignore the prediction.
- [x] Keep predictions limited to the transfer-quality sequence. Do not predict file contents or chunk data, and do not trust predicted content.
- [x] Collect error patterns and predict `next chunk timeout likelihood`, `fallback likelihood`, and `5-second recovery likelihood` separately.
- [x] Even when prediction is low, if a good network looks temporarily disturbed, prefer guarded transfer and fast recovery over reducing parallelism.

## SIMD Candidates

- [x] First stabilize Aitken/Wynn quality prediction and sample preprocessing with a scalar baseline.
- [x] Prioritize the blog deployment targets `x64`, `i386`, `arm32`, `arm64`, `mips64le`, and `ppcle64`.
- [x] Use a SIMD path for HTP hexagonal verification line-sum calculation on supported architectures, and fall back to simple scalar arithmetic in the same function when SIMD is unavailable.
- [x] Proceed with SIMD optimization after the math model is stable. Initial candidates are x64 SSE2/AVX2, arm64 NEON, and ppc64le VSX, while keeping scalar fallback for the rest.
- [x] SIMD must not increase memory usage or latency on the upload hot path. For small sample counts, function call and branch overhead can outweigh SIMD gains, so benchmark before expanding SIMD further.

## Success Criteria

- [x] For 50 MB uploads, remove progress stalls longer than `5 s`, or make unavoidable stalls still look alive in the UI.
- [x] On good 5G mobile networks, start bandwidth recovery within `5 s` after a short disturbance by using a fast recovery window.
- [x] Prevent `targetParallel` from staying low for too long after a temporary bottleneck.
- [x] Ensure 50 MB, 100 MB, and 1 GB uploads use the same recovery policy under Unlimited Upload settings.
- [x] Preserve TASFA consistency verification, HTP retry, and AES-GCM fallback security/integrity boundaries.

## Notes

- Actual performance validation by uploading 50 MB, 100 MB, and 1 GB files over Wi-Fi, wired, and 5G cannot be claimed complete from local code changes alone. Instead, `window.__tasfaUploadTraces`, `window.__tasfaUploadSummaries`, and server `chunk timing` / `htp verify` logs now provide a common comparison format.
- Process RSS deltas are intentionally not used as TASFA policy inputs because they mix allocator behavior, browser buffers, TLS/HTTP state, and application buffers. TASFA now tracks owned memory pressure through `encryptedCacheBytes`, `fallbackPrefetchBudget`, `inflightBytes`, and server-side timing logs.
- SIMD expansion for Aitken/Wynn itself is uncertain because the sample count is small. HTP hexagonal verification line-sum uses the SIMD path, while Aitken remains fixed to the scalar baseline.
