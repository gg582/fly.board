---
title: "Hello CWIST Blog"
date: "2026-05-03"
summary: "A first post showing Markdown images, GitHub-style tables, and CWIST-rendered layout."
---

# Hello CWIST Blog

This page is written as Markdown, rendered with **md4c**, and wrapped by CWIST's HTML builder on each request.

![CWIST blog mark](/assets/images/cwist-blog.svg)

## Table Rendering

| Feature | Source | Rendered by |
| --- | --- | --- |
| Markdown body | `posts/*.md` | md4c |
| Page shell | C code | CWIST SSR HTML builder |
| Theme | `/theme.css` | CWIST CSS builder |
| Static assets | `public` | `cwist_app_static` |

## Multimedia

HTML media tags can be embedded directly in Markdown when md4c renders raw HTML.

<video controls preload="metadata" poster="/assets/images/cwist-blog.svg">
  <source src="/assets/media/sample.mp4" type="video/mp4">
  Your browser can play a local MP4 when you add one at public/media/sample.mp4.
</video>

<audio controls>
  <source src="/assets/media/sample.ogg" type="audio/ogg">
  Your browser can play a local OGG file when you add one at public/media/sample.ogg.
</audio>

```c
cwist_app_use_https3(app, true);
cwist_app_use_https(app, "server.crt", "server.key");
```
