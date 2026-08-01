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
| OS | Linux 7.1.0-mountain-rc6+ |
| Architecture | x86_64 |
| CPU | 12 logical cores |
| RAM | 62 GiB |
| GCC | 14.2.0 (Debian 14.2.0-19) |
| OpenSSL | 3.5.6 |
| Benchmark Tool | h2load nghttp2/1.64.0 |
| CWIST | `/usr/local/lib/libcwist.a` |

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
| C10k | **~96 MB** (96,252 KB) | +12.25 MB | 10,000 concurrent connections |
| C100k | **~94 MB** (94,352 KB) | -1,900 KB | 100,000 concurrent connections |
| C1m | **~95 MB** (94,944 KB) | +592 KB | 1,000,000 concurrent connections |

The total RSS change from **C10k to C1m is -1,308 KB** — essentially noise. This is the most important result of the benchmark.

RSS values are the **Maximum resident set size (kbytes)** reported by `/usr/bin/time -v` for the server process.

### Memory Cost

| Transition | Δ RSS | Δ Connections | Approx. cost per additional connection |
|---|---|---|---|
| Idle → C10k | +12.25 MB | 10,000 | ~1.3 KB / connection |
| C10k → C1m | -1,308 KB | 990,000 | ~-1.4 bytes / additional connection (noise) |

The initial jump from idle to C10k pays for TLS state, connection buffers, and worker overhead up front. After that, the C10k-to-C1m RSS change remains within measurement noise — the per-connection memory cost is effectively flat.

### C10k Concurrent Connection Test

Measured with `h2load` maintaining 10,000 concurrent connections.

| Item | Value |
|------|-------|
| Workers | 4 |
| Concurrent connections | 10,000 |
| Duration | 13.62 s |
| Max RSS | **~96 MB** (96,252 KB) |
| CPU usage | ~578% |
| User time | 72.53 s |
| System time | 6.23 s |
| Major page faults | 0 |
| Minor page faults | 82,722 |
| Voluntary context switches | 1,689,448 |
| Involuntary context switches | 18,959 |
| File system outputs | 200 |
| Total requests | 20000 |
| Total succeeded | 20000 |
| Total failed | 0 |
| Approx total RPS | **2490.60** |
| Success rate | **100.00%** |
| Exit status | **0** |

### C100k Concurrent Connection Test

Measured with `h2load` maintaining 100,000 concurrent connections.

| Item | Value |
|------|-------|
| Workers | 12 |
| Concurrent connections | 100,000 |
| Duration | 1:24.88 |
| Max RSS | **~94 MB** (94,352 KB) |
| CPU usage | ~871% |
| User time | 701.20 s |
| System time | 38.28 s |
| Major page faults | 0 |
| Minor page faults | 292,073 |
| Voluntary context switches | 3,522,910 |
| Involuntary context switches | 184,003 |
| File system outputs | 208 |
| Total requests | 200000 |
| Total succeeded | 200000 |
| Total failed | 0 |
| Approx total RPS | **2541.24** |
| Success rate | **100.00%** |
| Exit status | **0** |

### C1m Concurrent Connection Test

Measured with `h2load` maintaining 1,000,000 concurrent connections.

| Item | Value |
|------|-------|
| Workers | 24 |
| Concurrent connections | 1,000,000 |
| Duration | 7:05.71 |
| Max RSS | **~95 MB** (94,944 KB) |
| CPU usage | ~641% |
| User time | 2517.01 s |
| System time | 215.52 s |
| Major page faults | 0 |
| Minor page faults | 789,451 |
| Voluntary context switches | 23,921,809 |
| Involuntary context switches | 943,712 |
| File system outputs | 208 |
| Total requests | 2000000 |
| Total succeeded | 711274 |
| Total failed | 1288726 |
| Approx total RPS | **1694.48** |
| Success rate | **35.56%** |
| Exit status | **0** |

> Note: Values measured while maintaining actual client connections over HTTP/2 (TLS 1.3). Worker counts differ per test; see "What This Benchmark Measures".

**Key Takeaways**

- **Connection Scalability**: RSS stays around **~94–96 MB** from 10,000 through 1,000,000 concurrent connections. The per-connection memory cost is effectively flat.
- **Stable under Realistic Load**: C10k and C100k completed with **100% success** while staying inside the same memory envelope.
- **Memory Envelope Holds at C1m**: Even when the test hardware could not fully serve all 1,000,000 connections (35.56% success), memory use remained essentially unchanged — the server did not spiral out of control.
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
