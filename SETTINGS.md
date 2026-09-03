# Settings Files

fly_board reads plain `key=value` files from its working directory at startup
(`engine_settings_load()` in `src/engine/settings.c`). Lines without `=` are
ignored; unknown keys are ignored. Restart the service after editing.

## blog.settings

Main site configuration (`blog_config_load()` in `src/config/config.c`).
If the file is missing, it is regenerated with the defaults below.

| Key | Type | Default | Effect |
| --- | --- | --- | --- |
| `title` | string | `CWIST Docker Blog` | Site title in header/`<title>` |
| `subtitle` | string | `Explore boards and read stories.` | Hero subtitle on the home page |
| `brand_footer` | string | `Built with CWIST C Framework` | Footer line |
| `accent` | string (hex color) | `#3b82f6` | Theme accent color |
| `port` | int | `8443` | Listen port; values outside 1–65535 fall back to the default |
| `home_img`, `home_img_dark` | filename | empty | Hero image for light/dark mode |
| `blog_logo`, `blog_logo_dark` | filename | empty | Logo for light/dark mode |
| `boards_img`, `boards_img_dark` | filename | empty | Boards page image, light/dark |
| `files_img`, `files_img_dark` | filename | empty | Files page image, light/dark |
| `favicon` | filename | empty | Site favicon |
| `bg_full_light`, `bg_full_dark` | filename | empty | Full-page background, light/dark |
| `invert_logo` | bool (`true`/`1`) | `false` | Invert the logo for the missing color mode |
| `bg_invert_color` | comma list | empty | Image targets (e.g. `home,boards,files,bg_full`) color-inverted for the mode that has no dedicated image |
| `bg_invert_algo` | `luminv` \| `oklch` | `luminv` | Inversion algorithm; unknown values fall back to `luminv` |
| `root_url` | string (URL) | empty | Canonical base URL (sitemap, RSS) |
| `use_tasfa` | bool | `true` | TASFA resumable upload/download protocol |
| `use_rss` | bool | `false` | Serve the RSS feed |
| `use_tls` | bool | `true` | HTTPS (needs certs); `false` serves plain HTTP |
| `use_http2` | bool | `true` | Enable HTTP/2 |
| `use_http3` | bool | `true` | Enable HTTP/3 (QUIC/UDP on the same port, requires TLS) |
| `roundness` | float 0.0–1.0 | `0.0` | UI corner rounding (clamped to range) |
| `max_upload_size` | size (`512K`, `1G`, …) | `1G` | Per-upload size cap |
| `max_total_parallel_uploads` | int 1–512 | `8` | Concurrent uploads across the server |
| `max_upload_parallel_chunks` | int 1–64 | `32` | Parallel chunks per upload |
| `max_concurrent_downloads` | int 1–512 | `128` | Concurrent downloads |
| `use_special_modes` | comma list | empty | Enabled seasonal/special themes (e.g. `ocean,forest`) |
| `vote_only` | `all` \| `authorized` \| `admin` | empty (= `all`) | Who may vote; unknown values fall back to `all` |

All image keys are filenames relative to `public/img/`; paths with `/` or
missing files are ignored with a warning.

## admin.settings

Initial admin credentials (`auth_admin_load()` in `src/auth/auth.c`).
**Not** `key=value`: line 1 is the admin username, line 2 the plaintext
password. If missing, it is regenerated as:

```
admin
fly.board
```

The password is hashed in memory at load; change it by editing the file and
restarting, or via the admin profile page.

## robots.settings

Crawler policy for `/robots.txt` and `/llms.txt` (`robots_config_load()` in
`src/config/config.c`). Optional: when missing, a fully commented template is
written and the site runs fully open.

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `robots.level` | `allow` \| `restrict` \| `block` | `allow` | `allow`: all crawlers welcome. `restrict`: disallow admin/private paths (`/admin`, `/api/`, `/login`, `/logout`, `/register`, `/profile`, `/notifications`). `block`: `Disallow: /`, no sitemap |
| `llms.level` | `allow` \| `restrict` \| `block` | `allow` | `allow`: serve `/llms.txt`, AI crawlers welcome. `restrict`: serve `/llms.txt` but disallow training crawlers (GPTBot, ClaudeBot, CCBot, Google-Extended, …) in robots.txt. `block`: `/llms.txt` returns 404 and AI crawlers are disallowed |

Unknown values fall back to `allow`.

## fonts.settings

Typography (`font_settings_load()` in `src/config/config.c`). If missing, it
is regenerated with the defaults below. Web fonts are inlined into every HTML
response, so the defaults need no external URLs.

| Key | Default |
| --- | --- |
| `font_import_url` | empty (no external `@import`) |
| `font_face_family` | `JetBrains Mono` |
| `font_face_src` | empty (no extra `@font-face` src) |
| `font_body` | `'Space Grotesk', 'IBM Plex Sans KR', 'Pretendard Variable', 'Pretendard', sans-serif` |
| `font_heading` | `'Outfit', sans-serif` |
| `font_ui` | `'Inter', 'IBM Plex Sans KR', 'Pretendard Variable', sans-serif` |
| `font_code` | `'JetBrains Mono', 'Fira Code', 'D2Coding', Consolas, Monaco, 'Courier New', monospace` |
| `font_blockquote` | `'Source Serif 4', 'IBM Plex Sans KR', serif` |
| `font_display` | `'Outfit', sans-serif` |
| `letter_spacing_{body,h1,h2,h3,h4,h5h6,topbar_title,btn,board_line_title,hero_h1,hero_p,md_h1,md_h2,md_h3,post_h1}` | CSS lengths, e.g. `-0.01em` body, `-0.05em` h1 (see generated file) |
| `font_weight_{body,h1,h2,h3,h4,h5h6,topbar_title,btn,board_line_title,hero_h1,md_h1,md_h2,md_h3,post_h1}` | numeric weights, e.g. `450` body, `800` h1 (see generated file) |

## s3.settings

Optional S3-compatible object storage for uploads (`s3_config_load()` in
`src/config/config.c`). If missing, an empty commented template is written and
uploads stay on local disk. S3 is enabled only when `endpoint`, `bucket`,
`access_key` and `secret_key` are all set.

| Key | Type | Effect |
| --- | --- | --- |
| `endpoint` | URL | S3 endpoint |
| `region` | string | Bucket region |
| `bucket` | string | Bucket name |
| `access_key` / `secret_key` | string | Credentials |
| `prefix` | string | Key prefix inside the bucket |
| `use_path_style` | bool | Path-style (vs virtual-host) URLs |
| `mode` | `mirror` \| `offload` | `mirror` keeps a local copy and also stores in S3; `offload` moves files to S3. Unknown values fall back to `mirror` |
