# fly.board

![fly.board logo](img/logo.png)

> One of the few simple blog engines that keeps memory nearly flat as connections scale: **~104 MB RSS** at idle (4 workers; maintains **68–120 MB** on a real production server with a single worker), and **~110–146 MB** under C10k, C100k, and even C1m.
> A lightweight board-and-blog engine built on the C-based CWIST web framework, supporting HTTPS/3, Argon2id, PQC signatures, and NATS messaging.

## Features

- **Memory-Efficient & Connection-Scalable** – Stack+heap C implementation. **~104 MB RSS** at idle; RSS stays around **~110–146 MB** from C10k through C1m concurrent connections.
- **Modern Transport** – TLS 1.3 + HTTP/3 (QUIC) by default. Optional ECH (Encrypted Client Hello).
- **Secure Auth** – Client-side SHA-512 prehash + server-side **Argon2id** (OpenSSL 3 KDF). JWT session cookies.
- **Board / Blog Hybrid** – Slug-based markdown posts + multiple boards + nested comments.
- **Real-time Preview** – Server-side preview rendered instantly from the markdown editor.
- **PQC Signatures** – Attach/verify post-quantum cryptography (PQC) based signatures on posts.
- **File Storage** – ≤1 MB in SQLite, larger files on volume. Auto-embed images/videos/audio.
- **NATS Integration** – Distributed messaging gateway via `NATS_URL` environment variable.
- **Dark Mode** – Cookie-based theme switching with dynamic CSS variables.

## Build

```sh
make
./keygen.sh
```

