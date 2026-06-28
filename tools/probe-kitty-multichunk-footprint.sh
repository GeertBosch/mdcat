#!/bin/sh
# probe-kitty-multichunk-footprint.sh — does c=/r= work on a MULTI-CHUNK a=T?
#
# Why: probe-kitty-aspect.sh proved c=/r= sizes a SINGLE-chunk image correctly,
# and mdcat now injects c=/r= on the first chunk of every Kitty image. But the
# README image table — whose images are large, MANY-chunk PNGs — STILL overflows
# its columns on iTerm even though the bytes carry the right c=/r= on the first
# chunk (verified). So the open question is whether the terminal honours display
# controls (c=,r=) placed on the FIRST chunk of a multi-chunk a=T transmission,
# or only on the LAST chunk, or only via a separate a=p placement.
#
# This probe paints the SAME ~18-chunk JPEG three ways against a 25-column ruler:
#   A. c=/r= on the FIRST chunk (a=T,...,m=1,c=25,r=..) — exactly what mdcat does.
#   B. c=/r= on the LAST chunk (m=0,c=25,r=..) — controls with the final chunk.
#   C. transmit-only (a=t) then a SEPARATE placement (a=p,c=25,r=..) — the
#      mechanism probe-kitty-clip.sh already validated for cropping.
#
# How to read it: screenshot. For each of A/B/C say whether the image's right
# edge sits inside the ] (25 cols) or spills past it. Whichever stays inside is
# the form mdcat must emit. Report terminal name + SSH?.

dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$dir/probe-common.sh"
[ -t 1 ] || { echo "stdout is not a TTY; run this directly in a terminal" >&2; exit 1; }
command -v timg >/dev/null 2>&1 || { echo "timg not found in PATH" >&2; exit 1; }

IMG="${PROBE_MULTI:-tests/img/joan-mitchell.jpg}"
[ -f "$IMG" ] || { echo "missing $IMG" >&2; exit 1; }

COLS=25; ROWS=12
RULER="[$(printf '%*s' $((COLS-2)) '' | tr ' ' '.')]"

banner "0. Context (include this in your screenshot)"
printf 'TERM=[%s] TERM_PROGRAM=[%s] SSH_CONNECTION=[%s]\n' \
       "${TERM:-}" "${TERM_PROGRAM:-}" "${SSH_CONNECTION:-}"
N=$(timg -pk -g"${COLS}x1000" "$IMG" 2>/dev/null | tr -cd '\033' | wc -c)
printf 'image transmits in roughly %s chunks (each ESC_G is one chunk)\n' "$N"

banner "A. c=,r= on the FIRST chunk (what mdcat emits today) — inside the ] ?"
printf '%s\n' "$RULER"
timg -pk -g"${COLS}x1000" "$IMG" 2>/dev/null \
  | perl -0777 -pe "s/(\\x1b_Ga=T[^;]*)/\$1,c=${COLS},r=${ROWS}/"
printf '\r\n'

banner "B. c=,r= on the LAST chunk (m=0) — inside the ] ?"
printf '%s\n' "$RULER"
# Append c=,r= to the LAST controls run, which is the one containing m=0.
timg -pk -g"${COLS}x1000" "$IMG" 2>/dev/null \
  | perl -0777 -pe "s/(\\x1b_G([^;]*m=0)[^;]*)/\$1,c=${COLS},r=${ROWS}/"
printf '\r\n'

banner "C. transmit-only a=t + separate a=p placement (c=,r=) — inside the ] ?"
printf '%s\n' "$RULER"
ID=8123
# Transmit only (a=T -> a=t), fixed id, no display.
timg -pk -g"${COLS}x1000" "$IMG" 2>/dev/null \
  | perl -0777 -pe "s/a=T/a=t/; s/i=\\d+/i=$ID/"
# Now place it, scaling into the COLSxROWS cell box (no payload).
printf '%s_Ga=p,i=%s,q=2,c=%s,r=%s%s\\' "$ESC" "$ID" "$COLS" "$ROWS" "$ESC"
printf '\r\n'

banner "DONE"
echo "Report A/B/C: which stay INSIDE the 25-col ] ? + terminal + SSH?."
echo "mdcat currently does A; if only B or C fits, mdcat's emit form must change."
