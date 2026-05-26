# TODO_TASFA

This document keeps only TASFA upload-improvement items that remain debatable or require measurement before a final decision. Confirmed observations and the execution plan are tracked in `TASFA_TODO.md`.

## Remaining Policy Choices

- [ ] `pending` queue order: the current implementation uses FIFO to reduce perceived progress stalls. Compare 50 MB, 100 MB, and 1 GB logs to determine whether LIFO is better for cache locality or retry responsiveness.
- [ ] Soft recovery strength: the current policy performs renegotiation and a parallelism bump after `25 s` of inactivity, then hard abort/resume after `90 s`. Compare `15 s`, `20 s`, and `30 s` candidates on mobile networks.
- [x] Parallelism floor on good 4G/5G links: the current floor is mobile 4 and desktop 8. Verify whether this is too aggressive on low-end devices or under thermal pressure.
- [ ] Fast recovery after renegotiation: after a drop, successful chunks currently recover by `+2` or `+4` for about `5 s`. Validate this against HTTP 429 frequency, since it may be too aggressive when the server is genuinely congested.
- [ ] Aitken confidence model: the current confidence uses variance over the latest four quality samples. Compare it with a model that also includes progress event silence, inflight delta, and retry-free streak.
- [ ] `dispatch_pacing_ms`: on good links, the client ignores pacing by forcing it to 0. Decide whether any deployment environment needs the server to enforce this value.
- [ ] Tier 1/Tier 2 threshold: the current policy slows down only after a failure pattern around five repeats. Verify whether Tier 2 starts too late on genuinely bad 5G/Wi-Fi links.
- [x] SIMD: HTP line-sum has a SIMD path plus simple scalar fallback. Because Aitken/Wynn sample counts are small, SIMD call overhead may outweigh gains, so SIMD expansion beyond HTP verification is deferred until benchmarked. (AVX2 implemented)

## Additional Verification Needed

- [ ] Align server `FLY_LOG_DEBUG("[TASFA] chunk timing ...")` logs with client `asset.tasfaTrace` by upload id to compare bottleneck timing.
- [x] Remove RSS interval heuristics from TASFA policy and trace owned pressure instead: encrypted fallback cache bytes, fallback prefetch budget, inflight bytes, HTP activity, and server timing.
- [ ] Check whether `retryEvents` and `timeoutEvents` accumulate too much during large-file Unlimited Upload sessions and make later renegotiation scores overly pessimistic.
