# Installing fly.board

fly.board is a single self-contained C binary (`fly_board`) plus three mail
helper binaries. It serves HTTPS/HTTP2/HTTP3 on one port (TCP+UDP), stores
everything in SQLite files under `data/`, and keeps uploaded files on disk
under `public/`.

## Dependencies

| Dependency | Used for | Detection |
|---|---|---|
| **CWIST v3.9** | HTTP server core: TLS 1.3, HTTP/2, HTTP/3 (QUIC), BoringSSL, event-driven C1M reactor | `CWIST_ROOT` (source tree) or `CWIST_PREFIX` (installed prefix); see below |
| gcc or clang, GNU make | build | `CC` / make |
| pkg-config | locates system libraries | required at build time |
| OpenSSL 3.x | Argon2id KDF, AES-GCM backup sealing, SHA-512 | `pkg-config openssl`, fallback `-lssl -lcrypto` |
| libcurl | SMTP outbound, Mailjet API, FlyWire replica feed, S3 | `pkg-config libcurl`, fallback `-lcurl` |
| brotli (`libbrotlienc`, `libbrotlicommon`, `libbrotlidec`) | response compression, backup-independent TASFA crypto tests | `pkg-config`, fallback `-lbrotli*` |
| zstd | response compression, backup archive compression | `pkg-config libzstd`, fallback `-lzstd` |
| libwebp + libwebpmux | image thumbnailing/previews | `pkg-config`; falls back to `-lwebp -lwebpmux` and defines `HAVE_WEBP` |
| libnghttp2 | HTTP/2 | `pkg-config libnghttp2` |
| libmagic | upload MIME sniffing (`third_party/file`, built as a static lib by the Makefile; needs `autoreconf`) | vendored |
| SQLite3 | all data | bundled with CWIST / system |
| python3 | `tools/prepare_assets.py` at build time; test suites | build-time only |
| ffmpeg (optional, runtime) | TASFA video thumbnails/previews | found on `PATH` at runtime |

## Build

```sh
git clone https://github.com/gg582/fly.board.git
cd fly.board
git submodule update --init --recursive   # third_party/file, third_party/libttak

make -j2 fly_board mail-tools
```

`make` first builds the vendored static libraries (md4c, libmagic), runs
`tools/prepare_assets.py`, then links `fly_board` and the three mail tools
(`mail-verify`, `mail-import`, `mail-alias-build`).

### Locating CWIST

The Makefile resolves CWIST in this order (first match wins):

1. `CWIST_ROOT` — defaults to `../cwist` (a cwist checkout next to this
   repository, already built: it must contain `libcwist.a`).
2. `CWIST_PREFIX` — defaults to `/usr/local`; expects
   `$CWIST_PREFIX/lib/libcwist.a` and `$CWIST_PREFIX/include/cwist`.
3. Homebrew/Linuxbrew — `brew` is resolved from `PATH` and the standard
   install locations (`/home/linuxbrew/.linuxbrew`, `~/.linuxbrew`,
   `/opt/homebrew`, `/usr/local`), then `<brew prefix>/opt/cwist` or a
   `lib/libcwist.a`-carrying prefix is used.

An explicit `CWIST_ROOT` or `CWIST_PREFIX` from the command line or the
environment always wins:

```sh
make -j2 CWIST_PREFIX=/opt/cwist fly_board mail-tools
```

If the installed `libcwist.a` was built with GCC LTO from a different GCC
major, the Makefile automatically switches to a matching `gcc-<major>` when
one is installed; an explicit `CC=` always wins.

## Per-distro setup

### Debian / Ubuntu

```sh
sudo apt update
sudo apt install -y build-essential pkg-config git ca-certificates curl \
  python3 autoconf automake libtool \
  libssl-dev libcurl4-openssl-dev libbrotli-dev libzstd-dev \
  libwebp-dev libnghttp2-dev zlib1g-dev ffmpeg
```

See [INSTALL.debian.md](../INSTALL.debian.md) for the full fresh-machine
sequence including CWIST installation.

### Fedora / RHEL

```sh
sudo dnf install -y gcc make pkg-config git ca-certificates curl \
  python3 autoconf automake libtool \
  openssl-devel libcurl-devel brotli-devel libzstd-devel \
  libwebp-devel libnghttp2-devel zlib-devel ffmpeg
```

### macOS (Homebrew)

```sh
brew install pkg-config openssl curl brotli zstd webp nghttp2 ffmpeg
make -j2 fly_board mail-tools   # CWIST from the c4punks/cwist tap is auto-detected
```

Homebrew lives under `/opt/homebrew` (Apple Silicon) or `/usr/local` (Intel);
both are probed automatically. Note: `-march=native` is in `OPT_FLAGS`, which
Apple clang ignores harmlessly.

### FreeBSD

`Makefile` defines `CWIST_OS_BSD` on FreeBSD. Install the same library set
with `pkg` (`openssl`, `curl`, `brotli`, `zstd`, `webp`, `nghttp2`,
`ffmpeg`) and build with gmake.

## First run

```sh
./keygen.sh        # server.crt / server.key (self-signed, ECDSA P-256; KEY_TYPE=rsa for RSA-4096)
make setup         # creates data/
printf 'admin\nchange-me-now\n' > admin.settings   # line 1: username, line 2: password
./fly_board        # generates blog.settings, fonts.settings, s3.settings,
                   # robots.settings, upload.settings, flywire.settings; then serves
```

Edit `blog.settings` (at minimum `root_url`, `port`, `language`) and restart.
The admin account is created automatically on startup. See
[configuration.md](configuration.md) for every key.

## Running as a service

See the systemd unit in [README.md](../README.md#systemd-unit). The binary
must start in the site root (the directory holding `public/` and `data/`) or
have `BLOG_ROOT` set; otherwise it exits with "Public assets not found".
If `use_tls=true` and `server.crt`/`server.key` are missing it exits with
"HTTPS init failed; run ./keygen.sh first".

## Troubleshooting

- **`libcwist.a: No such file or directory` / CWIST headers not found** —
  the Makefile could not find CWIST. Point it explicitly:
  `make CWIST_ROOT=/path/to/cwist` (source tree with `libcwist.a` already
  built) or `make CWIST_PREFIX=/usr/local` (installed layout).
- **Old hardcoded Linuxbrew paths error** — checkouts from before the
  Homebrew/Linuxbrew fallback probed fixed paths
  (`/home/linuxbrew/.linuxbrew/...`) and failed on machines without
  Linuxbrew with "No such file or directory" errors mentioning those paths.
  The current Makefile probes multiple locations and honors explicit
  overrides. Fix: update the checkout, or override explicitly with
  `make CWIST_PREFIX=<prefix>` so the fallback block is skipped entirely.
- **LTO version mismatch** (`bytecode stream ... generated with LTO version
  X instead of the expected Y`) — the installed `libcwist.a` was built by a
  different GCC major. Install the matching `gcc-<major>` package (the
  Makefile picks it up automatically) or build CWIST with your local GCC.
- **`autoreconf: command not found`** (libmagic build) — install
  `autoconf automake libtool` (Debian) or `autoconf automake libtool`
  (Fedora).
- **Port already in use** — change `port` in `blog.settings`; HTTP/3 binds
  the same port over UDP.
- **HTTPS init failed** — run `./keygen.sh` or install a real certificate as
  `server.crt` / `server.key` in the site root.
