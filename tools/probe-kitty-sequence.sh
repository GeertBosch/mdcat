#!/bin/sh
# probe-kitty-sequence.sh — does a SEQUENCE of multi-chunk Kitty images leak,
# where a single one does not?
#
# Context: in a real document mdcat emits several images one after another, each
# via the reserve-band + DECSC/DECRC dance with an INLINE a=T (transmit+display).
# Single multi-chunk images render fine in isolation (probe-kitty-multichunk-place),
# but in a document the multi-chunk ones leak their base64 (user saw this on iTerm2).
# The remaining difference is the SEQUENCE: many chunked transmissions in quick
# succession, interleaved with cursor save/restore/scroll. This probe reproduces
# that, two ways:
#
#   G. CURRENT: for each of N images, do the full dance with an INLINE a=T
#      (transmit+display) — exactly what mdcat does now.
#   H. FIX: FIRST transmit every image with a=t (no display, no cursor motion),
#      THEN for each do the dance and place with a payload-free a=p.
#
# How to read it: screenshot. Do all N images in G render, or do later ones leak
# base64 / mispaint? Do all N in H render cleanly? If G leaks and H doesn't, the
# sequence is the trigger and transmit-first-then-place is the fix. + terminal.

dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$dir/probe-common.sh"
[ -t 1 ] || { echo "stdout is not a TTY; run this directly in a terminal" >&2; exit 1; }
command -v timg >/dev/null 2>&1 || { echo "timg not found" >&2; exit 1; }
command -v perl >/dev/null 2>&1 || { echo "perl needed" >&2; exit 1; }

# Several DIFFERENT images so each is a distinct multi-chunk transmission, like a
# real document. Fall back to repeating one if some are missing.
IMGS="tests/img/joan-mitchell.jpg tests/img/sunflower.gif tests/img/example.png README.md-1.png"
ROWS=10

banner "0. Context"
printf 'TERM_PROGRAM=[%s] SSH_CONNECTION=[%s]\n' "${TERM_PROGRAM:-}" "${SSH_CONNECTION:-}"
# Show each image's chunk count (counted correctly: occurrences, not grep -c lines).
for f in $IMGS; do
    [ -f "$f" ] || continue
    n=$(timg -pk -g30x15 "$f" 2>/dev/null | tr -d '\n' | grep -o $'\x1b_G' | wc -l | tr -d ' ')
    printf '  %s: %s chunks\n' "$f" "$n"
done

# timg's chunked APC for $1 at a forced unique id $2, with timg's own cursor
# management stripped. Optionally rewrite a=T -> a=t for transmit-only ($3=t).
apc() {  # $1=path $2=id $3=action(T|t)
    timg -pk -g30x15 "$1" 2>/dev/null | perl -0777 -pe '
        s/^\x1b\[\?25l//; s/\n?\x1b\[\?25h$//;
        s/i=\d+/i='"$2"'/;
        '"$( [ "$3" = t ] && echo 's/a=T/a=t/;' )"
}

reserve_dance_begin() { # reserve ROWS, up, save
    i=0; while [ "$i" -lt "$ROWS" ]; do printf '\n'; i=$((i+1)); done
    printf '\033[%dA\0337' "$ROWS"
}
reserve_dance_end() { printf '\0338\033[%dB\r\n' "$ROWS"; }

banner "G. CURRENT: per-image reserve-dance with INLINE a=T (transmit+display)"
id=8200
for f in $IMGS; do
    [ -f "$f" ] || continue
    id=$((id+1))
    reserve_dance_begin
    apc "$f" "$id"            # a=T inline, inside the dance
    reserve_dance_end
done
printf '\n'

banner "H. FIX: transmit ALL first (a=t, no motion), then dance + place (a=p)"
id=8300
# 1) transmit every image up front, no cursor motion between them.
for f in $IMGS; do
    [ -f "$f" ] || continue
    id=$((id+1))
    apc "$f" "$id" t         # transmit-only
done
# 2) now place each in its own reserved band with a payload-free a=p.
id=8300
for f in $IMGS; do
    [ -f "$f" ] || continue
    id=$((id+1))
    reserve_dance_begin
    printf '\033_Ga=p,i=%s,q=2\033\\' "$id"
    reserve_dance_end
done
printf '\n'

banner "DONE"
echo "Report: in G, do ALL images render, or do later multi-chunk ones leak/mispaint?"
echo "In H, do all render cleanly? If G leaks and H is clean, transmit-first-then-"
echo "place is the fix (and the Phase 2 model)."
