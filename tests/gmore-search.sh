#!/bin/sh
# gmore's search (/, ?, n, N), checked deterministically via `--nav-trace`: it
# replays the GMORE_KEYS script through Nav + Search against the parsed grid (no
# tty, no painting) and prints the final view window as one plain-text line:
#     top=R bottom=B total=T pct=P% (more|END)[ notfound| badre]
# A `/pattern\n` or `?pattern\n` runs a search; n/N repeat it. Search is regex,
# smart-case (case-insensitive unless the pattern has an uppercase letter), and
# wraps around the file like less.
#
# Geometry is fixed at LINES=6 (pageH=5), COLUMNS=40 over the file below, so a
# match near the end clamps its top to maxTop rather than reaching the very top.
#
# Usage: tests/gmore-search.sh [gmore-binary]
# Exit status: 0 if all cases pass, 1 otherwise (printing diffs).

set -u

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
gmore=${1:-$root/gmore}

if [ ! -x "$gmore" ]; then
    echo "gmore-search: $gmore is not an executable; run 'make' first" >&2
    exit 2
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# 20-line file: rows 1-based as printed; matches at known rows.
#   row 2  : MATCH alpha
#   row 6  : match beta   (lowercase, for smart-case)
#   row 11 : MATCH gamma
#   row 17 : MATCH delta
cat > "$tmp/in" <<'EOF'
one
MATCH alpha
three
four
five
match beta
seven
eight
nine
ten
MATCH gamma
twelve
thirteen
fourteen
fifteen
sixteen
MATCH delta
eighteen
nineteen
twenty
EOF
# maxTop = 20 - 5 = 15.

fails=0
n=0

# check LABEL KEYS EXPECTED — replay KEYS, assert the trace line equals EXPECTED.
check() {
    label=$1; keys=$2; expected=$3
    act=$(GMORE_KEYS="$keys" LINES=6 COLUMNS=40 "$gmore" --nav-trace "$tmp/in" 2>"$tmp/err")
    if [ "$act" = "$expected" ]; then
        n=$((n + 1))
    else
        echo "gmore-search: FAIL [$label] keys='$keys'" >&2
        echo "  expected: $expected" >&2
        echo "  actual:   $act" >&2
        [ -s "$tmp/err" ] && { echo "  stderr:" >&2; sed 's/^/    /' "$tmp/err" >&2; }
        fails=$((fails + 1))
    fi
}

# A literal newline (terminates a search pattern). Command substitution strips
# trailing newlines, so embed one directly in a quoted string.
nl='
'

# Forward search from the top: first MATCH is row 2 -> viewTop 1.
check "find first"        "/MATCH$nl"          'top=1 bottom=6 total=20 pct=30% more'
# Smart-case: lowercase pattern matches the uppercase rows too; first is row 2.
check "smartcase lower"   "/match$nl"          'top=1 bottom=6 total=20 pct=30% more'
# Case-sensitive: an uppercase letter disables icase. "MATCH" (uc) is exactly
# the row-2/11/17 spelling, so from the top it still hits row 2 (alpha)...
check "case uc hits uc"   "/MATCH$nl"          'top=1 bottom=6 total=20 pct=30% more'
# ...but "Beta" (uc B) is case-sensitive and the file only has "beta" (lc): no hit.
check "case sensitive"    "/Beta$nl"           'top=0 bottom=5 total=20 pct=25% more notfound'
# n repeats forward: row 2 -> row 11 (gamma) -> viewTop 10.
check "n repeats"         "/MATCH${nl}n"       'top=10 bottom=15 total=20 pct=75% more'
# n again reaches delta (row 17); top clamps to maxTop 15.
check "n to last clamps"  "/MATCH${nl}nn"      'top=15 bottom=20 total=20 pct=100% END'
# n wraps from the last match back to the first.
check "n wraps"           "/MATCH${nl}nnn"     'top=1 bottom=6 total=20 pct=30% more'
# N reverses direction: forward to gamma, then back to alpha.
check "N reverses"        "/MATCH${nl}nN"      'top=1 bottom=6 total=20 pct=30% more'
# Backward search ?: from the top it wraps to the last match (delta, row 17).
check "backward search"   "?MATCH$nl"          'top=15 bottom=20 total=20 pct=100% END'
# Regex anchors: ^match beta$ hits exactly row 6.
check "regex anchored"    "/^match beta\$$nl"  'top=5 bottom=10 total=20 pct=50% more'
# Not found leaves the view put and notes it.
check "not found"         "/zzz$nl"            'top=0 bottom=5 total=20 pct=25% more notfound'
# A malformed regex is reported and does not move.
check "bad regex"         "/[$nl"              'top=0 bottom=5 total=20 pct=25% more badre'
# n with no prior search: nothing to repeat.
check "n no prior"        "n"                  'top=0 bottom=5 total=20 pct=25% more notfound'
# Search composes with motion: page down, then search continues from there.
check "search after move" "f/MATCH$nl"         'top=10 bottom=15 total=20 pct=75% more'

if [ "$fails" -eq 0 ]; then
    echo "gmore-search: OK ($n cases)"
    exit 0
fi
echo "gmore-search: FAIL ($fails of $((n + fails)) cases)" >&2
exit 1
