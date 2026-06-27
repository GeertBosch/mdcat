#!/bin/sh
# probe-kitty-multichunk-place.sh — does a MULTI-CHUNK Kitty image survive the
# cursor-bracketing mdcat uses to place images (DECSC/DECRC + reserve-band scroll)?
#
# Why: mdcat places each image with emitImageParagraph — it reserves N blank rows
# (printing newlines, which may scroll), moves the cursor up N (CSI A), saves the
# cursor (ESC7 / DECSC), emits the image, restores (ESC8 / DECRC), steps down N
# (CSI B). For a SINGLE-chunk Kitty image this works. But a large image is sent as
# MANY chunks (ESC_G a=T,...,m=1 ; <4096 b64> ESC\  then ESC_G q=2,m=1 ; ... ESC\
# ... ESC_G q=2,m=0 ; ... ESC\). Symptom in mdcat on iTerm2: 1-chunk images render,
# but multi-chunk images LEAK their base64 as text and mispaint. The emitted bytes
# are well-formed (chunks contiguous, no text between them), so the suspect is the
# TERMINAL's handling of a chunked transmission that is wrapped in DECSC/DECRC
# and/or preceded by a scroll. This probe isolates which wrapping breaks it.
#
# Four placements of the SAME multi-chunk image (timg makes it; we rewrite to a
# unique id each time). Watch which ones render the picture vs leak base64 text:
#   A. RAW: chunks emitted back-to-back at the cursor, no bracketing.
#   B. DECSC/DECRC: ESC7 <chunks> ESC8 (save/restore around the whole image).
#   C. RESERVE+UP+SAVE...RESTORE+DOWN: the full emitImageParagraph dance.
#   D. RAW but with a cursor move (CSI s/u) instead of ESC7/8, to tell DECSC apart.
#
# How to read it: screenshot. For each of A/B/C/D, did the IMAGE appear, or did
# base64 text leak? The first one that leaks identifies the breaking wrapper.
# + terminal + SSH.

dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$dir/probe-common.sh"
[ -t 1 ] || { echo "stdout is not a TTY; run this directly in a terminal" >&2; exit 1; }
command -v timg >/dev/null 2>&1 || { echo "timg not found" >&2; exit 1; }
command -v perl >/dev/null 2>&1 || { echo "perl needed" >&2; exit 1; }

# A reliably MULTI-CHUNK image: the large README PNG, or fall back to example.png.
BIG=README.md-1.png; [ -f "$BIG" ] || BIG=tests/img/example.png
ROWS=8   # reserved cell rows for the reserve-band test

banner "0. Context"
printf 'TERM_PROGRAM=[%s] SSH_CONNECTION=[%s]  image=%s\n' \
       "${TERM_PROGRAM:-}" "${SSH_CONNECTION:-}" "$BIG"

# Emit timg's Kitty APC for $BIG with a forced unique id, and STRIP timg's own
# cursor management (leading ESC[?25l, trailing \n + ESC[?25h) so only the bare
# chunked APC remains — exactly what mdcat does. Prints to stdout.
emit_image() {  # $1 = unique id
    timg -pk -g16x8 "$BIG" 2>/dev/null | perl -0777 -pe '
        s/^\x1b\[\?25l//;            # strip leading hide-cursor
        s/\n?\x1b\[\?25h$//;         # strip trailing newline + show-cursor
        s/i=\d+/i='"$1"'/;           # force unique id (first occurrence)
    '
}

# Count APC chunks correctly. NOTE: `grep -c` counts matching LINES, not matches,
# so it wrongly reports 1 for a whole-stream multi-chunk APC; count occurrences.
chunks_in() {
    timg -pk -g16x8 "$BIG" 2>/dev/null \
        | tr -d '\n' | grep -o $'\x1b_G' | wc -l | tr -d ' '
}
banner "1. (info) chunk count of the test image"
echo "the test image transmits in $(chunks_in) Kitty chunks (need >1 for this probe)"

banner "A. RAW chunks at the cursor (no bracketing) — baseline"
emit_image 7101
printf '\n\n'

banner "B. DECSC/DECRC bracketing: ESC7 <image> ESC8"
printf '\0337'; emit_image 7102; printf '\0338'
printf '\n\n'

banner "C. Full emitImageParagraph dance (reserve $ROWS rows, up, ESC7, image, ESC8, down)"
i=0; while [ "$i" -lt "$ROWS" ]; do printf '\n'; i=$((i+1)); done
printf '\033[%dA' "$ROWS"
printf '\0337'; emit_image 7103; printf '\0338'
printf '\033[%dB\r' "$ROWS"
printf '\n\n'

banner "D. SCO save/restore (CSI s / CSI u) instead of DECSC/DECRC"
printf '\033[s'; emit_image 7104; printf '\033[u'
printf '\n\n'

# The cases above use a SMALL reserve. The real mdcat path reserves the image's
# FULL (tall) height, so the reserve-band newlines SCROLL the screen, and the
# chunked transmission then happens AFTER a cursor-up into that scrolled region.
# E reproduces that; F tests the proposed fix.
TALL=20

banner "E. TALL reserve ($TALL rows) — transmit+display INSIDE the dance (current mdcat)"
echo "(this is the suspected-broken case: scroll, then up, then 30-chunk a=T)"
i=0; while [ "$i" -lt "$TALL" ]; do printf '\n'; i=$((i+1)); done
printf '\033[%dA' "$TALL"
printf '\0337'; emit_image 7105; printf '\0338'
printf '\033[%dB\r' "$TALL"
printf '\n\n'

banner "F. PROPOSED FIX: TRANSMIT (a=t) FIRST, then reserve-dance + PLACE (a=p)"
echo "(transmit the chunks with NO cursor motion, then a tiny placement in the band)"
# Transmit-only: rewrite a=T -> a=t so the chunks define the image without drawing.
timg -pk -g16x8 "$BIG" 2>/dev/null | perl -0777 -pe '
    s/^\x1b\[\?25l//; s/\n?\x1b\[\?25h$//; s/a=T/a=t/; s/i=\d+/i=7106/;'
# Now the reserve-band dance, placing the already-transmitted image with a=p (no payload).
i=0; while [ "$i" -lt "$TALL" ]; do printf '\n'; i=$((i+1)); done
printf '\033[%dA' "$TALL"
printf '\0337'
printf '\033_Ga=p,i=7106,q=2\033\\'      # placement only — no base64, can't leak
printf '\0338'
printf '\033[%dB\r' "$TALL"
printf '\n\n'

banner "DONE"
echo "Report which of A/B/C/D/E/F show the IMAGE vs leak BASE64 TEXT."
echo "Key: does E (tall reserve + inline transmit) leak where A-D did not? And does"
echo "F (transmit-first, then placement) render cleanly? If so, that's the fix."
