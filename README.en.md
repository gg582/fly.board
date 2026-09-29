# fly.board

![fly.board logo](img/logo.png)

> One of the few simple blog engines that holds **1,000,000 concurrent connections** on a single desktop-class host (cleartext HTTP/1.1, measured), and runs the C10k/C100k TLS load suites at 100% success.
> A lightweight board-and-blog engine built on the C-based CWIST web framework, supporting HTTPS/3, Argon2id, PQC signatures, and NATS messaging.

## Features

- **Connection-Scalable** – Stack+heap C implementation on cwist's event-driven reactor. **1,000,000 concurrent cleartext HTTP/1.1 connections** held and served in one measured run; anonymous public pages are served from a Big Dumb Reply cache.
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

### `s3.settings` (optional)

S3-compatible object storage for uploaded files (AWS S3, MinIO, R2, B2). Entirely optional — when empty, files stay on local disk under `public/uploads/`. Supports `mode=mirror` (keep local copy + S3 backup) and `mode=offload` (move to S3, serve downloads via presigned redirects). Full reference and example configurations: [S3.md](S3.md).

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

### What These Benchmarks Measure

Two different things, which earlier versions of this section mixed up:

- **Concurrent connections** (the classic C10K/C1M meaning): how many connections the server holds open and serves at the same time. Measured with `tools/connhold` via `run_c1m_held_bench.sh`.
- **Request churn over held connections**: the `h2load` suite (`run_c10k_bench.sh`, `run_c100k_bench.sh`, `run_c1m_bench.sh`). `h2load` runs with `-r` (rate limit), so the RPS figures reflect the configured load, not a throughput ceiling. `run_c1m_bench.sh` is C100K concurrency with 1,000,000 requests; the name is historical.

### Host Environment

| Item | Value |
|------|-------|
| OS | Linux 6.12.107+deb13-amd64 (Debian 13) |
| CPU | AMD Ryzen 5 5600X (6 cores / 12 threads) |
| RAM | 62 GiB |
| GCC | 14.2.0 (Debian 14.2.0-19) |
| Load generators | h2load nghttp2/1.64.0, `tools/connhold` (BoringSSL) |
| CWIST | `main` at `468a94d7` (2026-09-29) |
| TLS certificate | ECDSA P-256 (`keygen.sh` default) |
| Serving mode | `CWIST_C1M_MODE=1` (event-driven reactor) |

### System Tuning

| Parameter | Value |
|-----------|-------|
| ulimit -n | 1,050,000 |
| fs.file-max | 8,388,608 (1M held connections need 2M fds, client + server) |
| fs.nr_open | 1,050,000 |
| net.netfilter.nf_conntrack_max | 4,194,304 (loopback connections are tracked too) |
| net.core.somaxconn | 1,050,000 |
| net.ipv4.tcp_max_syn_backlog | 1,050,000 |
| net.ipv4.ip_local_port_range | 1024 65535 |
| vm.max_map_count | 1,048,576 |
| kernel.pid_max | 4,194,304 |
| CPU governor | ecodemand |

### C1M: 1,000,000 Concurrent Connections (2026-09-29)

`run_c1m_held_bench.sh`: 12 workers, client and server on the same host, 1,000,000 connections spread over 48 loopback addresses. Each connection sends `GET /robots.txt`, then repeats it every 120 s so the keep-alive timer never fires. Failed connections are counted, never retried.

| | Cleartext HTTP/1.1 | TLS 1.3 + HTTP/1.1 |
|---|---|---|
| Connections opened | 1,000,000 | 1,000,000 |
| Connect rate | 40,000 / s | 20,000 / s |
| Peak held (open, past TLS) | **1,000,000** | 395,729 |
| Peak served at the same time | **1,000,000** | **25** |
| Responses (incl. keep-alive GETs) | 1,124,246 | 52 |
| Failed connections | 0 | 1,000,000 |
| Server memory at peak (PSS, all workers) | 24.0 GB | 7.1 GB |

- **Cleartext HTTP/1.1 holds and serves all 1,000,000 connections.** cwist's cleartext path is event-driven, so an idle connection costs memory (about 24 KB each here) but no thread.
- **TLS connections are held but not served concurrently.** The handshake runs on non-blocking shepherd threads, but a connection past the handshake is served on an HTTPS pool thread for its whole lifetime (HTTP/1.1 keep-alive waits there for the next request, HTTP/2 until the connection goes idle). Only about as many TLS connections as there are pool threads are served at once; the rest wait, and are closed by the server's 45 s handshake budget or the client's 60 s response deadline.
- Load-shape pitfall: Linux `connect()` hands out even ephemeral ports first and then falls back to a slow odd-port search, so each destination address gives ~32k fast connections. With 24 addresses every run stalled near 774k; use at least `connections / 32,000` addresses.

### Memory per Connection

Measured on 2026-09-29 by summing PSS over all server worker processes (not the master alone).

| Case | Server | Kernel (slab + TCP buffers) | Client |
|---|---|---|---|
| 100k held TLS/HTTP/2 conns, h2load C100k | ~6.7 KB (1.11 GB warm idle → 1.77 GB) | ~12.7 KB | h2load ~60 KB |
| 1M held cleartext HTTP/1.1 conns, connhold | ~24 KB (24.0 GB total) | — | connhold ~0.06 KB |

> **Correction:** earlier versions of this README claimed RSS stayed at ~110–146 MB from C10k through C1m. Those figures were `/usr/bin/time -v` maximum RSS of the master process only; `cwist_app_listen()` forks the serving workers, and their memory was never counted. The totals above replace them.

### h2load Suite: Request Churn (2026-09-29)

| Test | Concurrent conns | Requests | Succeeded | Wall time | RPS (sum of processes) |
|---|---|---|---|---|---|
| C10k (4 workers) | 10,000 | 20,000 | **100%** | 4.56 s | 7,445 |
| C100k (12 workers) | 100,000 | 200,000 | **100%** | 24.31 s | 9,254 |
| C1m churn (12 workers) | 100,000 | 1,000,000 | **100%** | 49.65 s | 21,812 |

Wall time is the server process lifetime (startup and the 5 s drain included). Responses are the full 79 KB front page; h2load does not ask for compression.

Earlier the same day, with an RSA-4096 certificate, C100k fell to 72.6% and C1m churn to 65.2%: nearly all busy CPU went into the RSA CertificateVerify signature of each full TLS 1.3 handshake, so queued handshakes hit the 45 s budget. Switching to ECDSA P-256 (`keygen.sh` default), running route handlers on request workers, and serving anonymous public pages from the route Big Dumb Reply cache restored 100%.

**Key Takeaways**

- **C1M, cleartext:** 1,000,000 concurrent HTTP/1.1 connections held and served on one desktop-class host, zero failures.
- **C1M, TLS:** connections are accepted and pass the handshake, but concurrent *service* of TLS connections is bounded by the HTTPS pool threads. TLS C100k churn (h2load) is at 100%.
- **Per-connection memory is real:** ~7 KB (TLS/HTTP/2, h2load C100k) to ~24 KB (cleartext, 1M held) on the server; plan RAM accordingly.
- **Handshake cost dominates TLS churn:** use an ECDSA certificate.

### Throughput Benchmark

> Measured in 2026-08, before the 2026-09 changes (async routes, route BDR cache, ECDSA certificate); not re-run.

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
