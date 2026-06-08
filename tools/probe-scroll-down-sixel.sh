#!/bin/sh
# probe-scroll-down-sixel.sh — does the terminal SCROLL an already-painted sixel
# cleanly DOWNWARD by exactly one text cell (reverse-index at the top row),
# carrying the image raster intact?
#
# Why: gmore's pager is down-only. To turn "more" into "less" (scroll UP one
# line) the cheap scheme is: scroll the WHOLE window DOWN one cell so existing
# pixels move down intact, leaving a blank row at top, then repaint just the top
# block. That only works if a downward scroll preserves the sixel raster — the
# symmetric question to probe-scroll-sixel.sh (which proved UPWARD scroll is
# clean). Memory warns DECSTBM region scrolls corrupt sixels, so a full-window
# reverse-index might too; this probe settles it ([[mdcat-pager-stacking]]).
#
# Method: clear screen, paint a tall diagnostic image (diagonals over a gradient —
# tears show as kinks, dropped/duplicated rows as seams) a few rows down from the
# top. Then repeatedly put the cursor at the HOME row and emit Reverse Index
# (ESC M), which scrolls the whole screen down one cell, pushing the image lower
# each step, with a short delay so it's watchable.
#
# How to read it: if the image stays pixel-identical to its first frame at EVERY
# step as it descends, downward hardware scroll is clean -> the cheap up-scroll
# (window-down + top-block repaint) works. Any tear/seam/ghost/freeze/vanish at a
# step means downward scroll is dirty and up-scroll must full-repaint instead.
#
# Env: PROBE_CELL_H (px, default 14), ITER (scroll steps, default 20),
#      DELAY (seconds between steps, default 0.15).

dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$dir/probe-common.sh"
command -v magick >/dev/null 2>&1 || { echo "ImageMagick 'magick' not found" >&2; exit 1; }
[ -t 1 ] || { echo "stdout is not a TTY; run this directly in a terminal" >&2; exit 1; }

: "${PROBE_CELL_H:=14}"
: "${ITER:=20}"
: "${DELAY:=0.15}"
CELL="$PROBE_CELL_H"
ROWS=6                         # image height in cells
W=120
H=$((CELL * ROWS))
TOP=3                          # paint the image starting this many rows below home
TMP=$(mktemp -d)
SRC="$TMP/src.png"
trap 'rm -rf "$TMP"' EXIT INT TERM

# Diagnostic image: diagonals every 18px over a vertical gradient.
draw=""; k=-"$H"
while [ "$k" -lt "$H" ]; do
    draw="$draw -draw \"line 0,$k $W,$((k + H))\""
    k=$((k + 18))
done
eval magick -size "${W}x${H}" gradient:gray25-gray85 \
    -stroke black -strokewidth 1 $draw "$SRC"

printf '%s2J%sH' "$CSI" "$CSI"     # clear screen, home

# Park the cursor TOP rows down and paint the image there (DECSC/DECRC keeps the
# cursor put so the image anchors exactly at row TOP).
printf '%s%d;1H' "$CSI" $((TOP + 1))
printf '\0337'; magick "$SRC" sixel:- 2>/dev/null; printf '\0338'

# Each step: cursor HOME, then Reverse Index (ESC M) scrolls the whole screen
# down one cell — the image should descend one line, pixel-identical.
n=1
while [ "$n" -le "$ITER" ]; do
    printf '%sH' "$CSI"            # home
    printf '%sM' "$ESC"           # RI: scroll screen down one line
    printf '%s%d;1Hstep %d/%d  <- image should be %d lines LOWER, pixel-identical%sK' \
           "$CSI" 1 "$n" "$ITER" "$n" "$CSI"
    sleep "$DELAY"
    n=$((n + 1))
done

printf '%s%d;1H' "$CSI" $((TOP + ROWS + ITER + 2))
printf 'Done. Did the image descend cleanly every step? (tear/seam/ghost/freeze = dirty)\n'
