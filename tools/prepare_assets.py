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


def resolve_font_urls(css_text, base_url=None):
    """Make font URLs absolute so the CSS can be served from any path.

    Keeps the original format(...) qualifier; only relative URLs are rewritten
    against base_url."""
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
        fmt = (" " + match.group(2)) if match.group(2) else ""
        return f"url('{url}'){fmt}"

    # Match url(...) optionally followed by format(...).
    return re.sub(r"url\(([^)]+)\)\s*(format\([^)]+\))?", repl, css_text)


def inline_font_urls(css_text, base_url=None):
    """Replace WOFF2 font url(...) format(...) references with base64 data URLs.

    Used for KaTeX CSS so math pages do not trigger separate font requests.
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
        # Minor / niche language grammars not bundled in highlight.min.js
        "highlight-fortran.js":  "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/fortran.min.js",
        "highlight-cobol.js":    "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/cobol.min.js",
        "highlight-prolog.js":   "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/prolog.min.js",
        "highlight-erlang.js":   "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/erlang.min.js",
        "highlight-elixir.js":   "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/elixir.min.js",
        "highlight-elm.js":      "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/elm.min.js",
        "highlight-haskell.js":  "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/haskell.min.js",
        "highlight-ocaml.js":    "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/ocaml.min.js",
        "highlight-scheme.js":   "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/scheme.min.js",
        "highlight-lisp.js":     "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/lisp.min.js",
        "highlight-clojure.js":  "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/clojure.min.js",
        "highlight-vhdl.js":     "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/vhdl.min.js",
        "highlight-verilog.js":  "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/verilog.min.js",
        "highlight-matlab.js":   "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/matlab.min.js",
        "highlight-r.js":        "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/r.min.js",
        "highlight-perl.js":     "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/perl.min.js",
        "highlight-groovy.js":   "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/groovy.min.js",
        "highlight-nix.js":      "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/nix.min.js",
        "highlight-zig.js":      "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/zig.min.js",
        "highlight-x86asm.js":   "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/x86asm.min.js",
        "highlight-llvm.js":     "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/llvm.min.js",
        "highlight-sparql.js":   "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/dsconfig.min.js",
        "highlight-mermaid.js":  "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/mermaid.min.js",
        "highlight-brainfuck.js":"https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/brainfuck.min.js",
        "katex.js": "https://cdn.jsdelivr.net/npm/katex@0.16.9/dist/katex.min.js",
    }


    for name, url in resources.items():
        try:
            data = fetch(url)
            save(os.path.join(INLINE_DIR, name), data)
        except urllib.error.HTTPError as exc:
            print(f"  skipping {name}: HTTP {exc.code} ({url})", file=sys.stderr)
        except urllib.error.URLError as exc:
            print(f"  skipping {name}: {exc} ({url})", file=sys.stderr)


    # KaTeX CSS references local font files; inline them so math pages do not
    # trigger separate font requests.
    katex_url = "https://cdn.jsdelivr.net/npm/katex@0.16.9/dist/katex.min.css"
    katex_css = fetch(katex_url).decode("utf-8", errors="replace")
    katex_css = inline_font_urls(katex_css, base_url=katex_url)
    save(os.path.join(INLINE_DIR, "katex.css"), katex_css)

    # Fonts are served as separate cached stylesheets rather than inlined into
    # every HTML response. The WOFF2 URLs inside the CSS still point to the CDN,
    # so the browser can cache the font files independently.
    os.makedirs("public/css", exist_ok=True)

    print("Downloading Google Fonts CSS...")
    # Body/UI Korean coverage is provided by the Pretendard Variable font below,
    # so IBM Plex Sans KR is intentionally omitted here to avoid loading two
    # large Korean families on every page.
    google_fonts_url = (
        "https://fonts.googleapis.com/css2"
        "?family=Space+Grotesk:wght@400;500;700"
        "&family=Inter:wght@400;500;600;700"
        "&family=Outfit:wght@400;500;600;700;800"
        "&family=Source+Serif+4:ital,wght@0,400;0,600;1,400"
        "&display=swap"
    )
    google_css = fetch(google_fonts_url).decode("utf-8", errors="replace")
    google_css = resolve_font_urls(google_css, base_url=google_fonts_url)
    save(os.path.join("public/css", "google-fonts.css"), google_css)

    print("Downloading Pretendard variable font CSS...")
    pretendard_url = (
        "https://cdn.jsdelivr.net/gh/orioncactus/pretendard@v1.3.9"
        "/dist/web/variable/pretendardvariable.min.css"
    )
    pretendard_css = fetch(pretendard_url).decode("utf-8", errors="replace")
    pretendard_css = resolve_font_urls(pretendard_css, base_url=pretendard_url)
    save(os.path.join("public/css", "pretendard.css"), pretendard_css)

    print("Writing D2Coding font-face CSS...")
    d2coding_url = (
        "https://cdn.jsdelivr.net/gh/Joungkyun/font-d2coding-ligature-subset@master"
        "/D2Coding-ligature-subset.woff2"
    )
    d2coding_css = (
        "@font-face {\n"
        "  font-family: 'D2 coding';\n"
        "  font-style: normal;\n"
        "  font-weight: 400;\n"
        "  font-display: swap;\n"
        f"  src: local('D2Coding'), url('{d2coding_url}') format('woff2');\n"
        "}\n"
    )
    save(os.path.join("public/css", "d2coding.css"), d2coding_css)

    print("Done.")


if __name__ == "__main__":
    main()
