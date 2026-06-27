#!/bin/sh
# probe-kitty-clip.sh — can we display only a CROPPED band of a transmitted image?
#
# Why: gmore pages a scrollback that contains images. When an image is partially
# scrolled off the top or bottom of the window, gmore must paint ONLY its visible
# band. ADR 0002 plans to do this WITHOUT decoding the PNG: transmit the image
# once (a=t, image id i=), then issue placement commands (a=p) that select a
# SOURCE-PIXEL crop rectangle (x,y,w,h) scaled into a CELL footprint (c,r). This
# probe validates that mechanic on a real terminal before we build gmore on it.
#
# The test image is the 64x64 chess knight: its HEAD is the top half, its BASE is
# the bottom half, so a horizontal crop is visually unambiguous.
#
#   1. TRANSMIT ONCE (a=t, i=7777): no image should appear yet (transmit-only).
#   2. BOTTOM-CLIP (image's bottom is off-screen → show its TOP band): crop
#      y=0,h=32 (top half) → you should see ONLY THE KNIGHT'S HEAD.
#   3. TOP-CLIP (image's top is off-screen → show its BOTTOM band): crop
#      y=32,h=32 (bottom half) → you should see ONLY THE KNIGHT'S BASE/LEGS.
#   4. FULL reference: x=0,y=0,w=64,h=64 → the whole knight.
#
# How to read it: screenshot. Report whether #2 is the head only, #3 the base
# only, #4 the whole knight. If so, Kitty source-crop works and gmore can paint
# partial images with no PNG decode. + terminal name + SSH?.

dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$dir/probe-common.sh"
[ -t 1 ] || { echo "stdout is not a TTY; run this directly in a terminal" >&2; exit 1; }
command -v timg >/dev/null 2>&1 || { echo "timg not found in PATH" >&2; exit 1; }
[ -f "$PROBE_PNG" ] || { echo "PROBE_PNG not found: $PROBE_PNG" >&2; exit 1; }

ID=7777
PW=64; PH=64            # the knight PNG is 64x64 (verified)
APC_END="${ESC}\\"

banner "0. Context (include this in your screenshot)"
printf 'TERM=[%s] TERM_PROGRAM=[%s] SSH_CONNECTION=[%s]\n' \
       "${TERM:-}" "${TERM_PROGRAM:-}" "${SSH_CONNECTION:-}"

# Transmit the image ONCE: timg's a=T (transmit+display) rewritten to a=t
# (transmit-only) with a fixed id, byte-exact (perl -0777). No image is drawn.
banner "1. Transmit once (a=t, i=$ID) — NOTHING should appear here"
timg -pk -g24x24 "$PROBE_PNG" 2>/dev/null | perl -0777 -pe "s/a=T/a=t/; s/i=\\d+/i=$ID/"
printf '\n'

# Emit a placement: display a crop of the transmitted image. Controls only, no
# payload. x,y,w,h = source-pixel rectangle; c,r = cell box it scales into.
# q=2 SUPPRESSES the terminal's OK/error reply — without it, Kitty answers each
# graphics command with `ESC_G...;OK ESC\`, which echoes onscreen as stray escapes.
# (timg's own transmit already sets q=2, which is why the transmit step is clean.)
place() {  # $1=x $2=y $3=w $4=h $5=c $6=r
    printf '%s_Ga=p,i=%s,q=2,x=%s,y=%s,w=%s,h=%s,c=%s,r=%s%s' \
           "$ESC" "$ID" "$1" "$2" "$3" "$4" "$5" "$6" "$APC_END"
}

banner "2. BOTTOM-CLIP: show the TOP band (y=0,h=$((PH/2))) — expect HEAD ONLY"
echo "(This is what gmore paints when an image's bottom is below the window fold.)"
place 0 0 "$PW" "$((PH/2))" 10 2
printf '\n'

banner "3. TOP-CLIP: show the BOTTOM band (y=$((PH/2)),h=$((PH/2))) — expect BASE ONLY"
echo "(This is what gmore paints when an image's top is above the window fold.)"
place 0 "$((PH/2))" "$PW" "$((PH/2))" 10 2
printf '\n'

banner "4. FULL reference (whole image) — expect the WHOLE knight"
place 0 0 "$PW" "$PH" 10 4
printf '\n'

banner "DONE"
echo "Report: #2 = head only?, #3 = base only?, #4 = whole knight?, terminal name."
echo "If all three match, Kitty source-crop works and gmore can paint partial images"
echo "with no PNG decode (ADR 0002 scroll-clip design validated)."
