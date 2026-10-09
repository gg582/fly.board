# fly.board

![fly.board logo](img/logo.png)

> One of the few simple blog engines that holds **1,000,000 concurrent connections** on a single desktop-class host (re-verified 2026-10-07 on CWIST v3.9 with raised kernel limits over cleartext HTTP/1.1, TLS HTTP/1.1 and TLS HTTP/2, zero failures), and runs the C10k/C100k TLS load suites at 100% success.
> A lightweight board-and-blog engine built on the C-based CWIST web framework, supporting HTTPS/3, Argon2id, PQC signatures, and NATS messaging.

## Features

- **Connection-Scalable** – Stack+heap C implementation on cwist's event-driven reactor. **1,000,000 concurrent connections** held and served in measured runs over cleartext HTTP/1.1, TLS HTTP/1.1 and TLS HTTP/2 (2026-10-07, CWIST v3.9); anonymous public pages are served from a Big Dumb Reply cache.
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
make -j2 fly_board mail-tools
./keygen.sh
```

`fly_board` is the server; `mail-tools` builds the three mail-stack helpers
(`mail-verify`, `mail-import`, `mail-alias-build`) needed for the built-in
mail server — see [docs/mail.md](docs/mail.md). Plain `make` builds both plus
the vendored third-party libraries.

Dependencies:
- [CWIST](https://github.com/religiya-serdtsa/cwist) v3.9 — TLS 1.3 / HTTP/3 (QUIC) is handled by the embedded BoringSSL inside CWIST; no extra setup required.
- OpenSSL 3.x (Argon2id KDF)
- ngtcp2 / nghttp3 (HTTP/3)
- cJSON, SQLite3

The Makefile locates CWIST in this order:

1. `CWIST_ROOT` — a cwist source tree (defaults to `../cwist`, i.e. a checkout
   next to this repository; override with `make CWIST_ROOT=/path/to/cwist`).
2. `CWIST_PREFIX` — an installed prefix holding `lib/libcwist.a` and
   `include/cwist` (defaults to `/usr/local`; the Homebrew/Linuxbrew
   `opt/cwist` keg is auto-detected).
3. Homebrew/Linuxbrew locations, probed automatically.

`Makefile` clones and builds `third_party/md4c` as a static library.

### Quick start (first run)

```sh
make -j2 fly_board mail-tools
./keygen.sh                       # self-signed server.crt / server.key (or install real certs)

# Site admin account: line 1 = username, line 2 = password.
# Auto-created as admin / fly.board if the file is missing — change it before going public.
printf 'admin\nchange-me-now\n' > admin.settings

# Edit the generated settings, at minimum root_url, port, and language.
./fly_board                       # generates blog.settings, fonts.settings, s3.settings,
                                  # robots.settings, upload.settings, flywire.settings with defaults
# ^C, then:
$EDITOR blog.settings
./fly_board
```

On startup the admin account from `admin.settings` is created (or re-synced)
in the `users` table as the site admin — no separate account-registration
step is needed. Opening the site and logging in with those credentials lands
on `/admin`.

### systemd unit

```ini
[Unit]
Description=fly.board blog engine
After=network.target

[Service]
Type=simple
User=flyboard
Group=flyboard
WorkingDirectory=/srv/fly.board
Environment=BLOG_ROOT=/srv/fly.board
ExecStart=/srv/fly.board/fly_board
Restart=on-failure
RestartSec=2
# Hardening
NoNewPrivileges=true
ProtectSystem=full
ProtectHome=true
ReadWritePaths=/srv/fly.board
LimitNOFILE=1048576

