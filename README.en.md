# fly.board

![fly.board logo](img/logo.png)

> One of the few simple blog engines running at **~82 MB RSS** at idle (with 4 workers; maintains **90-200 MB** on a real production server with a single worker), and **~117 MB** under C10k (10,000 concurrent connections).  
> A lightweight board-and-blog engine built on the C-based CWIST web framework, supporting HTTPS/3, Argon2id, PQC signatures, and NATS messaging.
>
> **Fairly small, greater usability.**  
> TASFA prioritizes completion rate and real transfer throughput over peak RPS. Large chunks, aggressive parallel windows, HTP validation, bitmap sessions, and adaptive renegotiation use high-bandwidth servers quickly while still keeping uploads from breaking on degraded networks.
> PQC signatures absorb the ML-DSA-65 overhead to give post body tamper-evidence in a quantum-computing era.  
> Simultaneous HTTP/1.1, HTTP/2, and HTTP/3 support abandons single-protocol peak performance in exchange for universal reach across firewalls, proxies, and legacy devices.

## Features

- **Memory Efficient** – Stack+heap C implementation. **~82 MB RSS** at idle; **~117 MB** max RSS under 10,000 concurrent connections (C10k).
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
| OS | Linux 7.0.0-mountain+ |
| Architecture | x86_64 |
| CPU | AMD Ryzen 5 5600X @ 3.70GHz (6 cores / 12 threads) |
| RAM | 64 GB |
| Disk | Samsung SSD 980 1TB (NVMe) |
| OpenSSL | 3.5.6 |
| Benchmark Tool | wrk, h2load |
| CWIST | `patches/cwist` |

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
| C10k | **~117 MB** (120,184 KB) | 10,000 concurrent connections |
| C100k | **~174 MB** (178,056 KB) | 100,000 concurrent connections |
| C1m | **~216 MB** (220,888 KB) | 1,000,000 concurrent connections |

### C10k Concurrent Connection Test

Measured with `h2load` maintaining 10,000 concurrent connections.

| Item | Value |
|------|-------|
| Concurrent connections | 10,000 |
| Duration | 21.72 s |
| Max RSS | **~117 MB** (120,184 KB) |
| CPU usage | ~200% |
| User time | 35.19 s |
| System time | 8.39 s |
| Major page faults | **1** |
| Minor page faults | 57,581 |
| Voluntary context switches | 2,235,918 |
| Involuntary context switches | 405,099 |
| File system outputs | 8 |
| Total requests | 20000 |
| Total succeeded | 20000 |
| Total failed | 0 |
| Approx total RPS | **1291.35** |
| Success rate | **100.00%** |
| Exit status | **0** |

### C100k Concurrent Connection Test

Measured with `h2load` maintaining 100,000 concurrent connections.

| Item | Value |
|------|-------|
| Concurrent connections | 100,000 |
| Duration | 2:46.70 |
| Max RSS | **~174 MB** (178,056 KB) |
| CPU usage | ~88% |
| User time | 118.41 s |
| System time | 28.31 s |
| Major page faults | **0** |
| Minor page faults | 150,669 |
| Voluntary context switches | 6,984,249 |
| Involuntary context switches | 1,081,830 |
| File system outputs | 8 |
| Total requests | 200000 |
| Total succeeded | 200000 |
| Total failed | 0 |
| Approx total RPS | **1244.21** |
| Success rate | **100.00%** |
| Exit status | **0** |

### C1m Concurrent Connection Test

Measured with `h2load` maintaining 1,000,000 concurrent connections.

| Item | Value |
|------|-------|
| Concurrent connections | 1,000,000 |
| Duration | 10:13.39 |
| Max RSS | **~216 MB** (220,888 KB) |
| CPU usage | ~55% |
| User time | 201.98 s |
| System time | 136.96 s |
| Major page faults | **1** |
| Minor page faults | 220,927 |
| Voluntary context switches | 38,926,712 |
| Involuntary context switches | 4,460,022 |
| File system outputs | 8 |
| Total requests | 2000000 |
| Total succeeded | 607048 |
| Total failed | 1392952 |
| Approx total RPS | **1000.39** |
| Success rate | **30.35%** |
| Exit status | **0** |

> Note: Values measured while maintaining actual client connections over HTTP/2 (TLS 1.3).

**C10k Benchmark Highlights**
- **Memory Efficient**: RSS stays below 120 MB with 10,000 concurrent connections (~12 KB per connection)
- **Zero Disk I/O**: Major page faults 1, Swaps 0, FS inputs 0 — pure in-memory processing under load
- **High CPU Utilization**: Sustained ~200% CPU usage while remaining stable
- **Long-term Stability**: Ran continuously for 21.72 s under C10k load and exited cleanly (status 0)
- **Data Safety**: SQLite safely persisted all data on SIGINT (8 FS outputs)
