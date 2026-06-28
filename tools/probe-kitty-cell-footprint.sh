#!/bin/sh
# probe-kitty-cell-footprint.sh — does timg's -g cell count match the terminal's?
#
# Why: mdcat lays images out on a text grid (table columns). For Kitty it asks
# `timg -pk -gWxH` to size the image, then RESERVES W columns of blank in the
# table and reads the painted footprint back to confirm. The bug: when timg's
# stdout is a PIPE it has no terminal to query, so it sizes the -g box with a
# FIXED ~9px cell (measured: 33 cols -> 297px, 10 cols -> 90px = 9.0 px/col).
# The TERMINAL then lays that PNG out using ITS OWN real cell width. If the real
# cell is, say, 8px, a "33-cell" image is 297/8 ≈ 38 real cells — it OVERFLOWS
# the 33-column slot mdcat reserved and overlaps the next table cell.
#
# The fix mdcat will use: the Kitty DISPLAY command accepts c=<cols>,r=<rows>,
# which makes the TERMINAL scale the image into EXACTLY that cell box regardless
# of pixel math. This probe shows the two behaviours side by side so you can
# confirm in a real terminal which one stays inside its 10-column reservation.
#
# Layout: each test prints a 10-wide "[..........]" ruler, then on the line(s)
# below, paints the image. If the image stays within the brackets it fits its
# reservation; if it spills past the right bracket it would overlap a neighbour.
#
# How to read it: screenshot. Report for BOTH tests whether the image's right
# edge is inside or past the `]`. Expectation: test A (no c=/r=) spills past;
# test B (c=10,r=5) is flush inside. Include terminal name + SSH? from line 0.

dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$dir/probe-common.sh"
[ -t 1 ] || { echo "stdout is not a TTY; run this directly in a terminal" >&2; exit 1; }
command -v timg >/dev/null 2>&1 || { echo "timg not found in PATH" >&2; exit 1; }
[ -f "$PROBE_PNG" ] || { echo "PROBE_PNG not found: $PROBE_PNG" >&2; exit 1; }

COLS=10                   # the "column" we pretend the table reserved
RULER="[$(printf '%*s' $((COLS-2)) '' | tr ' ' '.')]"   # [........] = 10 wide

banner "0. Context (include this in your screenshot)"
printf 'TERM=[%s] TERM_PROGRAM=[%s] SSH_CONNECTION=[%s]\n' \
       "${TERM:-}" "${TERM_PROGRAM:-}" "${SSH_CONNECTION:-}"

banner "A. timg -g${COLS}x (no c=/r=) — terminal sizes from pixels (mdcat TODAY)"
echo "Reservation is the 10 dots below; image should NOT spill past the ] :"
printf '%s\n' "$RULER"
# Exactly what mdcat emits today: timg's -g box, a=T transmit+display, no c=/r=.
timg -pk -g"${COLS}x1000" "$PROBE_PNG" 2>/dev/null
printf '\r\n'

banner "B. same image + c=${COLS},r=5 on the display — terminal scales to the cell box (FIX)"
echo "Reservation is the 10 dots below; image SHOULD sit flush inside the ] :"
printf '%s\n' "$RULER"
# Append c=COLS,r=5 to the FIRST controls segment so the terminal scales the
# image into exactly COLS columns. We rewrite a=T's first "...;" controls run.
timg -pk -g"${COLS}x1000" "$PROBE_PNG" 2>/dev/null \
  | perl -0777 -pe "s/(\\x1b_G[^;]*)/\$1,c=${COLS},r=5/"
printf '\r\n'

banner "DONE"
echo "Report: A spills past ]? B flush inside ]? + terminal name + SSH?."
echo "If B is flush and A spills, the c=/r= cell-footprint fix is validated:"
echo "mdcat should emit c=cols,r=rows matching the table reservation."