[Install]
WantedBy=multi-user.target
```

The server must run from the site root (the directory containing `public/`
and `data/`); it refuses to start otherwise. `BLOG_ROOT` lets systemd start
the binary without a shell `cd`. HTTP/3 uses the same port over UDP, so open
both TCP and UDP in the firewall. Full install walkthrough:
[docs/install.md](docs/install.md).

## Documentation

| Document | Contents |
|---|---|
| [docs/install.md](docs/install.md) | Dependencies, per-distro build steps, troubleshooting |
| [docs/configuration.md](docs/configuration.md) | Every settings file key and environment variable |
| [docs/mail.md](docs/mail.md) | Built-in mail server: Postfix/Dovecot integration, DNS, Mailjet relay |
| [docs/flywire.md](docs/flywire.md) | Primary/replica site synchronization |
| [docs/backup.md](docs/backup.md) | Scheduled backups, archive format, restore procedure |
| [docs/tasfa-compression.md](docs/tasfa-compression.md) | TASFA resumable transfer protocol and compression |
| [SETTINGS.md](SETTINGS.md) | Settings files reference (same content as docs/configuration.md, repo-root form) |
| [TUNABLES.md](TUNABLES.md) | Performance knobs and benchmark environment |
| [INSTALL.debian.md](INSTALL.debian.md) | Debian-specific package list and setup sequence |

## Run

```sh
./fly_board
```

The listen port comes from `port` in `blog.settings` (default 8443).

```text
https://localhost:8443
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
| Drafts / Scheduling | `/account/drafts` | Save drafts, schedule a future publish time, unpublish. Visible only to the author and admins |
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
| `max_upload_parallel_chunks` | `48` | Parallel chunks per upload (1–64) |
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
| `FLYBOARD_CACHE_MAX_MB` | *(unlimited)* | Page cache size in MB (1–1024) |
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
pqc_keys    – every public key that has signed posts on this site
```

## Series and Translations

- **Series**: name a series in the editor's *Series* field (a new name starts one) and set the *Part* number, or leave it empty to keep the post's place / add it at the end. Each post shows the series' reading order with previous/next parts; `/series` lists them, `/series/<id>` shows one with its own RSS feed, and its owner (or an admin) can rename, reorder and remove parts at `/series/<id>/edit`.
- **Translations**: posts, boards and series each have a *Language* and can be linked as translations of one another (*Translation of*). Pages then carry `<html lang>`, `hreflang` alternates and an "Also in" switcher. `language=` in `blog.settings` sets the site default (`ko`).

## Backup and Migration

Run from the site root (where `public/` and `data/` live):

```sh
./fly_board --backup  /srv/backups/site.fbk                 # posts, comments, boards, uploads, images, settings
./fly_board --backup  /srv/backups/site.fbk --with-secrets  # also the signing seed, JWT secret, admin.settings, s3.settings
./fly_board --verify  /srv/backups/site.fbk
./fly_board --restore /srv/backups/site.fbk                 # into a fresh install; --force to replace an existing site
```

- The archive (tar + zstd) holds consistent SQLite snapshots taken while the server runs, and a manifest with the SHA-256 of every file, signed with the site's ML-DSA-65 key. `--verify` and `--restore` refuse an archive whose signature or contents do not match.
- Secrets are sealed with AES-256-GCM under a scrypt key derived from a passphrase (`--passphrase-file`, `FLY_BACKUP_PASSPHRASE`, or a prompt; at least 12 characters).
- Post signatures carry the id of the key that made them, and `pqc_keys` keeps every public key. A site restored without its secrets starts with a new signing key and still verifies every earlier post.
- `--restore --force` keeps each replaced file as `<name>.pre-restore-<time>`. Backups are written `0600` and never under `public/`.
- `./fly_board --sign-posts` signs posts whose signature is missing or no longer verifies.

Markdown export, for static site generators or for checking signatures outside fly.board:

```sh
./fly_board --export-markdown /srv/export                  # posts/<slug>.md with front matter, files/, pqc-keys.json
./fly_board --export-markdown /srv/export --rewrite-links  # attachment links point at files/; signed originals in originals/
./fly_board --verify-markdown /srv/export
```

Scheduled backups: register a target under *Dashboard → Scheduled Backups* (an S3-compatible bucket, or an absolute directory such as a mounted disk). Every day at 03:00 the server then runs `fly_board --scheduled-backup` as a separate process, ships the signed archive to the target and keeps the newest *N*. With a passphrase set, the secrets are sealed into each archive too. The target lives in `backup.settings` (0600, not in git); nothing runs while no target is registered.

Abandoned TASFA transfer sessions (expired, idle for an hour, not locked) are removed at start and daily at 03:00; `./fly_board --sweep-uploads --dry-run` shows what would go.

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

Three different things, which earlier versions of this section mixed up:

- **Concurrent connections** (the classic C10K/C1M meaning): how many connections the server holds open and serves at the same time. Measured with `tools/connhold` via `run_c1m_held_bench.sh`.
- **Request churn over held connections**: the `h2load` suite (`run_c10k_bench.sh`, `run_c100k_bench.sh`, `run_c1m_bench.sh`). `h2load` runs with `-r` (rate limit), so the RPS figures reflect the configured load, not a throughput ceiling. `run_c1m_bench.sh` is C100K concurrency with 1,000,000 requests; the name is historical.
- **Throughput**: unbounded `h2load` and `wrk` runs against the front page.

### Host Environment

| Item | Value |
|------|-------|
| OS | Linux 6.12.107+deb13-amd64 (Debian 13) |
| CPU | AMD Ryzen 5 5600X (6 cores / 12 threads) |
| RAM | 62 GiB |
| GCC | 14.2.0 (Debian 14.2.0-19) |
| Load generators | h2load nghttp2/1.64.0, wrk, `tools/connhold` (BoringSSL) |
| CWIST | v3.9 (`4af0de39`) |
| TLS certificate | ECDSA P-256 (`keygen.sh` default) |
| Serving mode | `CWIST_C1M_MODE=1` (event-driven reactor) |

Client and server run on the same host.

### System Tuning

| Parameter | Value |
|-----------|-------|
| ulimit -n | 1,050,000 |
| fs.file-max | 8,388,608 (1M held connections need 2M fds, client + server) |
| fs.nr_open | 1,050,000 |
| net.netfilter.nf_conntrack_max | 4,194,304 (loopback connections are tracked too) |
| net.core.somaxconn | 65,535 |
| net.ipv4.tcp_max_syn_backlog | 262,144 |
| net.ipv4.ip_local_port_range | 1024 65535 |
| vm.max_map_count | 1,048,576 |
| kernel.pid_max | 4,194,304 |
| CPU governor | ecodemand |

`fs.file-max` and `nf_conntrack_max` must be raised with sudo before a C1M run; the 2026-10-07 CWIST v3.9 run used the values above.

### C1M: 1,000,000 Concurrent Connections (2026-10-07, CWIST v3.9)

`run_c1m_held_bench.sh`: 12 workers, 1,000,000 connections spread over 48 loopback addresses. Each connection sends `GET /robots.txt`, then repeats it every 60–120 s so the keep-alive timer never fires (HTTP/2: a new stream each time). Failed connections are counted, never retried.

| | Cleartext HTTP/1.1 | TLS 1.3 + HTTP/1.1 | TLS 1.3 + HTTP/2 |
|---|---|---|---|
| Connections opened | 1,000,000 | 1,000,000 | 1,000,000 |
| Connect rate | 40,000 / s | 8,000 / s | 8,000 / s |
| Peak held | **1,000,000** | **1,000,000** | **1,000,000** |
| Peak served at the same time | **1,000,000** | **1,000,000** | **1,000,000** |
| Responses (incl. keep-alive GETs) | 2,119,348 | 2,628,499 | 1,677,602 |
| Failed connections | 0 | 0 | 0 |
| Server memory at peak (PSS, all workers) | 16.0 GiB | 18.7 GiB | 27.0 GiB |

- **All three modes hold and serve 1,000,000 concurrent connections with zero failures.**
- Stock `nf_conntrack_max=262,144` caps loopback connections near 262k (each loopback connection costs one conntrack entry); raise it to 4,194,304 for C1M, as in the tuning table above.
- TLS needs CWIST v3.7.1 or later. Before it, a pool thread waited on each idle TLS connection, so only as many TLS connections as pool threads were served at once: the same run held 395,729 TLS connections but served at most 25. v3.7.1 parks idle TLS connections in an epoll set and hands them back to the pool when bytes arrive.
- The TLS connect rate is limited by full handshakes: at 20,000 new connections/s, ~37% of handshakes missed the 45 s handshake budget; at 8,000/s none did.
- Load-shape pitfall: Linux `connect()` hands out even ephemeral ports first and then falls back to a slow odd-port search, so each destination address gives ~32k fast connections. With 24 addresses every run stalled near 774k; use at least `connections / 32,000` addresses.

### Memory

PSS summed over every server process (master and forked workers).

| Case | Server total | Per connection |
|---|---|---|
| Idle, 1 worker | 117 MB | — |
| Idle, 4 workers | 409 MB | — |
| ~91k TLS/HTTP/2 connections (h2load C100k, sampled at 91,286 established) | 7.58 GB | ~83 KB |
| 1M cleartext HTTP/1.1 connections (connhold) | 16.0 GiB | ~16 KB |
| 1M TLS HTTP/1.1 connections (connhold) | 18.7 GiB | ~19 KB |
| 1M TLS HTTP/2 connections (connhold) | 27.0 GiB | ~27 KB |

Client-side costs for planning same-host runs: h2load ~60 KB per connection, connhold ~0.06 KB plus kernel socket memory (~12.7 KB per connection pair at C100k).

> **Correction:** earlier versions of this README claimed ~102–108 MB idle and ~110–146 MB from C10k through C1m. Those were `/usr/bin/time -v` maximum RSS of the master process only; `cwist_app_listen()` forks the serving workers, and their memory was never counted. The totals above replace them.

### h2load Suite: Request Churn (2026-10-07, CWIST v3.9)

| Test | Concurrent conns | Requests | Succeeded | Wall time | RPS (sum of processes) |
|---|---|---|---|---|---|
| C10k (4 workers) | 10,000 | 20,000 | **100%** | 5.06 s | 8,812 |
| C100k (12 workers) | 100,000 | 200,000 | **100%** | 29.06 s | 7,626 |
| C1m churn (12 workers) | 100,000 | 1,000,000 | **100%** | 55.15 s | 19,483 |

Wall time is the server process lifetime, including startup and the shutdown drain. Responses are the full 79 KB front page; h2load does not ask for compression.

Earlier, with an RSA-4096 certificate, C100k fell to 72.6% and C1m churn to 65.2%: nearly all busy CPU went into the RSA CertificateVerify signature of each full TLS 1.3 handshake, so queued handshakes hit the 45 s budget. Switching to ECDSA P-256 (`keygen.sh` default), running route handlers on request workers, and serving anonymous public pages from the route Big Dumb Reply cache restored 100%.

### Throughput Benchmark (2026-10-07, CWIST v3.9)

Unbounded load (no `-r`) against the front page, 12 workers.

| Tool | Command | Result |
|---|---|---|
| h2load (HTTP/2) | `h2load -c512 -n100000 https://127.0.0.1:8888/` | **12,123 req/s**, 963.50 MB/s, 100,000/100,000 succeeded, 8.25 s; request time min 187 µs, mean 20.76 ms, max 200.24 ms |
| wrk (HTTP/1.1) | `wrk -t12 -c512 -d60s https://127.0.0.1:8888/` | **38,775 req/s**, 3.02 GB/s; latency avg 12.55 ms, stdev 7.67 ms, max 185.73 ms; no socket errors |

The two tools use different protocols and are not directly comparable. In 2026-08 the same commands gave 7,167 req/s (h2load) and 1,282 req/s with 77,027 read errors (wrk); anonymous front-page requests are now answered from the route Big Dumb Reply cache. The wrk figure is lower than the 48,829 req/s of 2026-09-29 because the 2026-10-07 run shared the desktop host with a browser and a VM; no socket errors occurred.

**Key Takeaways**

- **C1M:** 1,000,000 concurrent connections held and served on one desktop-class host over cleartext HTTP/1.1, TLS HTTP/1.1 and TLS HTTP/2, zero failures — re-verified on CWIST v3.9 (2026-10-07) with the kernel limits in the tuning table raised.
- **Per-connection memory is real:** ~16–28 KB per held connection on the server; plan RAM accordingly.
- **TLS connect rate is bounded by handshakes:** use an ECDSA certificate; on this host ~8,000 full handshakes/s completed without misses.
