#!/bin/sh
# gmore's navigation state machine (Nav), checked deterministically via
# `--nav-trace`: it replays the GMORE_KEYS script through Nav alone — no tty, no
# painting — and prints the final view window as one plain-text line:
#     top=R bottom=B total=T pct=P% (more|END)
# So every motion command is asserted without parsing sixels or RIS repaints.
#
# Geometry is fixed at LINES=24 (pageH=23), COLUMNS=80 over a 50-line file, so
# maxTop = 50 - 23 = 27 and a page step is 23 rows.
#
# Usage: tests/gmore-nav.sh [gmore-binary]
# Exit status: 0 if all cases pass, 1 otherwise (printing diffs).

set -u

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
gmore=${1:-$root/gmore}

if [ ! -x "$gmore" ]; then
    echo "gmore-nav: $gmore is not an executable; run 'make' first" >&2
    exit 2
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
seq 1 50 > "$tmp/in"
fails=0
n=0

# check LABEL KEYS EXPECTED — replay KEYS, assert the trace line equals EXPECTED.
check() {
    label=$1; keys=$2; expected=$3
    act=$(GMORE_KEYS="$keys" LINES=24 COLUMNS=80 "$gmore" --nav-trace "$tmp/in" 2>"$tmp/err")
    if [ "$act" = "$expected" ]; then
        n=$((n + 1))
    else
        echo "gmore-nav: FAIL [$label] keys='$keys'" >&2
        echo "  expected: $expected" >&2
        echo "  actual:   $act" >&2
        [ -s "$tmp/err" ] && { echo "  stderr:" >&2; sed 's/^/    /' "$tmp/err" >&2; }
        fails=$((fails + 1))
    fi
}

# Initial window: top of file, first page visible.
check "initial"          ''     'top=0 bottom=23 total=50 pct=46% more'
# Page forward (space and f are equivalent), then back.
check "page fwd f"       'f'    'top=23 bottom=46 total=50 pct=92% more'
check "page fwd space"   ' '    'top=23 bottom=46 total=50 pct=92% more'
check "page fwd then back" 'fb' 'top=0 bottom=23 total=50 pct=46% more'
# Line stepping.
# Enter/\r share j's code path in Nav::dispatch; \n is awkward to pass through a
# shell string, so j stands in for all three line-forward keys here.
check "line down j"      'j'    'top=1 bottom=24 total=50 pct=48% more'
check "line up clamps"   'k'    'top=0 bottom=23 total=50 pct=46% more'
check "down then up"     'jjk'  'top=1 bottom=24 total=50 pct=48% more'
# Count prefixes (more(1)'s "10j", "5b" …).
check "count 10j"        '10j'  'top=10 bottom=33 total=50 pct=66% more'
check "count 25j clamps" '25j'  'top=25 bottom=48 total=50 pct=96% more'
check "count 99j clamps" '99j'  'top=27 bottom=50 total=50 pct=100% END'
# A long digit run must not overflow; it just clamps to the end like any big count.
check "count overflow"   '999999999999999999999j' 'top=27 bottom=50 total=50 pct=100% END'
check "count then back"  '20j5k' 'top=15 bottom=38 total=50 pct=76% more'
check "count resets"     '5jj'  'top=6 bottom=29 total=50 pct=58% more'
# Half-screen scroll d/u (pageH=23 -> half = 11); a count sets a sticky step.
check "half down d"      'd'    'top=11 bottom=34 total=50 pct=68% more'
check "half down twice"  'dd'   'top=22 bottom=45 total=50 pct=90% more'
check "half up u"        'ddu'  'top=11 bottom=34 total=50 pct=68% more'
check "d count sets step" '5d'  'top=5 bottom=28 total=50 pct=56% more'
check "d step sticks"    '5dd'  'top=10 bottom=33 total=50 pct=66% more'
check "u uses d step"    '5ddu' 'top=5 bottom=28 total=50 pct=56% more'
# Go to top / bottom; g/G with a count go to line N (1-based).
check "G to bottom"      'G'    'top=27 bottom=50 total=50 pct=100% END'
check "g to top"         'Gg'   'top=0 bottom=23 total=50 pct=46% more'
check "G3 line 3"        'G3G'  'top=2 bottom=25 total=50 pct=50% more'
check "g count line 10"  '10g'  'top=9 bottom=32 total=50 pct=64% more'
check "G count clamps"   '99G'  'top=27 bottom=50 total=50 pct=100% END'
check "g works at end"   'Gg'   'top=0 bottom=23 total=50 pct=46% more'
# Forward steps clamp at maxTop (27); (END) reached.
check "page to end"      'fff'  'top=27 bottom=50 total=50 pct=100% END'
# more(1) quirk: space at (END) quits (script stops there); other fwd keys no-op.
check "space at end quits" 'fffj j' 'top=27 bottom=50 total=50 pct=100% END'
# q quits immediately, leaving the view unmoved.
check "q quits"          'jq'   'top=1 bottom=24 total=50 pct=48% more'

if [ "$fails" -eq 0 ]; then
    echo "gmore-nav: OK ($n cases)"
    exit 0
fi
echo "gmore-nav: FAIL ($fails of $((n + fails)) cases)" >&2
exit 1
