#!/bin/sh
# Property: <br> HTML hard line breaks.
#
# GFM renders an inline <br> tag (https://github.github.com/gfm/#hard-line-breaks
# describes the line-break semantics; GitHub honors the literal HTML tag) as a
# line break wherever it appears in inline content — paragraphs and table cells
# alike. mdcat accepts <br>, <br/>, and <br /> case-insensitively, with optional
# whitespace before the slash. A malformed run such as <brx> or <br foo> is not a
# break and passes through as literal text, matching mdcat's "inline HTML is
# literal" rule.
#
# Each case feeds one Markdown document to mdcat, strips ANSI CSI and OSC 8
# hyperlink escapes to recover the visible text, and compares the multi-line
# result against the expected rendering.
#
# Usage: tests/property-hard-breaks.sh [mdcat-binary]
# Exit status: 0 if every case matches, 1 otherwise.

set -eu

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
mdcat=${1:-$root/mdcat}

if [ ! -x "$mdcat" ]; then
    echo "property-hard-breaks: $mdcat is not an executable; run 'make' first" >&2
    exit 2
fi

# Strip ANSI CSI sequences (ESC[ ... letter) and OSC 8 hyperlink sequences
# (ESC] ... ESC\), leaving only the visible characters.
strip() {
    perl -pe 's/\e\][^\e]*\e\\//g; s/\e\[[0-9;]*[A-Za-z]//g'
}

# Drop trailing space padding, and delete a table's box-drawing rule lines
# (── ...) entirely so only the visible cell text remains for comparison.
visible() {
    perl -pe 's/[ \t]+$//' | perl -ne 'print unless /^─+$/'
}

status=0
fail=0
pass=0

# check NAME INPUT EXPECTED — INPUT is the Markdown (newlines as literal \n in
# the string, expanded by printf); EXPECTED is the visible text, lines joined
# with \n. Output is run through mdcat -w 60, stripped of escapes, padding and
# table rules removed, then compared.
check() {
    name=$1
    input=$2
    expected=$3
    got=$(printf '%b\n' "$input" | "$mdcat" -w 60 | strip | visible)
    expected=$(printf '%b' "$expected")
    if [ "$got" = "$expected" ]; then
        pass=$((pass + 1))
    else
        printf 'property-hard-breaks: FAIL [%s]\n  expected:\n%s\n  got:\n%s\n' \
            "$name" "$expected" "$got" >&2
        fail=$((fail + 1))
        status=1
    fi
}

# A plain <br> in a paragraph breaks the line.
check para-basic 'one<br>two' 'one\ntwo'

# Self-closing and spaced forms, case-insensitive.
check para-selfclose 'a<br/>b'   'a\nb'
check para-spaced    'a<br />b'  'a\nb'
check para-upper     'a<BR>b'    'a\nb'
check para-tabslash  'a<br\t/>b' 'a\nb'

# Several breaks in one paragraph.
check para-multi 'a<br>b<br>c' 'a\nb\nc'

# Emphasis spanning a break is closed and reopened so style never leaks past the
# line; after stripping escapes the visible text is just the two lines.
check para-emphasis '**a<br>b**' 'a\nb'

# A malformed tag is literal text, not a break.
check literal-brx 'a<brx>b'   'a<brx>b'
check literal-attr 'a<br foo>b' 'a<br foo>b'

# In a table cell, a <br> stacks the cell over two rendered rows.
check table-cell \
    '| H |\n| - |\n| x<br>y |' \
    'H\nx\ny'

if [ "$status" -eq 0 ]; then
    printf 'property-hard-breaks: OK (%d cases)\n' "$pass"
else
    printf 'property-hard-breaks: %d passed, %d failed\n' "$pass" "$fail" >&2
fi
exit $status
