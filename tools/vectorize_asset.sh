#!/bin/bash
# Single-color vectorization of a raster image into a lightweight SVG.
#
# Usage: vectorize_asset.sh <input.png> <output.svg> [ink-color] [fuzz]
#
# Every pixel that is not (near-)white becomes part of the traced silhouette;
# the result is a single fill color on a transparent background. Designed for
# the flat line-art hero/background assets in public/img/ - it is not a
# general photo vectorizer.
#
# Requires: ImageMagick (magick), potrace (https://potrace.sourceforge.net).
set -euo pipefail

IN=${1:?input path required}
OUT=${2:?output path required}
COLOR=${3:-#2a2d34}
FUZZ=${4:-12%}

POTRACE=${POTRACE:-$HOME/.local/bin/potrace}
command -v "$POTRACE" >/dev/null || POTRACE=$(command -v potrace)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

W=$(magick identify -format "%w" "$IN")
H=$(magick identify -format "%h" "$IN")

# Ink mask: near-white -> background, everything else -> black silhouette.
magick "$IN" -alpha off -fuzz "$FUZZ" -transparent white \
    -alpha extract -negate -threshold 40% "$TMP/mask.pgm"

# turdsize drops speckle noise; opttolerance smooths the bezier fit.
"$POTRACE" -s -t 30 -O 0.2 \
    -o "$TMP/raw.svg" "$TMP/mask.pgm"

# potrace emits fill:#000000 and pt-based root dimensions; recolor the ink
# and normalize width/height to unitless (pixel) values.
sed -e "s/#000000/${COLOR}/g" -e "s/#000\b/${COLOR}/g" \
    -e "s/width=\"\([0-9.]*\)pt\"/width=\"\1\"/g" \
    -e "s/height=\"\([0-9.]*\)pt\"/height=\"\1\"/g" \
    "$TMP/raw.svg" > "$OUT"

SIZE=$(stat -c%s "$OUT")
echo "$OUT: ${SIZE} bytes (${W}x${H}, ink ${COLOR})"
