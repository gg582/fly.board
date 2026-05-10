# fly.board

![fly.board logo](img/logo.png)

> One of the few simple blog engines running at **8–16 MB RSS**.  
> A lightweight board-and-blog engine built on the C-based CWIST web framework, supporting HTTPS/3, Argon2id, PQC signatures, and NATS messaging.

## Features

- **Memory Efficient** – Stack+heap C implementation. Production RSS stays around **8–16 MB**.
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
- [CWIST](https://github.com/religiya-serdtsa/cwist)
- OpenSSL 3.x (Argon2id KDF, TLS 1.3, QUIC)
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

- `blog.settings` – Blog title, subtitle, footer, port
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

> For detailed benchmark methodology and full results, see [`benchmarks/README.md`](benchmarks/README.md).

### Host Environment

| Item | Value |
|------|-------|
| OS | Linux 7.0.0-mountain+ |
| Architecture | x86_64 |
| CPU | AMD Ryzen 5 5600X @ 3.70GHz (6 cores / 12 threads) |
| RAM | 64 GB |
| Disk | Samsung SSD 980 1TB (NVMe) |
| OpenSSL | 3.5.5 |
| Benchmark Tool | wrk |
| CWIST | `patches/cwist` (SIGPIPE patch applied) |

### Max Throughput (RPS)

`wrk -t4 -c400 -d30s` (TLS 1.3, no serialization)

| Endpoint | Peak RPS | Avg Latency | Notes |
|----------|----------|-------------|-------|
| `/` (Home) | **3,409.92** | 121.84ms | DB query + markdown rendering |
| `/login` | **3,948.77** | 18.03ms | Static form (cacheable) |
| `/boards` | **3,901.77** | 17.26ms | DB-driven list |

### Resource Usage (Peak Load)

| Item | Value |
|------|-------|
| CPU Usage | ~600% (on 12-thread system) |
| RAM (RSS) | ~12 MB |
| Virtual Memory (VSZ) | ~1.2 GB |

> Note: Benchmarks were run **without** request serialization (`pthread_mutex_t`).  
> `ulimit -n` is set to 20,000, allowing stable measurement up to 400 connections.
