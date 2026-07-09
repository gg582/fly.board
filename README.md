# fly.board

![fly.board logo](img/logo.png)

> One of the few simple blog engines running at **~82 MB RSS** at idle (with 4 workers; maintains **68-120 MB** on a real production server with a single worker), and **~146 MB** under C10k (10,000 concurrent connections).  
> A lightweight board-and-blog engine built on the C-based CWIST web framework, supporting HTTPS/3, Argon2id, PQC signatures, and NATS messaging.

## Features

- **Memory Efficient** – Stack+heap C implementation. **~82 MB RSS** at idle; **~146 MB** max RSS under 10,000 concurrent connections (C10k).
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

## Performance Benchmark

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

| State | RSS | Notes |
|-------|-----|-------|
| Idle | **~82 MB** (83,708 KB) | 4 workers, no connections |
| C10k | **~146 MB** (145,928 KB) | 10,000 concurrent connections |
| C100k | **~146 MB** (146,076 KB) | 100,000 concurrent connections |
| C1m | **~146 MB** (146,420 KB) | 1,000,000 concurrent connections |

### C10k Concurrent Connection Test

Measured with `h2load` maintaining 10,000 concurrent connections.

| Item | Value |
|------|-------|
| Concurrent connections | 10,000 |
| Duration | 17.04 s |
| Max RSS | **~146 MB** (145,928 KB) |
| CPU usage | ~480% |
| User time | 73.54 s |
| System time | 8.25 s |
| Major page faults | 51 |
| Minor page faults | 267,239 |
| Voluntary context switches | 1,959,611 |
| Involuntary context switches | 17,100 |
| File system outputs | 10,600 |
| Total requests | 20000 |
| Total succeeded | 20000 |
| Total failed | 0 |
| Approx total RPS | **2383.81** |
| Success rate | **100.00%** |
| Exit status | **0** |

### C100k Concurrent Connection Test

Measured with `h2load` maintaining 100,000 concurrent connections.

| Item | Value |
|------|-------|
| Concurrent connections | 100,000 |
| Duration | 1:30.30 |
| Max RSS | **~146 MB** (146,076 KB) |
| CPU usage | ~824% |
| User time | 700.38 s |
| System time | 44.12 s |
| Major page faults | 0 |
| Minor page faults | 472,679 |
| Voluntary context switches | 3,908,475 |
| Involuntary context switches | 165,739 |
| File system outputs | 101,672 |
| Total requests | 200000 |
| Total succeeded | 200000 |
| Total failed | 0 |
| Approx total RPS | **2458.23** |
| Success rate | **100.00%** |
| Exit status | **0** |

### C1m Concurrent Connection Test

Measured with `h2load` maintaining 1,000,000 concurrent connections.

| Item | Value |
|------|-------|
| Concurrent connections | 1,000,000 |
| Duration | 7:02.81 |
| Max RSS | **~146 MB** (146,420 KB) |
| CPU usage | ~654% |
| User time | 2553.88 s |
| System time | 211.70 s |
| Major page faults | 3 |
| Minor page faults | 895,633 |
| Voluntary context switches | 24,007,690 |
| Involuntary context switches | 931,088 |
| File system outputs | 366,248 |
| Total requests | 2000000 |
| Total succeeded | 722910 |
| Total failed | 1277090 |
| Approx total RPS | **1744.04** |
| Success rate | **36.14%** |
| Exit status | **0** |

> Note: Values measured while maintaining actual client connections over HTTP/2 (TLS 1.3).

**C10k Benchmark Highlights**
- **Memory Efficient**: RSS stays around 146 MB with 10,000 concurrent connections (~15 KB per connection)
- **Disk I/O**: Major page faults 51, Swaps 0 — SQLite and cache activity generate ~10 MB of FS output
- **High CPU Utilization**: Sustained ~480% CPU usage while remaining stable
- **Long-term Stability**: Ran continuously for 17.04 s under C10k load and exited cleanly (status 0)
- **Data Safety**: SQLite safely persisted all data on SIGINT (10,600 FS outputs)
