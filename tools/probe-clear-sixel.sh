#!/bin/sh
# probe-clear-sixel.sh — does ESC[2J (clear screen) also erase already-painted
# sixel graphics, or only the text cells (leaving the raster on screen)?
#
# Why: gmore's up-scroll full-repaints the window by clearing once with ESC[2J
# then redrawing. If ESC[2J erases text but NOT sixels, old images persist and the
# repaint draws new ones on top -> the top-overlap garbage we saw. This probe
# settles which clear strategy up-scroll must use ([[mdcat-pager-stacking]]).
#
# Method: paint a sixel, pause, issue ESC[2J + home, pause, then print a marker.
# Tries three clears in sequence so you can see which (if any) erases the image:
#   1. ESC[2J            (erase entire screen)
#   2. ESC[2J ESC[3J     (also erase scrollback)
#   3. overwrite: home + print blank lines (text cells over the image area)
#
# How to read it: after each step the screen says which clear was just applied and
# whether the chess image is still visible. The first clear that makes the image
# VANISH is the one up-scroll should use. If only step 3 (overwrite) clears it,
# the repaint must write blanks over the image cells, not rely on ESC[2J.
#
# Env: PROBE_PNG (default tests/chess-piece.png).

dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$dir/probe-common.sh"
require_tty

pause() { printf '%s' "$1"; read -r _ < /dev/tty; }

printf '%s2J%sH' "$CSI" "$CSI"
printf 'Painting a sixel now. Press Enter after each step.\n\n'
printf '\0337'; emit_sixel "$PROBE_PNG" "" "" ; printf '\0338'
printf '%s12;1H' "$CSI"        # move below a ~10-row image
pause '[1] Image painted. Press Enter to try ESC[2J ...'

printf '%s2J%sH' "$CSI" "$CSI"
printf 'After ESC[2J: is the chess image GONE or still visible?\n'
pause '[2] Press Enter to also try ESC[3J (erase scrollback) ...'

printf '%s2J%s3J%sH' "$CSI" "$CSI" "$CSI"
printf 'After ESC[2J + ESC[3J: image gone now?\n'
pause '[3] Press Enter to try OVERWRITING with blank lines ...'

printf '%sH' "$CSI"
i=0; while [ "$i" -lt 14 ]; do printf '%s2K\n' "$CSI"; i=$((i + 1)); done
printf '%sHAfter overwriting cells with ESC[2K blanks: image gone now?\n' "$CSI"
pause 'Done. Which step erased the image? Press Enter to exit.'
printf '%s2J%sH' "$CSI" "$CSI"
