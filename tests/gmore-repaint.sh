#!/bin/sh
# Property: gmore must FULL-REPAINT (emit RIS, ESC c) on any view move that is not
# a contiguous forward scroll, so stale on-screen content — especially sixel
# images — is cleared. A contiguous forward move (delta <= one page) instead does
# the scrollback-preserving incremental append and emits NO RIS.
#
# This guards the reported bug where jumping to a search hit or to a line via g/G
# left the previous screen's sixel images on display, because the forward-append
# path was used for a non-overlapping (gap) forward jump. See paintMove() in
# gmore_core.h.
#
# We can't render real sixels off-tty here, but the RIS escape is the observable
# proxy: exactly the moves that must clear the screen emit it. Counting ESC c per
# scripted session pins the paint strategy deterministically.
#
# IMPORTANT: gmore only RENDERS when it thinks stdout is a terminal; GMORE_KEYS
# forces the interactive render path off-tty (see tests/gmore-links.sh).
#
# Geometry: LINES=6 -> pageH=5, over a 50-line file. maxTop = 45. A full page is
# 5 rows, so delta>5 is a "jump" (must RIS) and delta<=5 is contiguous (no RIS).
#
# Usage: tests/gmore-repaint.sh [gmore-binary]
# Exit status: 0 if all cases pass, 1 otherwise.

set -u

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
gmore=${1:-$root/gmore}

if [ ! -x "$gmore" ]; then
    echo "gmore-repaint: $gmore is not an executable; run 'make' first" >&2
    exit 2
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
seq 1 50 > "$tmp/in"
fails=0
n=0

# Count RIS (ESC c) occurrences in the rendered byte stream.
count_ris() { grep -aoE "$(printf '\033c')" | wc -l | tr -d ' '; }

# A literal newline (terminates a search pattern); command substitution strips
# trailing newlines, so embed one directly.
nl='
'

# check LABEL KEYS EXPECTED_RIS — render with KEYS scripted, assert the RIS count.
check() {
    label=$1; keys=$2; want=$3
    got=$(GMORE_KEYS="$keys" LINES=6 COLUMNS=20 "$gmore" "$tmp/in" 2>"$tmp/err" | count_ris)
    if [ "$got" = "$want" ]; then
        n=$((n + 1))
    else
        echo "gmore-repaint: FAIL [$label] keys='$keys'" >&2
        echo "  expected RIS: $want" >&2
        echo "  actual RIS:   $got" >&2
        [ -s "$tmp/err" ] && { echo "  stderr:" >&2; sed 's/^/    /' "$tmp/err" >&2; }
        fails=$((fails + 1))
    fi
}

# Contiguous forward moves preserve scrollback — no RIS.
check "line down"        'jq'        0
check "partial page 5j"  '5jq'       0   # delta == pageH: still abuts, no gap
check "full page f"      'fq'        0
check "two pages ff"     'ffq'       0
# Forward JUMPS leave a gap — must full-repaint (clear stale images).
check "count jump 6j"    '6jq'       1   # delta > pageH
check "count jump 30j"   '30jq'      1
check "G to bottom"      'Gq'        1
check "g count line 30"  '30gq'      1
# G then g (bottom then top) is two non-contiguous moves -> two RIS.
check "G then g"         'Ggq'       2
# Backward moves always full-repaint.
check "back page"        'fbq'       1
# Search ALWAYS full-repaints (RIS), near or far: the match highlight must be
# applied across the whole visible window, which the incremental append can't do.
check "search far jump"  "/45${nl}q" 1
check "search near"      "/4${nl}q"  1   # "4" first matches line 4, within page 1
# Quit at the first screen: nothing moves, no RIS.
check "no move"          'q'         0

# --- Kitty image re-transmit on repaint ---------------------------------------
# RIS (ESC c) makes the terminal FORGET every transmitted Kitty image, so each
# full repaint must RE-TRANSMIT the image data (a=t chunk) before re-placing it
# (a=p crop). Before the fix, gmore tracked transmissions in a set that survived
# RIS, so a repaint emitted only an a=p referencing an id the terminal no longer
# had — the image vanished on every backward/jump/search move while plain forward
# scrolling (no RIS) kept it. A re-transmit happens on each RIS repaint in which
# the image is actually VISIBLE (an off-screen image needs neither transmit nor
# placement), so the telling case is returning to a view that shows the image.
b64="iVBORw0KGgoAAAANSUhEUgAAABAAAAAgCAIAAACU62+bAAAAHUlEQVR4nGP4z8BAEiJN9aiGUQ2jGkY1jGoYohoAP8T+EE3PqSIAAAAASUVORK5CYII="
printf '\033_Gi=7,a=T,f=100,m=0;%s\033\\\n' "$b64" > "$tmp/kin"
seq 1 50 >> "$tmp/kin"
count_tx() { grep -aoE 'a=t,' | wc -l | tr -d ' '; }

# kcheck LABEL KEYS EXPECTED_TRANSMITS — render with a Kitty image, assert the
# number of a=t (re)transmissions.
kcheck() {
    label=$1; keys=$2; want=$3
    got=$(GMORE_KEYS="$keys" LINES=6 COLUMNS=20 GMORE_CELLW=8 GMORE_CELLH=16 \
          "$gmore" "$tmp/kin" 2>"$tmp/err" | count_tx)
    if [ "$got" = "$want" ]; then
        n=$((n + 1))
    else
        echo "gmore-repaint: FAIL [$label] keys='$keys'" >&2
        echo "  expected a=t transmits: $want" >&2
        echo "  actual a=t transmits:   $got" >&2
        [ -s "$tmp/err" ] && { echo "  stderr:" >&2; sed 's/^/    /' "$tmp/err" >&2; }
        fails=$((fails + 1))
    fi
}

kcheck "kitty no move"      'q'    1   # initial transmit only, no RIS
kcheck "kitty fwd advance"  'jq'   1   # forward append: no RIS, no re-transmit
kcheck "kitty jump off G"   'Gq'   1   # RIS, but image is off-screen: nothing to send
kcheck "kitty back to top"  'Ggq'  2   # g returns to the image's view: re-transmit
kcheck "kitty page back"    'fbq'  2   # backward RIS repaint with image visible

if [ "$fails" -eq 0 ]; then
    echo "gmore-repaint: OK ($n cases)"
    exit 0
fi
echo "gmore-repaint: FAIL ($fails of $((n + fails)) cases)" >&2
exit 1
