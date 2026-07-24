#!/bin/zsh
# svg2png.sh <in.svg> [out.png] — render an SVG to PNG for viewing.
set -e
in="$1"
out="${2:-${in%.svg}.png}"
rsvg-convert -z "${SVG_ZOOM:-2}" "$in" -o "$out"
echo "$out"
