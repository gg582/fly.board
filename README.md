# CWIST Simple Blog

Jekyll-style Markdown blog example served with CWIST SSR, HTTPS, HTTP/3, TLS 1.3, optional ECH, md4c Markdown rendering, dynamic CSS, images, tables, and multimedia tags.

## Build

```sh
make
./keygen.sh
./simple_blog
```

## Dependencies

- [CWIST](https://github.com/religiya-serdtsa/cwist)

The Makefile clones `md4c` into `third_party/md4c`, builds the md4c C sources into a local static archive, and links it into the example.

Open:

```text
https://localhost:8443
```

HTTP/3 uses UDP port `8443` with the same TLS context. Browser support depends on the local OpenSSL/ngtcp2/nghttp3 stack and client behavior.

## ECH

ECH is optional because it requires an OpenSSL build with server-side ECH support and a valid ECH key/config file. To request ECH:

```sh
BLOG_ECH_KEY=ech/server.ech ./simple_blog
```

or:

```sh
BLOG_ECH_DIR=ech ./simple_blog
```

If the linked OpenSSL does not support server ECH, the app logs a warning and continues with HTTPS3/TLS 1.3.

## Content

Markdown posts live in `posts/*.md` with small YAML-style front matter:

```md
---
title: "Post title"
date: "2026-05-03"
summary: "Index card text."
---
```

Static assets are served from `public` at `/assets`.
