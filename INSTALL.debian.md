# Debian Install Notes

This note is for a fresh Debian machine where you want to build `fly.board` with one dependency install command and a minimal setup sequence.

## 1. Install system packages

```sh
sudo apt update
sudo apt install -y \
  build-essential \
  pkg-config \
  git \
  ca-certificates \
  curl \
  libssl-dev \
  libsqlite3-dev \
  libcjson-dev \
  libmagic-dev \
  ffmpeg \
  zlib1g-dev \
  libnghttp2-dev \
  libnghttp3-dev \
  libngtcp2-dev \
  libngtcp2-crypto-ossl-dev
```

## 2. Clone the repository and initialize bundled dependencies

```sh
git clone https://github.com/gg582/fly.board.git
cd fly.board
git submodule update --init --recursive
```

`third_party/multipart-parser-c` is now vendored directly, so the important submodules are `third_party/file` and `third_party/libttak`.

## 3. Install CWIST

`fly.board` expects `libcwist` and CWIST headers under `/usr/local` by default.

```sh
git clone https://github.com/religiya-serdtsa/cwist.git
cd cwist
make
sudo make install
cd ..
```

If you installed CWIST somewhere else, build `fly.board` with a custom prefix:

```sh
make CWIST_PREFIX=/your/cwist/prefix
```

## 4. Build `fly.board`

```sh
cd fly.board
make -j"$(nproc)"
```

The top-level `Makefile` will:

- build `third_party/libttak/lib/libttak.a`
- clone and build `third_party/md4c` if it is missing
- link against the Debian-provided OpenSSL, SQLite, cJSON, zlib, libmagic, and optional HTTP/3 libraries

## 5. First run

Generate local keys and start the server:

```sh
./keygen.sh
./fly_board
```

Default HTTPS port is read from `blog.settings` and is `9443` unless you changed it.

## Common failure points

- `cannot find -lcwist`: CWIST is not installed in `/usr/local`, or `CWIST_PREFIX` is wrong.
- `No rule to make target 'third_party/libttak/lib/libttak.a'`: run `git submodule update --init --recursive`.
- `Package ... has no installation candidate`: your Debian release may ship different HTTP/3 package names. In that case, install the matching `nghttp2/nghttp3/ngtcp2` development packages for your release, or build without those optional libraries if your CWIST build does not require them.
