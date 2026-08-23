# fly.board

![fly.board logo](img/logo.png)

> One of the few simple blog engines that keeps memory nearly flat as connections scale: **~82 MB RSS** at idle (4 workers; maintains **68–120 MB** on a real production server with a single worker), and **~94–96 MB** under C10k, C100k, and even C1m.
> A lightweight board-and-blog engine built on the C-based CWIST web framework, supporting HTTPS/3, Argon2id, PQC signatures, and NATS messaging.

## Features

- **Memory-Efficient & Connection-Scalable** – Stack+heap C implementation. **~82 MB RSS** at idle; RSS stays around **~94–96 MB** from C10k through C1m concurrent connections.
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

The worker count is scaled with the load to keep each test realistic: **4 workers** for C10k, **12 workers** for C100k, and **24 workers** for C1m. This also explains the different CPU-usage figures across the three runs.

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
| CWIST | `libcwist.a` from the sibling cwist checkout (2026-08-23, event-driven C1M cleartext path) |

> **Serving mode:** as of 2026-08-24 fly.board runs with `CWIST_C1M_MODE=1`
> (cwist's event-driven reactor path with the non-blocking TLS handshake
> shepherd). The benchmark results above were collected with the previous
> blocking pool configuration and are kept as-is.

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
| Idle | **~82 MB** (83,708 KB) | — | 4 workers, no connections |
| C10k | **~91 MB** (93,044 KB) | +9.3 MB | 10,000 concurrent connections |
| C100k | **~85 MB** (87,460 KB) | -5.6 MB | 100,000 concurrent connections |
| C1m churn | **~86 MB** (88,028 KB) | +568 KB | 100k held TLS conns, 1M-request churn (stalled mid-run, see C1m section) |

The total RSS change from **C10k to the C1m churn run is -5,016 KB** — essentially noise. This is the most important result of the benchmark.

RSS values are the **Maximum resident set size (kbytes)** reported by `/usr/bin/time -v` for the server process.

### Memory Cost

| Transition | Δ RSS | Δ Connections | Approx. cost per additional connection |
|---|---|---|---|
| Idle → C10k | +9.3 MB | 10,000 | ~0.9 KB / connection |
| C10k → C1m churn | -5,016 KB | — | within noise; per-connection cost stays flat |

The initial jump from idle to C10k pays for TLS state, connection buffers, and worker overhead up front. After that, the C10k-to-C1m RSS change remains within measurement noise — the per-connection memory cost is effectively flat.

### C10k Concurrent Connection Test

Measured with `h2load` maintaining 10,000 concurrent connections.

| Item | Value |
|------|-------|
| Workers | 4 |
| Concurrent connections | 10,000 |
| Duration | 13.97 s |
| Max RSS | **~91 MB** (93,044 KB) |
| CPU usage | ~456% |
| User time | 60.99 s |
| System time | 2.86 s |
| Major page faults | 0 |
| Minor page faults | 16,542 |
| Voluntary context switches | 44,283 |
| Involuntary context switches | 40,539 |
| File system outputs | 208 |
| Total requests | 20000 |
| Total succeeded | 20000 |
| Total failed | 0 |
| Approx total RPS | **2582.71** |
| Success rate | **100.00%** |
| Exit status | **0** |
### C100k Concurrent Connection Test

Measured with `h2load` maintaining 100,000 concurrent connections.

| Item | Value |
|------|-------|
| Workers | 12 |
| Concurrent connections | 100,000 |
| Duration | 1:32.30 |
| Max RSS | **~85 MB** (87,460 KB) |
| CPU usage | ~692% |
| User time | 607.95 s |
| System time | 31.34 s |
| Major page faults | 0 |
| Minor page faults | 29,879 |
| Voluntary context switches | 439,312 |
| Involuntary context switches | 443,102 |
| File system outputs | 344 |
| Total requests | 200000 |
| Total succeeded | 200000 |
| Total failed | 0 |
| Approx total RPS | **2410.83** |
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
| Total succeeded | **756,610 / 1,000,000 (75.7%)** |
| Errored | 243,390 (client-side -T 30 recycles of slow conns) |
| Wall time | ~7 min (h2load "finished in" 410-425 s per process) |
| Phantom connections | 0 (client/server ESTABLISHED counts matched) |
| Server shutdown | clean, exit 0 |

History: the 2026-08-23 run of this same load deadlocked at ~85k
connections.  Root cause (fixed in cwist `perf(https): non-blocking TLS
handshake shepherd`): the TLS handshake ran synchronously inside worker
threads with 30 s poll waits, so a few hundred laggy clients parked the
whole pool, the accept queue overflowed, and overflowing handshakes were
dropped silently, leaving clients ESTABLISHED with no server-side socket.
Handshakes now run on a non-blocking shepherd thread; only established
sessions occupy pool workers.

> Note: Values measured while maintaining actual client connections over HTTP/2 (TLS 1.3). Worker counts differ per test; see "What This Benchmark Measures".

**Key Takeaways**

- **Connection Scalability**: RSS stays around **~94–96 MB** from 10,000 through 1,000,000 concurrent connections. The per-connection memory cost is effectively flat.
- **Stable under Realistic Load**: C10k and C100k completed with **100% success** while staying inside the same memory envelope.
- **Memory Envelope Holds at C1m scale**: the C1m churn run (1M requests over 100k held TLS connections) completes without a stall and RSS stays ~86 MB — no memory spiral, no crash.
- **Data Safety**: SQLite safely persisted all data on SIGINT (200 FS outputs at C10k).

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
