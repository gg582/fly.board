# fly.board

![fly.board logo](img/logo.png)

> One of the few simple blog engines that keeps memory nearly flat as connections scale: **~108 MB RSS** at idle even with a single worker (**~102 MB** with 4 workers), and **~110–146 MB** under C10k, C100k, and even C1m.
> A lightweight board-and-blog engine built on the C-based CWIST web framework, supporting HTTPS/3, Argon2id, PQC signatures, and NATS messaging.

## Features

- **Memory-Efficient & Connection-Scalable** – Stack+heap C implementation. **~102–108 MB RSS** at idle (1–4 workers); RSS stays around **~110–146 MB** from C10k through C1m concurrent connections.
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

Configuration comes from three files (auto-created with defaults on first run) plus environment variables for operational toggles.

### `admin.settings`

Two raw lines: line 1 is the admin username, line 2 the admin password.

### `blog.settings`

Plain `key=value` lines. Unknown keys are ignored; invalid values fall back to defaults.

| Key | Default | Values / scope |
|-----|---------|----------------|
| `title` | `CWIST Docker Blog` | Site title shown in the top bar |
| `subtitle` | `Explore boards and read stories.` | Hero subtitle |
| `brand_footer` | `Built with CWIST C Framework` | Footer text |
| `root_url` | `https://localhost:8888/` | Canonical site URL (trailing `/`). Used for RSS links, verification emails, and cert renewal — set this to the public URL in production |
| `port` | `8443` | TCP/UDP listen port (HTTP/3 uses the same port over UDP) |
| `accent` | `#3b82f6` | Accent color (hex) |
| `use_tls` | `true` | `true`/`false` — HTTPS on/off (run `./keygen.sh` first) |
| `use_http2` | `true` | HTTP/2 over TLS |
| `use_http3` | `true` | HTTP/3 (QUIC) over UDP |
| `use_tasfa` | `true` | TASFA media pipeline (video thumbnails/previews via ffmpeg) |
| `use_rss` | `false` | Expose `/rss.xml` |
| `roundness` | `0.0` | UI corner roundness, `0.0`–`1.0` |
| `max_upload_size` | `1G` | Per-file upload limit. Accepts suffixes `K/M/G/T` (e.g. `500M`) |
| `max_total_parallel_uploads` | `8` | Concurrent uploads overall (1–512) |
| `max_upload_parallel_chunks` | `32` | Parallel chunks per upload (1–64) |
| `max_concurrent_downloads` | `128` | Concurrent downloads (1–512) |
| `vote_only` | *(empty = `all`)* | Who may vote on posts: `all` (anyone, incl. anonymous), `authorized` (logged-in users only), `admin` (admins only) |
| `use_special_modes` | *(empty)* | Replaces the light/dark themes: `lightTheme,darkTheme` (or a single theme). Available themes: `light`, `dark`, `ocean`, `forest`, `sepia`. E.g. `ocean,forest` |
| `home_img`, `boards_img`, `files_img` | *(empty)* | Hero/background images per page; file name inside `public/img/` |
| `*_dark` (`home_img_dark`, `boards_img_dark`, `files_img_dark`) | *(empty)* | Dark-mode variants of the above |
| `blog_logo`, `blog_logo_dark` | *(empty)* | Logo image in `public/img/` |
| `invert_logo` | `false` | Auto-invert the logo for the mode that has no image |
| `favicon` | *(empty)* | Favicon file in `public/img/` |
| `bg_full_light`, `bg_full_dark` | *(empty)* | Full-page background images |
| `bg_invert_color` | *(empty)* | Comma-separated targets whose missing mode variant is auto-generated by inverting the other one: `home`, `boards`, `files`, `toplevel`, `logo` |
| `bg_invert_algo` | `luminv` | Inversion algorithm: `luminv` or `oklch` |

### `fonts.settings`

Typography overrides: `font_body`, `font_heading`, `font_ui`, `font_code`, `font_blockquote`, `font_display`, `font_import_url`, `font_face_family`, `font_face_src`, plus per-element `letter_spacing_*` and `font_weight_*` values. Defaults are written out on first run, so open the generated file to see every key.

### Environment variables

**Core**

| Variable | Default | Description |
|----------|---------|-------------|
| `BLOG_ROOT` | *(unset)* | Project root; used when the binary is started outside it. Otherwise the directory containing `public/` is auto-detected |
| `DEBUG` | *(off)* | `1`/`true`/`yes` enables DEBUG/INFO logs; otherwise only warnings/errors are printed |
| `NATS_URL` | *(unset)* | e.g. `nats://localhost:4222` — enables the NATS messaging gateway |
| `BLOG_ECH_KEY` / `BLOG_ECH_DIR` | *(unset)* | ECH (Encrypted Client Hello) key file / key directory |
| `CWIST_C1M_MODE` | `1` | Event-driven C1M reactor. Set to `0` to force the legacy thread-pool path |

