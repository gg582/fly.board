# fly.board Tunables

> Runtime knobs, environment variables, and config-file settings that affect
> performance, latency, and payload size.

## Local benchmark environment

| Item | Value |
|------|-------|
| Host | `elegant` |
| OS | Linux 7.1.0-mountain-rc6+ #11 SMP PREEMPT_DYNAMIC |
| CPU | x86_64, 12 logical cores |
| RAM | 62 GiB |
| GCC | 14.2.0 (Debian 14.2.0-19) |
| h2load | nghttp2/1.64.0 |
| TLS | self-signed `server.crt` / `server.key` |

Benchmarks run against `https://127.0.0.x:8888/` via `run_all_bench.sh`.

## Environment variables

### Request/response path

| Name | Default | Unit | Description |
|------|---------|------|-------------|
| `CWIST_WORKER_THREADS` | `cpu_cores * 4`, clamped `[16, 128]` | threads | Number of HTTP worker threads spawned by CWIST. Each request handler runs on one of these threads. |
| `FLYBOARD_CORK_THRESHOLD` | `1460` | bytes | Responses whose header + body total is **≤** this value skip `TCP_CORK`. Larger responses keep CORK to coalesce header+body (and TLS records). Set to `0` to always CORK, or a larger value (e.g. `4096`) if most of your pages are small. |
| `FLYBOARD_ADVERTISE_H3` | `true` | bool | Whether `Alt-Svc` is advertised when HTTP/3 is enabled. |
| `FLYBOARD_ALT_SVC_MAX_AGE` | `300` | seconds | `ma` parameter for the `Alt-Svc` header. |
| `FLYBOARD_CACHE_MAX_MB` | *(unlimited)* | MiB | Soft cap for the in-memory page cache. |

### Asset inlining

| Name | Default | Description |
|------|---------|-------------|
| `FLYBOARD_INLINE_ALL_ASSETS` | `false` | Convenience switch. When `1`/`true`/`on`, it enables **all** inlining groups below. |
| `FLYBOARD_INLINE_SHELL` | `false` | Inline critical shell assets (font CSS, `jwt.js`, `layout.js`) into `<head>`. Reduces first-paint round trips on high-RTT links at the cost of a larger initial HTML payload. |
| `FLYBOARD_INLINE_CDN` | `false` | Inline optional CDN libraries (highlight.js, KaTeX) from `public/inline_assets/` instead of loading them from cdnjs/jsdelivr. Useful for air-gapped or very high-latency deployments. |
| `FLYBOARD_INLINE_IMAGES` | `false` | Encode logo, favicon, and background images as base64 WebP data URLs. When disabled, these are served as ordinary `/assets/img/*` URLs. |

Accepted boolean values for the flags above: `1`, `true`, `on`.

### Infrastructure / deployment

| Name | Default | Description |
|------|---------|-------------|
| `BLOG_ROOT` | current directory | Working directory searched for `public/`. |
| `BLOG_ECH_KEY` / `BLOG_ECH_DIR` | *(none)* | ECH (Encrypted ClientHello) key / config directory. |
| `NATS_URL` | *(none)* | NATS endpoint. If unset, NATS worker is not started. |
| `DEBUG` | *(unset)* | Enables debug logging when set. |

## Config-file tunables

### `blog.settings`

| Key | Default | Description |
|-----|---------|-------------|
| `title` | `Fly Me to The Moon` | Site title. |
| `subtitle` | `..And let me play among the stars` | Site subtitle. |
| `brand_footer` | `🄯 Lee Yunjin` | Footer text. |
| `root_url` | `https://localhost:8888/` | Canonical origin, used for CORS and OpenGraph. |
| `port` | `8888` | HTTP/HTTPS listen port. |
| `accent` | `#56b4e9` | Theme accent color. |
| `home_img` | *(empty)* | Home background image under `public/img/`. |
| `blog_logo` | *(empty)* | Logo image under `public/img/`. |
| `boards_img` | *(empty)* | Boards background image. |
| `files_img` | *(empty)* | Files background image. |
| `favicon` | *(empty)* | Favicon image. |
| `use_rss` | `true` | Enable RSS feed. |
| `use_tasfa` | `true` | Enable TASFA large-file upload/download. |
| `use_tls` | `true` | Enable HTTPS. |
| `use_http2` | `true` | Enable HTTP/2. |
| `use_http3` | `true` | Enable HTTP/3 (QUIC). |
| `roundness` | `0.5` | UI corner radius (0–1). |
| `max_upload_size` | `1G` | Maximum single upload size. |
| `max_total_parallel_uploads` | `8` | Global cap on concurrent TASFA uploads. |
| `max_upload_parallel_chunks` | `32` | Max concurrent chunks per upload session. |
| `max_concurrent_downloads` | `128` | Global cap on concurrent TASFA downloads. |

### `fonts.settings`

Controls font families, letter spacing, and weights. See `fonts.settings` for the full list. Key performance-related notes:

- `font_import_url`: if empty, fonts are loaded from the bundled Google Fonts CSS and served as cached local files.
- `font_body`, `font_heading`, `font_ui`, `font_code`: font stacks; shorter stacks parse slightly faster but have worse fallback coverage.

## Recommended settings

### General production (default)

```bash
# Keep the initial HTML small; fetch libraries and images on demand.
FLYBOARD_INLINE_SHELL=0
FLYBOARD_INLINE_CDN=0
FLYBOARD_INLINE_IMAGES=0
FLYBOARD_CORK_THRESHOLD=1460
```

### High-RTT / satellite / mobile edge

```bash
# Inline the shell so first paint needs only one round trip.
FLYBOARD_INLINE_SHELL=1
FLYBOARD_INLINE_CDN=0
FLYBOARD_INLINE_IMAGES=0
FLYBOARD_CORK_THRESHOLD=4096
```

### Air-gapped / offline

```bash
# No external CDN reachability.
FLYBOARD_INLINE_SHELL=1
FLYBOARD_INLINE_CDN=1
FLYBOARD_INLINE_IMAGES=1
FLYBOARD_CORK_THRESHOLD=1460
```

## Benchmark results

Run locally with the settings below and the scripts in the repository root.

```bash
export FLYBOARD_INLINE_SHELL=1
export FLYBOARD_INLINE_CDN=0
export FLYBOARD_INLINE_IMAGES=0
export FLYBOARD_CORK_THRESHOLD=1460
bash run_all_bench.sh
```

### C10K

| Metric | Value |
|--------|-------|
| Total Requests | 20000 |
| Succeeded | 20000 |
| Failed | 0 |
| Approx RPS | 2383.81 |
| Success Rate | 100.00% |
| Wall time | 17.04 s |
| Max RSS | 145928 kB |

### C100K

| Metric | Value |
|--------|-------|
| Total Requests | 200000 |
| Succeeded | 200000 |
| Failed | 0 |
| Approx RPS | 2458.23 |
| Success Rate | 100.00% |
| Wall time | 1:30.30 |
| Max RSS | 146076 kB |

### C1M

| Metric | Value |
|--------|-------|
| Total Requests | 2000000 |
| Succeeded | 722910 |
| Failed | 1277090 |
| Approx RPS | 1744.04 |
| Success Rate | 36.14% |
| Wall time | 7:02.81 |
| Max RSS | 146420 kB |
