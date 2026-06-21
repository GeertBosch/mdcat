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
# Search to a FAR match jumps -> RIS; a NEAR match within a page does not.
check "search far jump"  "/45${nl}q" 1
check "search near"      "/4${nl}q"  0   # "4" first matches line 4, within page 1
# Quit at the first screen: nothing moves, no RIS.
check "no move"          'q'         0

if [ "$fails" -eq 0 ]; then
    echo "gmore-repaint: OK ($n cases)"
    exit 0
fi
echo "gmore-repaint: FAIL ($fails of $((n + fails)) cases)" >&2
exit 1
