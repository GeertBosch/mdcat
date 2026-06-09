#!/bin/sh
# probe-clear-scrolled-sixel.sh — once a TALL sixel has been relocated by the
# terminal's scroll mechanism, which clear sequence actually erases its raster?
#
# Why this exists (and why the earlier probe-clear-sixel.sh, dropped in 12acc0e,
# was WRONG): that probe painted a SHORT image at a FIXED position and concluded
# "ESC[2J erases sixels in VSCode". True for that case — but gmore's bug appears
# only with the real shape: gmore's down-scroll (advance) paints an image strip on
# the BOTTOM line then prints a newline that SCROLLS the whole screen up, including
# the sixel raster (which on VSCode scrolls in lockstep with text). The images at a
# page bottom are TALL, so most of their raster sits OFF-SCREEN. When up-scroll
# (retreat) then issues ESC[2J + home + repaint, ESC[2J does NOT erase that
# scrolled/off-screen raster — the images GHOST at the top (verified live with the
# captured mdcat stream). So the question isn't "does ESC[2J clear a sixel" but
# "does it clear a SCROLLED tall sixel" — and the answer is no.
#
# Method: paint a tall sixel near the bottom (tail off-screen), scroll it up with
# newlines (exactly like repeated 'j'), then try each clear in turn and repaint a
# screen of markers. Observe which clear leaves NO image ghost anywhere.
#   1. ESC[2J            (what gmore used to do — leaves a ghost here)
#   2. ESC[2J + ESC[3J   (also erase scrollback)
#   3. ESC[!p + ESC[2J   (soft reset, DECSTR)
#   4. ESC c             (RIS, full reset — the one gmore now uses)
#
# How to read it: the FIRST clear whose marker screen shows NO image pixels (no
# ghost at the top or anywhere) is the one up-scroll must use. On VSCode only RIS
# (step 4) fully erases the scrolled raster; the rest leave partial/whole ghosts.
#
# Env: PROBE_PNG (default tests/chess-piece.png); PROBE_TALL (px height, def 300);
#      SCROLL (lines to scroll, def 6).

dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$dir/probe-common.sh"
require_tty

: "${PROBE_TALL:=300}"
: "${SCROLL:=6}"
pause() { printf '%s' "$1"; read -r _ < /dev/tty; }
rows=$(stty size 2>/dev/null | awk '{print $1}'); [ -n "$rows" ] || rows=36
markers() { i=1; while [ "$i" -le "$1" ]; do printf 'marker row %02d\n' "$i"; i=$((i + 1)); done; }

# A tall image so its lower half runs off the screen bottom (mirrors gmore's
# page-bottom images, whose raster mostly lives off-screen).
TMP=$(mktemp -d); TALL="$TMP/tall.png"
trap 'rm -rf "$TMP"' EXIT INT TERM
if command -v magick >/dev/null 2>&1; then
    magick "$PROBE_PNG" -resize "64x${PROBE_TALL}!" "$TALL"
else
    TALL="$PROBE_PNG"
fi

paint_scrolled() {   # fill screen, paint tall sixel at bottom, scroll it up
    printf '%s2J%sH' "$CSI" "$CSI"
    i=1; while [ "$i" -lt "$rows" ]; do printf 'filler %02d\n' "$i"; i=$((i + 1)); done
    printf '\0337'; emit_sixel "$TALL" "" "" ; printf '\0338'
    i=0; while [ "$i" -lt "$SCROLL" ]; do printf '\n'; i=$((i + 1)); done
}

try_clear() {   # $1 = label, $2... = printf format(s) for the clear bytes
    label=$1; shift
    paint_scrolled
    pause "[setup] Tall sixel scrolled up $SCROLL lines. Press Enter to apply: $label ..."
    eval "$@"                                   # emit the clear sequence
    markers $((rows - 1))
    printf '%s1;1H>>> %s: any image GHOST (esp. at TOP)? <<<' "$CSI" "$label"
    pause ' Press Enter for next clear ...'
}

try_clear "1) ESC[2J"          'printf "%s2J%sH" "$CSI" "$CSI"'
try_clear "2) ESC[2J+ESC[3J"   'printf "%s2J%s3J%sH" "$CSI" "$CSI" "$CSI"'
try_clear "3) ESC[!p+ESC[2J"   'printf "%s!p%s2J%sH" "$CSI" "$CSI" "$CSI"'
try_clear "4) ESC c (RIS)"     'printf "%sc" "$ESC"'

printf '%sc' "$ESC"
printf 'Done. Which clear left NO ghost? (expected: only ESC c / RIS)\n'