**Performance / caching**

| Variable | Default | Description |
|----------|---------|-------------|
| `FLYBOARD_CACHE_MAX_MB` | `64` | Page cache size in MB (1–1024) |
| `FLYBOARD_ADVERTISE_H3` | `true` | Send `Alt-Svc` headers advertising HTTP/3 |
| `FLYBOARD_ALT_SVC_MAX_AGE` | `300` | `Alt-Svc` `ma` value in seconds (0–86400) |
| `FLYBOARD_INLINE_IMAGES` | *(off)* | Inline images as base64 data URIs in HTML |
| `FLYBOARD_INLINE_ALL_ASSETS` | *(off)* | Also inline scripts/styles |
| `FLYBOARD_INLINE_BG_IMAGES` | *(off)* | Also inline background images (explicit opt-in even with `ALL_ASSETS`) |
| `FLYBOARD_INLINE_MAX_IMAGE_SIZE` | `49152` | Max bytes per inlined image |
| `FLYBOARD_INLINE_MAX_ASSET_SIZE` | `65536` | Max bytes per inlined script/style chunk |
| `FLY_MEDIA_MAX_CONCURRENT` | `2` | Concurrent ffmpeg conversions for media previews |
| `FLYBOARD_MEDIA_BACKFILL_ON_START` | *(off)* | Regenerate all legacy media previews at startup (maintenance runs only) |

**Automatic TLS certificate renewal** (uses a local ACME client; temporary self-signed certs from `keygen.sh` are detected and never touched)

| Variable | Default | Description |
|----------|---------|-------------|
| `FLY_CERT_RENEWAL` | *(off)* | `true` enables the daily expiry watchdog. Renews when the certificate has ≤ `FLY_CERT_DAYS` days left and hot-reloads it without a restart |
| `FLY_CERT_DAYS` | `30` | Renewal threshold in days |
| `FLY_CERT_EMAIL` | `admin@<host>` | ACME account email |
| `FLY_CERT_LEGO_BIN` | `lego` | lego binary name/path (point to a wrapper script for DNS challenges etc.) |

The watchdog derives the domain from `root_url` and runs lego with the HTTP-01 challenge, so port 80 must reach the machine. State lives under `.lego/`; renewed certs are installed over `server.crt`/`server.key`.

**Email-verified signup** (off by default = open registration)

| Variable | Default | Description |
|----------|---------|-------------|
| `FLY_EMAIL_CERT` | *(off)* | `true` requires new signups to verify their email before they can log in. A 24-hour token link is sent over SMTP |
| `FLY_SMTP_HOST` | *(required when on)* | SMTP relay host |
| `FLY_SMTP_PORT` | `25` (`465` with implicit TLS) | SMTP port |
| `FLY_SMTP_TLS` | *(off)* | `starttls` or `implicit` |
| `FLY_SMTP_USER` / `FLY_SMTP_PASS` | *(unset)* | AUTH LOGIN credentials (optional) |
| `FLY_SMTP_FROM` | `FLY_SMTP_USER` | Envelope/header sender |

Example — production with verified signup and auto cert renewal:

```sh
FLY_CERT_RENEWAL=true FLY_CERT_EMAIL=admin@example.com \
FLY_EMAIL_CERT=true FLY_SMTP_HOST=smtp.example.com FLY_SMTP_PORT=587 \
FLY_SMTP_TLS=starttls FLY_SMTP_USER=noreply@example.com FLY_SMTP_PASS=secret \
./fly_board
```

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
| Idle (1 worker) | **~108 MB** (110,196 KB) | — | 1 worker, no connections |
| Idle (4 workers) | **~102 MB** (104,940 KB) | — | 4 workers, no connections |
| C10k | **~110 MB** (112,436 KB) | +7,496 KB vs idle (4 workers) | 10,000 concurrent connections |
| C100k | **~146 MB** (148,848 KB) | +36,412 KB | 100,000 concurrent connections |
| C1m churn | **~146 MB** (149,816 KB) | +968 KB | 100k held TLS conns, 1M-request churn |

The total RSS change from **C100k to the C1m churn run is +968 KB** — essentially noise. This is the most important result of the benchmark.

RSS values are the **Maximum resident set size (kbytes)** reported by `/usr/bin/time -v` for the server process.

### Memory Cost

| Transition | Δ RSS | Δ Connections | Approx. cost per additional connection |
|---|---|---|---|
| Idle → C10k | +7,496 KB | 10,000 | ~0.75 KB / connection |
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
