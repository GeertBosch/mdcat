#!/bin/sh
# probe-slice-paint.sh — does a SINGLE viewport-clipped sixel (one DCS, not a
# per-row strip stack) render seamlessly, and does repainting successive slices
# at a fixed top row simulate clean scrolling?
#
# Why: probe-pager-stack proved the per-row 18px-strip stack TEARS on iTerm2
# (gaps between strips; the cursor doesn't advance vertically after a sixel).
# But the whole-image-as-one-sixel REFERENCE block rendered flawlessly. The open
# question for "paint whole sixels everywhere" is paging: a tall image must be
# shown a viewport-tall SLICE at a time, repainted as you scroll. Each slice is
# ONE sixel (encodeSixel(img, y0, y1) over a viewport-sized y-range), not a stack.
# This probe paints a sequence of such slices to confirm:
#   (a) each single slice is internally seamless (no intra-slice tearing), and
#   (b) sliding the slice's y-origin by a few px per frame scrolls smoothly.
#
# METHOD: build a tall diagnostic PNG (diagonals + gradient, same as the stack
# probe). Reserve a VIEWPORT of ROWS cells. Then, for a series of scroll offsets,
# clear the viewport and paint ONE sixel = the source slice [off, off+viewportPx)
# at the viewport's top-left. Pause between frames so the scroll is visible.
#
# HOW TO READ IT: each frame must show unbroken diagonals and a smooth gradient
# with NO horizontal seams inside the image (contrast with probe-pager-stack #2).
# Across frames the image should appear to scroll up smoothly. If every frame is
# internally seamless, single-sixel slice paging is viable on this terminal.
#
# Env: PROBE_CELL_H (px, default 16), ROWS (viewport height in cells, default 10),
#      STEP (scroll px per frame, default 7), FRAMES (default 8), DELAY (default 0.3).

dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$dir/probe-common.sh"
command -v magick >/dev/null 2>&1 || { echo "ImageMagick 'magick' not found" >&2; exit 1; }
[ -t 1 ] || { echo "stdout is not a TTY; run this directly in a terminal" >&2; exit 1; }

: "${PROBE_CELL_H:=16}"
: "${ROWS:=10}"
: "${STEP:=7}"
: "${FRAMES:=8}"
: "${DELAY:=0.3}"
CELL="$PROBE_CELL_H"
W=120
VIEWPX=$((CELL * ROWS))         # viewport height in px
H=$((VIEWPX * 2))               # source is twice the viewport, so we can scroll
TMP=$(mktemp -d)
SRC="$TMP/src.png"
trap 'rm -rf "$TMP"' EXIT INT TERM

# Diagnostic image: a diagonal every 18px over a vertical gradient (a kink at any
# seam, a brightness jump at any skipped/repeated row).
draw=""; k=-"$H"
while [ "$k" -lt "$H" ]; do
    draw="$draw -draw \"line 0,$k $W,$((k + H))\""
    k=$((k + 18))
done
eval magick -size "${W}x${H}" gradient:gray25-gray85 \
    -stroke black -strokewidth 1 $draw "$SRC"

slice_sixel() {  # $1=y  $2=h  -> native-pixel sixel of [y, y+h)
    magick "$SRC" -crop "${W}x${2}+0+${1}" +repage sixel:- 2>/dev/null
}

printf 'Painting ONE viewport-tall sixel per frame, scrolling %spx/frame.\n' "$STEP"
printf 'Each frame should be internally seamless; the image should scroll smoothly.\n'

# Reserve the viewport band (scroll-safe) and remember its top-left.
i=0; while [ "$i" -lt "$ROWS" ]; do printf '\n'; i=$((i + 1)); done
printf '%s%dA' "$CSI" "$ROWS"          # back to viewport top-left

f=0
while [ "$f" -lt "$FRAMES" ]; do
    off=$((f * STEP))
    [ $((off + VIEWPX)) -gt "$H" ] && off=$((H - VIEWPX))
    # Paint the slice as ONE sixel at the viewport top-left (DECSC/DECRC bracket).
    printf '\0337'
    slice_sixel "$off" "$VIEWPX"
    printf '\0338'
    sleep "$DELAY"
    f=$((f + 1))
done

printf '%s%dB\r' "$CSI" "$ROWS"        # park below the viewport
printf 'Done. Was every frame internally seamless, and the scroll smooth?\n'
printf '(cell height assumed %spx — override with PROBE_CELL_H)\n' "$CELL"
