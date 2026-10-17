#!/usr/bin/env python3
"""Convert a flat-color rectangular mosaic (raster) into a compact SVG of
<rect> elements.  Designed for geometric wallpapers whose pixels form
axis-aligned tiles of a few flat tones; line art should use potrace instead
(tools/vectorize_asset_color.sh).

Usage: mosaic_to_svg.py <input.pgm> <output.svg> [max-levels]

The input must be an 8-bit PGM (P5) — produce it with, e.g.:
    magick in.png -alpha off -colorspace Gray +dither -colors N out.pgm

Pixels are quantized to at most `max-levels` (default 8) gray tones; runs of
equal tone are merged vertically into maximal rectangles.  The output keeps
the source pixel dimensions (1 unit = 1px).
"""
import sys


def read_pgm(path):
    with open(path, "rb") as f:
        data = f.read()
    # Minimal PGM (P5) parser: magic, width, height, maxval, then raw bytes.
    tokens = []
    i = 0
    while len(tokens) < 4:
        while i < len(data) and data[i:i+1].isspace():
            i += 1
        if data[i:i+1] == b"#":
            while i < len(data) and data[i:i+1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j+1].isspace():
            j += 1
        tokens.append(data[i:j])
        i = j
    i += 1  # single whitespace after maxval
    magic, w, h, maxval = tokens[0], int(tokens[1]), int(tokens[2]), int(tokens[3])
    if magic != b"P5" or maxval != 255:
        raise SystemExit("need an 8-bit binary PGM (P5, maxval 255)")
    px = data[i:i + w * h]
    if len(px) != w * h:
        raise SystemExit("truncated pixel data")
    return w, h, px


def main():
    src, dst = sys.argv[1], sys.argv[2]
    max_levels = int(sys.argv[3]) if len(sys.argv) > 3 else 8

    w, h, px = read_pgm(src)

    # Quantize the tone range into `max_levels` buckets.
    lo, hi = min(px), max(px)
    if hi == lo:
        raise SystemExit("single-tone image; nothing to trace")

    def bucket(v):
        return int((v - lo) * (max_levels - 1) / (hi - lo) + 0.5)

    # Snap each bucket to the median source tone it contains so the SVG
    # colors match the artwork instead of the quantized ladder.
    sums = {}
    counts = {}
    for v in px:
        b = bucket(v)
        sums[b] = sums.get(b, 0) + v
        counts[b] = counts.get(b, 0) + 1
    tone = {b: int(sums[b] / counts[b] + 0.5) for b in sums}
    grid = [bucket(v) for v in px]

    # Row-run length encoding with greedy vertical merge: an open rectangle
    # extends downward only while the run below matches its span exactly.
    open_rects = []   # [x0, x1, y0, color]
    rects = []        # (x, y, w, h, color)
    for y in range(h):
        row = grid[y * w:(y + 1) * w]
        runs = []
        x = 0
        while x < w:
            c = row[x]
            x1 = x + 1
            while x1 < w and row[x1] == c:
                x1 += 1
            runs.append((x, x1, c))
            x = x1
        matched = [False] * len(open_rects)
        new_open = []
        for (x0, x1, c) in runs:
            found = False
            for idx, r in enumerate(open_rects):
                if not matched[idx] and r[3] == c and r[0] == x0 and r[1] == x1:
                    matched[idx] = True
                    found = True
                    break
            if not found:
                new_open.append([x0, x1, y, c])
        for idx, r in enumerate(open_rects):
            if matched[idx]:
                new_open.append(r)
            else:
                rects.append((r[0], r[2], r[1] - r[0], y - r[2], r[3]))
        open_rects = new_open
    for r in open_rects:
        rects.append((r[0], r[2], r[1] - r[0], h - r[2], r[3]))

    rects.sort(key=lambda r: (r[4], r[1], r[0]))
    parts = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
        f'viewBox="0 0 {w} {h}" preserveAspectRatio="xMidYMid slice">',
        '<title>background</title>',
    ]
    cur = None
    for (x, y, rw, rh, c) in rects:
        if c != cur:
            if cur is not None:
                parts.append("</g>")
            t = tone[c]
            parts.append(f'<g fill="#{t:02x}{t:02x}{t:02x}">')
            cur = c
        parts.append(f'<rect x="{x}" y="{y}" width="{rw}" height="{rh}"/>')
    parts.append("</g>")
    parts.append("</svg>")
    out = "\n".join(parts) + "\n"
    with open(dst, "w") as f:
        f.write(out)
    print(f"{dst}: {len(out)} bytes, {len(rects)} rects, "
          f"{len(sums)} tones ({w}x{h})")


if __name__ == "__main__":
    main()
