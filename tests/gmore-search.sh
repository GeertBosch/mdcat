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

# --- Match highlighting (live render path, not --nav-trace) ----------------------
# A search highlights matches with a blue BACKGROUND: the current match (the one
# searched to) gets the saturated CUR colour, other on-screen matches the lighter
# OTHER colour. We assert the SGR counts in the rendered byte stream. gmore renders
# off-tty only with GMORE_KEYS set (see the header); a small LINES forces paging so
# search applies (a one-screen file early-returns before the key loop).
cur='48;5;75'      # current-match background (must match Highlight::CUR in gmore_core.h)
other='48;5;153'   # other-match background  (must match Highlight::OTHER)

# hcheck LABEL KEYS WANT_CUR WANT_OTHER — render, assert highlight-bg counts on the
# FINAL screen only. The captured stream concatenates every repaint, so we take the
# bytes after the last RIS (ESC c) — the last full repaint — before counting.
hcheck() {
    label=$1; keys=$2; wcur=$3; woth=$4
    out=$(GMORE_KEYS="$keys" LINES=6 COLUMNS=40 "$gmore" "$tmp/in" 2>"$tmp/err" \
          | awk 'BEGIN{RS="\033c"} {last=$0} END{printf "%s", last}')
    gcur=$(printf '%s' "$out" | grep -ao "$cur" | wc -l | tr -d ' ')
    goth=$(printf '%s' "$out" | grep -ao "$other" | wc -l | tr -d ' ')
    if [ "$gcur" = "$wcur" ] && [ "$goth" = "$woth" ]; then
        n=$((n + 1))
    else
        echo "gmore-search: FAIL [$label] keys='$keys'" >&2
        echo "  expected: current=$wcur other=$woth" >&2
        echo "  actual:   current=$gcur other=$goth" >&2
        [ -s "$tmp/err" ] && { echo "  stderr:" >&2; sed 's/^/    /' "$tmp/err" >&2; }
        fails=$((fails + 1))
    fi
}

# /MATCH is case-sensitive (an uppercase letter disables icase), so it matches only
# the "MATCH" rows (2, 11, 17), not "match beta" (6). It lands on row 2 -> window
# rows 2..6; only row 2 matches there -> 1 current, 0 other.
hcheck "hl current only"  "/MATCH$nl"     1 0
# n moves the current to gamma (row 11); window rows 11..15 has only that match.
hcheck "hl after n"       "/MATCH${nl}n"  1 0
# Smart-case lowercase /match matches BOTH "MATCH alpha" (row 2) and "match beta"
# (row 6) via icase; landing on row 2, window 2..6 shows current(row2)+other(row6).
hcheck "hl current+other" "/match$nl"     1 1
# No search: zero highlight backgrounds anywhere.
hcheck "hl none"          "fq"            0 0

# --- n/N step through EVERY match, including several on one line ------------------
# A file whose first line has three matches, plus one more below. The current
# (saturated) highlight must move to each in turn — the bug being fixed: previously
# only the first match on a line could be the current one.
cat > "$tmp/multi" <<'EOF'
xx HIT aa HIT bb HIT cc
filler
filler
filler
HIT solo
EOF

# scheck PAT KEYS EXPECT LABEL — assert WHICH occurrence is the current match.
scheck() {
    # On the final screen, pick the line whose TEXT contains $pat, then reduce its
    # highlight-open SGRs to a marker string in order: C for the current match (CUR
    # colour), o for an other match (OTHER). grep -o the codes themselves (not the
    # surrounding text), so letters like the "o" in "solo" can't pollute the markers.
    pat=$1; keys=$2; expect=$3; label=$4
    got=$(GMORE_KEYS="$keys" LINES=4 COLUMNS=40 "$gmore" "$tmp/multi" 2>"$tmp/err" \
          | awk 'BEGIN{RS="\033c"} {last=$0} END{printf "%s", last}' \
          | tr '\r' '\n' | grep "$pat" \
          | grep -ao "$(printf '\033')\[48;5;\(75\|153\)m" \
          | sed -e 's/.*75m/C/' -e 's/.*153m/o/' | tr -d '\n')
    if [ "$got" = "$expect" ]; then
        n=$((n + 1))
    else
        echo "gmore-search: FAIL [$label] keys='$keys'" >&2
        echo "  expected markers: $expect" >&2
        echo "  actual markers:   $got" >&2
        fails=$((fails + 1))
    fi
}

# scheck PAT KEYS EXPECT LABEL — see the reduction comment above.
# Line 1 (text starts "xx ") holds three HIT matches; the current one steps with n/N.
scheck '^xx '  "/HIT$nl"        'Coo' "step 1st on line"
scheck '^xx '  "/HIT${nl}n"     'oCo' "step 2nd same line"
scheck '^xx '  "/HIT${nl}nn"    'ooC' "step 3rd same line"
# After wrapping off line 1 the current lands on the solo match below; assert it via
# the "solo" line (line 1 has scrolled off, so only the solo line's marker is checked).
scheck 'solo'  "/HIT${nl}nnn"   'C'   "step wraps to solo"
scheck 'solo'  "/HIT${nl}N"     'C'   "step N wraps back to solo"

if [ "$fails" -eq 0 ]; then
    echo "gmore-search: OK ($n cases)"
    exit 0
fi
echo "gmore-search: FAIL ($fails of $((n + fails)) cases)" >&2
exit 1