Dependencies:
- [CWIST](https://github.com/religiya-serdtsa/cwist) — TLS 1.3 / HTTP/3 (QUIC) is handled by the embedded BoringSSL inside CWIST; no extra setup required.
- OpenSSL 3.x (Argon2id KDF)
- ngtcp2 / nghttp3 (HTTP/3)
- cJSON, SQLite3

`Makefile` clones and builds `third_party/md4c` as a static library.

## Run

```sh
./fly_board
```

The default port follows the `port` value in `blog.settings` (default 9443).

```text
https://localhost:9443
```

HTTP/3 listens on the same port over UDP.

### Enable ECH (optional)

```sh
BLOG_ECH_KEY=ech/server.ech ./fly_board
# or
BLOG_ECH_DIR=ech ./fly_board
```

If the OpenSSL build does not support ECH, a warning is logged and the server continues with regular HTTPS/3.

### NATS Integration (optional)

```sh
NATS_URL=nats://localhost:4222 ./fly_board
```

## Key Features

| Feature | Path | Description |
|---------|------|-------------|
| Home | `/` | Latest post list |
| Boards | `/boards` | Multi-board management (admin-only support) |
| Post | `/post/:slug` | md4c markdown rendering + comments + attachments |
| Login/Register | `/login`, `/register` | Argon2id + JWT cookie |
| Profile | `/profile` | Nickname, bio, profile picture, join date |
| Account Settings | `/account/settings` | Profile edit |
| Password Change | `/account/password` | Verify current password, rehash with Argon2id |
| Admin | `/admin/users` | Change user roles, delete users |
| File Storage | `/files` | Upload/download/delete |

## Configuration

- `blog.settings` – Blog title, subtitle, footer, port, and upload limits
- `admin.settings` – Admin account (2 lines: `username`\n`password`)

## Database

SQLite3 (`data/blog.db`). Schema is auto-migrated on app startup.

```
users       – accounts, Argon2id hashes, roles, profiles
boards      – board name/slug/description/admin_only
posts       – markdown body, PQC signature, summary
files       – attachment path/size/MIME
comments    – nested comments (target_type, parent_id)
board_permissions – private board access permissions
```

## Architecture

```
CWIST (HTTP/3, TLS 1.3)
  ├── src/auth/     – Argon2id, JWT, sessions
  ├── src/db/       – SQLite3 CRUD
  ├── src/handlers/ – routing/business logic
  ├── src/render/   – cwist_html_element SSR + md4c
  ├── src/crypto/   – PQC sign/verify
  └── src/nats/     – messaging Pub/Sub
```

## License

MIT License

---

## Scalability Benchmark

### What This Benchmark Measures

These tests use `h2load` **with the `-r` (rate-limit) option**. They are intentionally **not** maximum-throughput tests. Instead, they measure whether the server can **sustain a massive number of concurrent HTTP/2 connections** while processing a controlled, per-process request rate.

Because the load is rate-limited:

- The reported **RPS reflects the configured request rate**, not the server's absolute throughput ceiling.
- The headline metric is **resident-set-size (RSS) stability** as connections grow from 10,000 to 1,000,000.

The worker count is scaled with the load to keep each test realistic: **4 workers** for C10k, **12 workers** for C100k, and **12 workers** for C1m. This also explains the different CPU-usage figures across the three runs.

### Host Environment

| Item | Value |
|------|-------|
| OS | Linux 6.12.101+deb13-amd64 (Debian 13) |
| Architecture | x86_64 |
| CPU | AMD Ryzen 5 5600X (6 cores / 12 threads) |
| RAM | 62 GiB |
| GCC | 14.2.0 (Debian 14.2.0-19) |
| OpenSSL | 3.5.6 |
| Benchmark Tool | h2load nghttp2/1.64.0 |
| CWIST | `libcwist.a` from the sibling cwist checkout (2026-08-29, arena bump allocator, shared req/res arena, 256KB worker stacks, HTTP/3 hardening, sharded TLS handshake shepherd) |

> **Serving mode:** as of 2026-08-24 fly.board runs with `CWIST_C1M_MODE=1`
> (cwist's event-driven reactor path with the non-blocking TLS handshake
> shepherd). The benchmark results below were re-collected on 2026-08-28
> against the current cwist build.

### System Tuning

| Parameter | Value |
|-----------|-------|
| ulimit -n | 1,050,000 |
| fs.file-max | 2,097,152 |
| fs.nr_open | 1,050,000 |
| net.core.somaxconn | 1,050,000 |
| net.ipv4.tcp_max_syn_backlog | 1,050,000 |
| net.ipv4.ip_local_port_range | 1024 65535 |
| vm.max_map_count | 1,048,576 |
| kernel.pid_max | 4,194,304 |
| CPU governor | ecodemand |

### Memory Usage

| State | RSS | Δ from previous | Notes |
|-------|-----|-----------------|-------|
| Idle | **~104 MB** (106,192 KB) | — | 4 workers, no connections |
| C10k | **~110 MB** (112,436 KB) | +6,244 KB | 10,000 concurrent connections |
| C100k | **~146 MB** (148,848 KB) | +36,412 KB | 100,000 concurrent connections |
| C1m churn | **~146 MB** (149,816 KB) | +968 KB | 100k held TLS conns, 1M-request churn |

The total RSS change from **C100k to the C1m churn run is +968 KB** — essentially noise. This is the most important result of the benchmark.

RSS values are the **Maximum resident set size (kbytes)** reported by `/usr/bin/time -v` for the server process.

### Memory Cost

| Transition | Δ RSS | Δ Connections | Approx. cost per additional connection |
|---|---|---|---|
| Idle → C10k | +6,244 KB | 10,000 | ~0.6 KB / connection |
| C10k → C1m churn | +37,380 KB | — | ~0.4 KB / added held connection; C100k → C1m is +968 KB (noise) |

The initial jump from idle to C10k pays for TLS state, connection buffers, and worker overhead up front. From C10k to C100k the cost stays near ~0.4 KB per additional held connection, and the C100k-to-C1m RSS change (+968 KB) is pure measurement noise — the per-connection memory cost is effectively flat.

### C10k Concurrent Connection Test

Measured with `h2load` maintaining 10,000 concurrent connections.

| Item | Value |
|------|-------|
| Workers | 4 |
| Concurrent connections | 10,000 |
| Duration | 12.05 s |
| Max RSS | **~110 MB** (112,436 KB) |
| CPU usage | ~365% |
| User time | 41.05 s |
| System time | 3.04 s |
| Major page faults | 2 |
| Minor page faults | 16,948 |
| Voluntary context switches | 58,050 |
| Involuntary context switches | 14,828 |
| File system outputs | 256 |
| Total requests | 20000 |
| Total succeeded | 20000 |
| Total failed | 0 |
| Approx total RPS | **2285.22** |
| Success rate | **100.00%** |
| Exit status | **0** |
### C100k Concurrent Connection Test

Measured with `h2load` maintaining 100,000 concurrent connections.

| Item | Value |
|------|-------|
| Workers | 12 |
| Concurrent connections | 100,000 |
| Duration | 1:23.49 |
| Max RSS | **~146 MB** (148,848 KB) |
| CPU usage | ~815% |
| User time | 653.83 s |
| System time | 26.78 s |
| Major page faults | 0 |
| Minor page faults | 76,332 |
| Voluntary context switches | 446,557 |
| Involuntary context switches | 617,777 |
| File system outputs | 336 |
| Total requests | 200000 |
| Total succeeded | 200000 |
| Total failed | 0 |
| Approx total RPS | **2785.16** |
| Success rate | **100.00%** |
| Exit status | **0** |
### C1m Churn Test (redesigned 2026-08-23, fixed 2026-08-24)

The old "1,000,000 concurrent TLS connections" target was retired: the HTTPS
path parks one worker thread per live connection, so held-connection
concurrency is capped at workers x threads, far below 1M.  (cwist's
**cleartext** HTTP/1.x path is event-driven and did reach
1,000,000/1,000,000 held connections — see the cwist README.)  The C1m test
measures churn: 20 h2load processes x 50,000 requests over 100,000
concurrently held TLS connections, bounded by a watchdog.

| Item | Value |
|------|-------|
| Workers | 12 |
| Load shape | 20 x (-c 5000 -n 50000 -r 1000 -T 30) |
| Totals | 1,000,000 requests over 100,000 held connections |
| Outcome | **completed — no stall** |
| Total succeeded | **1,000,000 / 1,000,000 (100.0%)** |
| Errored | 0 |
| Wall time | ~1:36 (h2load "finished in" 63.7-89.3 s per process) |
| Phantom connections | 0 (client/server ESTABLISHED counts matched) |
| Server shutdown | clean, exit 0 |

History: the 2026-08-23 run of this same load deadlocked at ~85k
connections.  Root cause (fixed in cwist `perf(https): non-blocking TLS
handshake shepherd`): the TLS handshake ran synchronously inside worker
threads with 30 s poll waits, so a few hundred laggy clients parked the
whole pool, the accept queue overflowed, and overflowing handshakes were
dropped silently, leaving clients ESTABLISHED with no server-side socket.
Handshakes now run on non-blocking shepherd threads; only established
sessions occupy pool workers.

2026-08-29 follow-up: under sustained C100k connect bursts the single shepherd
thread itself became the bottleneck — its fixed 45 s handshake deadline was
consumed by queue delay, so healthy handshakes were reaped and h2load reported
them as errored (C100k fell to 100.00% on 2026-08-28). The shepherd is now
sharded per process (up to 8 threads, hashed by fd) and the deadline bounds
stalls, not queueing: any handshake progress refreshes the budget. C100k is
back to 200,000/200,000 (100.00%).

> Note: Values measured while maintaining actual client connections over HTTP/2 (TLS 1.3). Worker counts differ per test; see "What This Benchmark Measures".

**Key Takeaways**

- **Connection Scalability**: RSS stays around **~110–146 MB** from 10,000 through 1,000,000 concurrent connections. The per-connection memory cost is effectively flat.
- **Stable under Realistic Load**: C10k completed with **100% success** and C100k with **100.00%** while staying inside the same memory envelope.
- **Memory Envelope Holds at C1m scale**: the C1m churn run (1M requests over 100k held TLS connections) completes without a stall at **100% success** and RSS stays ~146 MB — no memory spiral, no crash.
- **Data Safety**: SQLite safely persisted all data on SIGINT (256 FS outputs at C10k).

### Throughput Benchmark

The benchmark above measures **connection scalability**, not absolute **request throughput**. To measure the server's raw throughput ceiling, an unbounded test was run with `h2load` (no `-r` rate limit) over HTTP/2.

| Item | Value |
|------|-------|
| Command | `h2load -c512 -n100000 https://127.0.0.1:8888/` |
| Workers | 12 |
| Concurrent connections | 512 |
| Total requests | 100,000 |
| Succeeded | 100,000 |
| Failed / Errored / Timeout | 0 |
| Duration | 13.95 s |
| Mean RPS | **7167.28** |
| Mean throughput | **290.51 MB/s** |
| Request latency (h2load `time for request`) | min 183 µs, mean 30.69 ms, max 209.00 ms, sd 11.18 ms |
| Approx. percentile latency* | p50 ~30.7 ms, p95 ~49.1 ms, p99 ~56.7 ms |

\* Percentiles are approximated from the reported mean and standard deviation; h2load prints min/max/mean/sd by default. Run with `--latency-collect` for exact percentile histograms.

#### HTTP/1.1 comparison with `wrk`

For comparison, the same endpoint was tested with `wrk` over HTTP/1.1. These are different protocols and different tools, so the numbers below are **not directly comparable** to the HTTP/2 h2load results above.

| Item | Value |
|------|-------|
| Command | `wrk -t12 -c512 -d60s https://127.0.0.1:8888/` |
| Duration | 60 s |
| Requests/sec | **1282.49** |
| Transfer/sec | 52.29 MB |
| Latency | Avg 138.61 ms, Stdev 39.26 ms, Max 311.70 ms |

These numbers show the engine's absolute throughput ceiling under a focused, non-rate-limited load. They are separate from the connection-scalability tests above.
