# HTTP/1.1 · HTTP/2 · HTTP/3 Simultaneous Support Analysis

## Overview

fly.board enables both `cwist_app_use_https2()` and `cwist_app_use_https3()`, and injects `Alt-Svc: h3=":%d"; ma=86400, h2=":%d"; ma=86400` into every response. This creates an architecture that **prioritizes HTTP/3, falls back to HTTP/2 when unavailable, and further downgrades to HTTP/1.1 if necessary**.

## Why Support All Three "Even at the Cost of Performance"

### 1. HTTP/3 (QUIC) — Ideal but Environmentally Constrained

- **Pros**: 0-RTT handshake, complete elimination of TCP Head-of-Line Blocking, connection migration (session persists across network changes).
- **Performance cost**: UDP-based QUIC introduces extra encryption, packet reassembly, and congestion-control overhead, resulting in **higher CPU usage than HTTP/2**. Additionally, the BoringSSL + ngtcp2/nghttp3 stack inside CWIST occupies more memory and code paths than a plain HTTP/2 TCP stack.
- **Why keep it**: Modern browsers prefer HTTP/3, but **enterprise firewalls, certain NATs, legacy proxies, and mobile carrier networks block or QoS-throttle UDP**. Supporting only HTTP/3 would make the service unreachable for users behind such restrictions.

### 2. HTTP/2 (TLS 1.3) — The Stable "Greatest Common Denominator"

- **Pros**: Multiplexing over a single TCP connection solves HTTP/1.1 connection waste; optional server push; header compression (HPACK).
- **Performance cost**: HTTP/2 has a more complex state machine than HTTP/1.1, consuming CPU cycles on stream prioritization and flow-control window updates. Notably, **fly.board's benchmarks were measured on HTTP/2** — implying that HTTP/3 does not yet produce predictable, reproducible results across all benchmark tools and network conditions.
- **Why keep it**: It serves as the **primary fallback** when HTTP/3 is blocked. Performance tools such as `h2load` standardize on HTTP/2, and the debugging/monitoring ecosystem is far more mature than for HTTP/3.

### 3. HTTP/1.1 — The "Last Resort" and Compatibility Baseline

- **Pros**: Simplicity. Text-based protocol makes debugging trivial; every load balancer, reverse proxy, and health-check tool supports it perfectly.
- **Performance cost**: One request-response per connection (pipelining is effectively broken). As connection count rises, memory and file-descriptor consumption explodes. **Concurrency efficiency is drastically lower** than HTTP/2 multiplexing.
- **Why keep it**:
  - **Legacy clients**: Old CLI tools, some embedded devices, and aged crawlers only understand HTTP/1.1.
  - **Infrastructure compatibility**: Many load balancers and WAFs terminate TLS but cannot handle HTTP/2 natively and fall back to HTTP/1.1.
  - **Health checks and monitoring**: `curl`, `wget`, and Nagios/Zabbix-based probes remain most stable over HTTP/1.1.

## Alt-Svc Based Priority Negotiation

```c
// src/handlers/handlers.c
snprintf(altsvc, sizeof(altsvc), 
    "h3=\":%d\"; ma=86400, h2=\":%d\"; ma=86400", 
    g_config.port, g_config.port);
```

- When a client makes its first connection over HTTP/2 (or HTTP/1.1), the server advertises that HTTP/3 is available on the same port via UDP.
- The client caches this hint (`ma=86400`, 24 hours) and **attempts HTTP/3 on subsequent requests**.
- If HTTP/3 fails, client libraries (browsers, etc.) automatically downgrade to HTTP/2 → HTTP/1.1.

## What Is Gained by "Giving Up" Performance

| What you give up | What you gain |
|------------------|---------------|
| Theoretical lowest latency by running HTTP/3 exclusively | **Global accessibility**: the service remains reachable even in UDP-blocked environments |
| Stable throughput of an HTTP/2-only deployment | **Future-proofing**: as HTTP/3 becomes standard, clients automatically switch to the optimal path |
| Extreme simplicity and low memory of an HTTP/1.1-only deployment | **TASFA large-file transfer optimization**: HTTP/2 multiplexing + HTTP/3 streams are advantageous for chunked transfers |
| Reduced server code complexity (maintaining only one stack) | **Operational resilience**: if one protocol stack has a bug, traffic automatically routes around it |

## Conclusion

Supporting HTTP/1.1, HTTP/2, and HTTP/3 simultaneously is not a strategy of "cling to one modern protocol for peak performance." Rather, it is a pragmatic bet that **"whatever the client, network, or infrastructure, the service remains reachable, and the best available protocol is chosen automatically."**

This design choice suits a **community platform that encounters diverse end-devices and network conditions** far better than a plain blog engine. fly.board is not abandoning HTTP/3 performance; it is preserving HTTP/2 and HTTP/1.1 as safety nets for the moments when HTTP/3 is impossible.
