# TASFA Fast-Link Strategy

This document describes the TASFA fast-link policy for high bandwidth clients, especially 500M- and 1000M-class networks.  
The goal is to keep throughput high without letting the browser saturate itself with too many concurrent media chunk jobs.

FAST does not change the baseline TASFA session, chunk authentication, encryption, or HTP integrity contract. It only changes how the client and server choose chunk size, parallelism, coalescing, and fallback behavior.

## Link Classes

TASFA keeps the existing strategy for normal links. A 100M-class or unknown-quality connection continues to use the existing adaptive chunk size and parallel scheduling.

A connection is treated as fast-link only when the browser reports:

- `navigator.connection.downlink >= 500`
- `saveData` is not enabled
- `effectiveType` is not `slow-2g`, `2g`, or `3g`
- reported RTT is absent or `<= 80ms`

The server applies the same guard to the handshake parameters:

- `link_downlink_mbps >= 500`
- `link_save_data != 1`
- `link_effective_type` is not `slow-2g`, `2g`, or `3g`
- `link_rtt_ms` is absent or `<= 80`

This prevents an unstable wireless client that merely advertises a high physical link from being forced into the fast-link strategy.

## Fast-Link Tiers

TASFA uses two fast-link tiers:

| Reported Downlink | Requested Chunk | Server Maximum | Strategy |
| --- | ---: | ---: | --- |
| `< 500 Mbps` or unstable hints | normal preference | 32 MiB | normal TASFA adaptive strategy |
| `500-899 Mbps` with stable hints | 48 MiB | 48 MiB | large chunks, low parallelism |
| `>= 900 Mbps` with stable hints | 64 MiB | 64 MiB | larger chunks, low parallelism |

## 100M-Class Strategy

100M-class behavior is unchanged:

- Download chunk hints are read from the normal stored TASFA preference.
- Download chunk size is clamped by the normal 32 MiB server maximum.
- Large media may keep the existing higher media parallel floors.
- Adaptive success/failure rules may grow chunk span and target parallelism over time.
- Service Worker range fallback behavior remains compatible with existing cache-based media responses.

## Fast-Link Strategy

For a qualified fast-link connection, TASFA uses larger chunks with fewer parallel jobs:

- The client requests 48 MiB at 500M-class and 64 MiB at 1000M-class during handshake.
- The server permits those larger maximums only for qualified fast-link handshakes.
- Large video/audio sessions cap media download parallelism:
  - desktop: up to 4 session-level workers, progressive media fetch limit of 3
  - mobile: up to 3 session-level workers, progressive media fetch limit of 2
- Chunk coalescing/span is forced to `1` for large media so each request maps to one large chunk.
- The progressive forward window is kept shallow so the browser only prepares near-future media data.

The intended shape is:

```text
100M-class:  smaller chunks + broader adaptive parallelism
500M-class:  48 MiB chunks + small fixed parallelism
1000M-class: 64 MiB chunks + small fixed parallelism
```

This avoids Chrome-side overload from excessive XHR completions, decrypt jobs, queue churn, and media stream enqueue pressure.

## Runtime Demotion

The fast-link decision is only an initial policy. The client still measures actual chunk transfer results.

For large media, if a fast-link session observes poor early effective throughput after at least two successful chunks, the client demotes that session back to the normal strategy:

- 500M-class sessions demote below 150 Mbps EWMA.
- 1000M-class sessions demote below 250 Mbps EWMA.

- `ultraFastConnection` is cleared for the session.
- Standard `maxParallel`, `targetParallel`, `coalesceChunks`, `currentSpan`, and `maxSpan` values are restored.
- Normal TASFA adaptive success/failure tuning resumes.

This handles unstable Wi-Fi, overloaded clients, bad RF conditions, or network paths where the reported link speed does not reflect actual application throughput.

## Service Worker Behavior

The Service Worker receives the fast-link flag with `TASFA_SESSION`.

For normal sessions, existing range fallback behavior remains available. For fast-link sessions, when the browser issues a `Range` request and the server does not return `206 Partial Content`, the Service Worker does not convert the entire response into a `Blob` just to slice it. That fallback is dangerous for very large media because it can put the entire response into browser memory.

The preferred fast-link media path is native server `206` range streaming or bounded TASFA progressive streaming.

## Server Guardrail

The server never trusts chunk size alone. A client can request `48 MiB` or `64 MiB`, but the download handshake only honors those upper bounds when the fast-link quality hints pass the server-side gate. Otherwise the normal 32 MiB maximum applies.

This keeps older clients, unknown clients, unstable wireless clients, and manually crafted requests on the conservative path.

## Scope

FAST currently applies to TASFA download and media playback. Upload tuning remains separate because upload chunk offsets and chunk counts are session-defining and have different renegotiation risks.
