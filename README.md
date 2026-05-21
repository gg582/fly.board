# fly.board

![fly.board logo](img/logo.png)

> One of the few simple blog engines running at **~577 MB RSS** at idle, and **~658 MB** under C10k (10,000 concurrent connections).  
> A lightweight board-and-blog engine built on the C-based CWIST web framework, supporting HTTPS/3, Argon2id, PQC signatures, and NATS messaging.

## Features

- **Memory Efficient** – Stack+heap C implementation. **~577 MB RSS** at idle; **~658 MB** max RSS under 10,000 concurrent connections (C10k).
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

- `blog.settings` – Blog title, subtitle, footer, port, optional `multi_ports`, and upload limits
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

## Performance Benchmark

### Host Environment

| Item | Value |
|------|-------|
| OS | Linux 7.0.0-mountain+ |
| Architecture | x86_64 |
| CPU | AMD Ryzen 5 5600X @ 3.70GHz (6 cores / 12 threads) |
| RAM | 64 GB |
| Disk | Samsung SSD 980 1TB (NVMe) |
| OpenSSL | 3.5.5 |
| Benchmark Tool | wrk, h2load |
| CWIST | `patches/cwist` |

### Max Throughput (RPS)

`wrk -t4 -c400 -d30s` (TLS 1.3)

| Endpoint | Peak RPS | Avg Latency | Notes |
|----------|----------|-------------|-------|
| `/` (Home) | **941.67** | 174.60ms | DB query + markdown rendering |
| `/login` | **927.08** | 175.83ms | Static form |
| `/boards` | **920.36** | 178.16ms | DB-driven list |

> Note: Socket read errors occur during TLS connection teardown but do not affect throughput measurement.

### Memory Usage

| State | RSS | Notes |
|-------|-----|-------|
| Idle | **~577 MB** (590,528 KB) | 4 workers, no connections |
| C10k | **~658 MB** (673,688 KB) | 10,000 concurrent connections |
| C100k | **~692 MB** (708,300 KB) | 100,000 concurrent connections |

### C10k Concurrent Connection Test

Measured with `h2load` maintaining 10,000 concurrent connections.

| Item | Value |
|------|-------|
| Concurrent connections | 10,000 |
| Duration | 21.98 s |
| Max RSS | **~658 MB** (673,688 KB) |
| CPU usage | ~199% |
| User time | 36.41 s |
| System time | 7.43 s |
| Major page faults | **0** |
| Minor page faults | 170,352 |
| Voluntary context switches | 2,197,128 |
| Involuntary context switches | 293,375 |
| File system outputs | 72 |
| Exit status | **0** |

### C100k Concurrent Connection Test

Measured with `h2load` maintaining 100,000 concurrent connections.

| Item | Value |
|------|-------|
| Concurrent connections | 100,000 |
| Duration | 2:38.55 |
| Max RSS | **~692 MB** (708,300 KB) |
| CPU usage | ~91% |
| User time | 120.81 s |
| System time | 24.13 s |
| Major page faults | **0** |
| Minor page faults | 191,633 |
| Voluntary context switches | 6,371,528 |
| Involuntary context switches | 842,479 |
| File system outputs | 72 |
| Exit status | **0** |

> Note: Values measured while maintaining actual client connections over HTTP/2 (TLS 1.3).

**C10k Benchmark Highlights**
- **Memory Efficient**: RSS stays below 660 MB with 10,000 concurrent connections (~66 KB per connection)
- **Zero Disk I/O**: Major page faults 0, Swaps 0, FS inputs 0 — pure in-memory processing under load
- **High CPU Utilization**: Sustained ~199% CPU usage while remaining stable
- **Long-term Stability**: Ran continuously for 21.98 s under C10k load and exited cleanly (status 0)
- **Data Safety**: SQLite safely persisted all data on SIGINT (72 FS outputs)
