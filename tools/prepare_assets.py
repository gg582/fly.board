#!/usr/bin/env python3
"""Prepare inline-ready assets for fly.board.

Downloads external CSS/JS/font resources and rewrites font URLs as base64
 data URLs so the C server can embed everything directly into HTML.
"""
import base64
import os
import re
import sys
import urllib.error
import urllib.request

INLINE_DIR = "public/inline_assets"
USER_AGENT = (
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
)


def fetch(url, headers=None, timeout=60):
    req_headers = {"User-Agent": USER_AGENT}
    if headers:
        req_headers.update(headers)
    req = urllib.request.Request(url, headers=req_headers)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def encode_data_url(data, mime):
    b64 = base64.b64encode(data).decode("ascii")
    return f"data:{mime};base64,{b64}"


def inline_font_urls(css_text, base_url=None):
    """Replace WOFF2 font url(...) format(...) references with base64 data URLs.

    Non-WOFF2 fallbacks (woff, ttf, eot) are dropped because modern browsers
    support WOFF2 and keeping every format multiplies the inline payload.
    """
    from urllib.parse import urljoin

    def repl(match):
        raw_url = match.group(1).strip("'\"")
        if raw_url.startswith("data:"):
            return match.group(0)
        url = raw_url
        if base_url and not url.startswith(("http:", "https:")):
            url = urljoin(base_url, url)
        if not url.startswith(("http:", "https:")):
            return match.group(0)
        lower = url.lower()
        is_woff2 = lower.endswith(".woff2") or ".woff2?" in lower
        if not is_woff2:
            # Drop the entire url(...) format(...) fragment.
            return ""
        try:
            font_data = fetch(url)
            fmt = match.group(2) or "format('woff2')"
            return f"url({encode_data_url(font_data, 'font/woff2')}) {fmt}"
        except urllib.error.URLError as exc:
            print(f"  warning: failed to inline font {url}: {exc}", file=sys.stderr)
            return match.group(0)

    # Match url(...) optionally followed by format(...).
    return re.sub(r"url\(([^)]+)\)\s*(format\([^)]+\))?", repl, css_text)


def save(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if isinstance(data, str):
        with open(path, "w", encoding="utf-8") as f:
            f.write(data)
    else:
        with open(path, "wb") as f:
            f.write(data)
    print(f"  wrote {path}")


def main():
    os.makedirs(INLINE_DIR, exist_ok=True)
    print("Downloading static JS/CSS...")

    resources = {
        "highlight-light.css": "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/github.min.css",
        "highlight-dark.css": "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/github-dark.min.css",
        "highlight.js": "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/highlight.min.js",
        "highlight-fortran.js": "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/fortran.min.js",
        "katex.js": "https://cdn.jsdelivr.net/npm/katex@0.16.9/dist/katex.min.js",
    }

    for name, url in resources.items():
        data = fetch(url)
        save(os.path.join(INLINE_DIR, name), data)

    # KaTeX CSS references local font files; inline them so math pages do not
    # trigger separate font requests.
    katex_url = "https://cdn.jsdelivr.net/npm/katex@0.16.9/dist/katex.min.css"
    katex_css = fetch(katex_url).decode("utf-8", errors="replace")
    katex_css = inline_font_urls(katex_css, base_url=katex_url)
    save(os.path.join(INLINE_DIR, "katex.css"), katex_css)

    print("Downloading Google Fonts and inlining WOFF2 files...")
    # Body/UI Korean coverage is provided by the inlined Pretendard Variable
    # font below, so IBM Plex Sans KR is intentionally omitted here to avoid
    # loading two large Korean families on every page.
    google_fonts_url = (
        "https://fonts.googleapis.com/css2"
        "?family=Space+Grotesk:wght@400;500;700"
        "&family=Inter:wght@400;500;600;700"
        "&family=Source+Serif+4:ital,wght@0,400;0,600;1,400"
        "&display=swap"
    )
    google_css = fetch(google_fonts_url).decode("utf-8", errors="replace")
    google_css = inline_font_urls(google_css)
    save(os.path.join(INLINE_DIR, "google-fonts.css"), google_css)

    print("Downloading Pretendard variable font CSS and inlining WOFF2 files...")
    pretendard_url = (
        "https://cdn.jsdelivr.net/gh/orioncactus/pretendard@v1.3.9"
        "/dist/web/variable/pretendardvariable.min.css"
    )
    pretendard_css = fetch(pretendard_url).decode("utf-8", errors="replace")
    pretendard_css = inline_font_urls(pretendard_css, base_url=pretendard_url)
    save(os.path.join(INLINE_DIR, "pretendard.css"), pretendard_css)

    print("Downloading D2Coding ligature-subset font and inlining...")
    d2coding_url = (
        "https://cdn.jsdelivr.net/gh/Joungkyun/font-d2coding-ligature-subset@master"
        "/D2Coding-ligature-subset.woff2"
    )
    d2coding_data = fetch(d2coding_url)
    d2coding_b64 = encode_data_url(d2coding_data, "font/woff2")
    d2coding_css = (
        "@font-face {\n"
        "  font-family: 'D2 coding';\n"
        "  font-style: normal;\n"
        "  font-weight: 400;\n"
        "  font-display: swap;\n"
        f"  src: local('D2Coding'), url({d2coding_b64}) format('woff2');\n"
        "}\n"
    )
    save(os.path.join(INLINE_DIR, "d2coding.css"), d2coding_css)

    print("Done.")


if __name__ == "__main__":
    main()
