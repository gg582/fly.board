#!/bin/bash
# Color-preserving vectorization of flat line-art into a lightweight SVG.
#
# Usage: vectorize_asset_color.sh <input.png> <output.svg> [ink-color]
#
# Dark line work is traced into a single ink color; saturated accent pixels
# (red/yellow/green/blue) are traced into separate layers that keep their own
# hue.  The result is a multi-fill SVG on a transparent background.  Unlike
# vectorize_asset.sh (full monochrome), this keeps the small color accents
# of the source illustration while staying a fraction of the raster size.
#
# Requires: ImageMagick 7 (magick), potrace.
set -euo pipefail

IN=${1:?input path required}
OUT=${2:?output path required}
INK=${3:-#2a2d34}

POTRACE=${POTRACE:-$HOME/.local/bin/potrace}
command -v "$POTRACE" >/dev/null || POTRACE=$(command -v potrace)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Accent fills sampled from the source artwork.
RED=#e30700
YELLOW=#f1c30b
GREEN=#149d63
BLUE=#0a08da

# Ink: dark, low-saturation pixels (line work, mesh, silhouettes).
magick "$IN" -alpha off \
    -fx "(max(max(r,g),b) < 0.86 && max(max(r,g),b) - min(min(r,g),b) < 0.12) ? 0 : 1" "$TMP/ink.pgm"

# Accents: saturated pixels classified by channel dominance in sRGB.
declare -A RANGE=(
    ["$RED"]="r - g > 0.15 && r - b > 0.15 && r < 0.99"
    ["$YELLOW"]="r - b > 0.3 && g - b > 0.2 && abs(r - g) < 0.2"
    ["$GREEN"]="g - r > 0.08 && g - b > 0.08"
    ["$BLUE"]="b - r > 0.15 && b - g > 0.1"
)

# Layer order: ink first, then accents on top.
ORDERED=("$TMP/ink.pgm:$INK:30")
for color in "$RED" "$YELLOW" "$GREEN" "$BLUE"; do
    magick "$IN" -alpha off \
        -fx "${RANGE[$color]} ? 0 : 1" "$TMP/$color.pgm"
    # Skip layers with almost no ink (keeps the SVG small).
    MEAN=$(magick "$TMP/$color.pgm" -format "%[fx:mean]" info:)
    if awk -v m="$MEAN" 'BEGIN{exit !(m<0.99995)}'; then
        ORDERED+=("$TMP/$color.pgm:$color:6")
    fi
done

# Trace every layer and collect recolored <g> blocks.
GLIST=""
for spec in "${ORDERED[@]}"; do
    mask=${spec%%:*}; rest=${spec#*:}; color=${rest%%:*}; turd=${rest##*:}
    "$POTRACE" -s -t "$turd" -O 0.4 -o "$TMP/layer.svg" "$mask"
    awk '/<g /{f=1} f{print} /<\/g>/{f=0}' "$TMP/layer.svg" \
        | sed "s/#000000/${color}/g; s/#000\b/${color}/g" > "$TMP/$color.group"
    GLIST="$GLIST $TMP/$color.group"
done

# First layer's preamble carries the svg root element.
"$POTRACE" -s -t 30 -O 0.4 -o "$TMP/first.svg" "$TMP/ink.pgm"
awk '/<g /{exit} {print}' "$TMP/first.svg" > "$OUT"
for g in $GLIST; do cat "$g" >> "$OUT"; done
echo '</svg>' >> "$OUT"

# Normalize pt-based root dimensions to unitless (pixel) values.
sed -i -e 's/width="\([0-9.]*\)pt"/width="\1"/g' \
       -e 's/height="\([0-9.]*\)pt"/height="\1"/g' "$OUT"

SIZE=$(stat -c%s "$OUT")
ACCENTS=""
for g in $GLIST; do
    [ "$(basename "$g")" = "$(basename "$INK").group" ] || ACCENTS="$ACCENTS $(basename "$g" .group)"
done
echo "$OUT: ${SIZE} bytes (ink ${INK}, accents:${ACCENTS:- none})"
