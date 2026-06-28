#!/bin/sh
# probe-kitty-aspect.sh — does c=/r= preserve aspect ratio, or stretch?
#
# Follow-up to probe-kitty-cell-footprint.sh, which proved c=/r= keeps an image
# inside its reserved columns but left the knight looking slightly squished.
#
# The Kitty display command sizes an image into a cell box. The question this
# probe answers on a real terminal:
#   - With BOTH c= and r=, does the terminal STRETCH the image to fill c*r
#     exactly (distorting a square), or fit-with-aspect inside it?
#   - With ONLY c= (no r=), does the terminal derive the row count from the
#     image's pixel aspect ratio and the cell shape (the un-distorted result)?
#
# Test images are a perfect blue CIRCLE and a red SQUARE, each 200x200px, so any
# horizontal/vertical distortion is obvious. All are width-bound to 10 columns.
#
# Cases (square shown first, then circle, for each strategy):
#   1. c=10 only           — expected: stays square / round (correct)
#   2. c=10,r=5            — r guessed at half of c (1:2 cell ratio guess)
#   3. c=10,r=10           — deliberately wrong r: should look stretched TALL
#                            if the terminal honours r exactly (stretch), or
#                            stay square if it fits-with-aspect (ignores excess r)
#
# How to read it: screenshot. For each case say whether the SQUARE looks square
# and the CIRCLE round, or distorted (and which way). Report terminal + SSH?.
# This tells mdcat whether to emit c= alone (preferred, no aspect math) or
# c=,r= with a correctly computed r.

dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$dir/probe-common.sh"
[ -t 1 ] || { echo "stdout is not a TTY; run this directly in a terminal" >&2; exit 1; }
command -v timg >/dev/null 2>&1 || { echo "timg not found in PATH" >&2; exit 1; }

SQ="${PROBE_SQUARE:-/tmp/probe-square.png}"
CI="${PROBE_CIRCLE:-/tmp/probe-circle.png}"
[ -f "$SQ" ] || { echo "missing $SQ (generate the 200x200 test PNGs first)" >&2; exit 1; }
[ -f "$CI" ] || { echo "missing $CI" >&2; exit 1; }

COLS=10
RULER="[$(printf '%*s' $((COLS-2)) '' | tr ' ' '.')]"

# Paint $1 (png) width-bound to COLS, with extra controls $2 appended (e.g.
# ",c=10" or ",c=10,r=5"). Empty $2 = today's behaviour for reference.
paint() {
    timg -pk -g"${COLS}x1000" "$1" 2>/dev/null \
      | perl -0777 -pe "s/(\\x1b_G[^;]*)/\$1$2/"
    printf '\r\n'
}

banner "0. Context (include this in your screenshot)"
printf 'TERM=[%s] TERM_PROGRAM=[%s] SSH_CONNECTION=[%s]\n' \
       "${TERM:-}" "${TERM_PROGRAM:-}" "${SSH_CONNECTION:-}"

banner "1. c=${COLS} ONLY — expect SQUARE square, CIRCLE round (no distortion)"
printf '%s  (square)\n' "$RULER"; paint "$SQ" ",c=${COLS}"
printf '%s  (circle)\n' "$RULER"; paint "$CI" ",c=${COLS}"

banner "2. c=${COLS},r=5 — r guessed at c/2; is it still square/round?"
printf '%s  (square)\n' "$RULER"; paint "$SQ" ",c=${COLS},r=5"
printf '%s  (circle)\n' "$RULER"; paint "$CI" ",c=${COLS},r=5"

banner "3. c=${COLS},r=10 — deliberately TOO MANY rows; stretched tall, or ignored?"
printf '%s  (square)\n' "$RULER"; paint "$SQ" ",c=${COLS},r=10"
printf '%s  (circle)\n' "$RULER"; paint "$CI" ",c=${COLS},r=10"

banner "DONE"
echo "Report per case: square still square? circle still round? if not, which way"
echo "is it distorted? + terminal name + SSH?. Goal: pick c= alone vs c=,r=."
