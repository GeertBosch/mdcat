#!/bin/sh
# Property: GFM backslash escapes (https://github.github.com/gfm/#backslash-escapes).
#
# Any ASCII punctuation character may be backslash-escaped, rendering as the
# literal character with any Markdown meaning suppressed. A backslash before any
# other character (letter, digit, space, non-ASCII) is a literal backslash.
# Backslashes inside code spans are literal and never act as escapes.
#
# Each case feeds one line of Markdown to mdcat, strips ANSI CSI and OSC 8
# hyperlink escapes from the output to recover the visible text, and compares it
# to the expected rendering.
#
# Usage: tests/property-escapes.sh [mdcat-binary]
# Exit status: 0 if every case matches, 1 otherwise.

set -eu

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
mdcat=${1:-$root/mdcat}

if [ ! -x "$mdcat" ]; then
    echo "property-escapes: $mdcat is not an executable; run 'make' first" >&2
    exit 2
fi

# Strip ANSI CSI sequences (ESC[ ... letter) and OSC 8 hyperlink sequences
# (ESC] ... ESC\), leaving only the visible characters.
strip() {
    perl -pe 's/\e\][^\e]*\e\\//g; s/\e\[[0-9;]*[A-Za-z]//g'
}

status=0
fail=0
pass=0

# check NAME INPUT EXPECTED — runs INPUT through mdcat, strips escapes, compares.
check() {
    name=$1
    input=$2
    expected=$3
    got=$(printf '%s\n' "$input" | "$mdcat" | strip)
    if [ "$got" = "$expected" ]; then
        pass=$((pass + 1))
    else
        printf 'property-escapes: FAIL [%s]\n  input:    %s\n  expected: %s\n  got:      %s\n' \
            "$name" "$input" "$expected" "$got" >&2
        fail=$((fail + 1))
        status=1
    fi
}

# Every ASCII punctuation character, escaped, renders as the bare character.
check all-punct \
    '\!\"\#\$\%\&\'\''\(\)\*\+\,\-\.\/\:\;\<\=\>\?\@\[\\\]\^\_\`\{\|\}\~' \
    '!"#$%&'\''()*+,-./:;<=>?@[\]^_`{|}~'

# Escaped markers suppress emphasis, code, and links.
check no-emphasis '\*not emphasized\*'      '*not emphasized*'
check no-code     '\`not code\`'            '`not code`'
check no-link     '\[not a link\](/foo)'    '[not a link](/foo)'
check literal-star 'a\*b\*c'                'a*b*c'

# Escapes are real where the markup is real.
check star-in-em  '*foo\*bar*'              'foo*bar'
check esc-backslash-then-em '\\*foo*'       '\foo'

# A backslash before a non-punctuation character stays literal.
check backslash-letter '\a'                 '\a'
check backslash-digit  '\3'                 '\3'

# Inside a code span, backslashes are literal — never escapes.
check code-backslash '`\*`'                 '\*'
check code-dbackslash '`\\`'                '\\'

if [ "$status" -eq 0 ]; then
    printf 'property-escapes: OK (%d cases)\n' "$pass"
else
    printf 'property-escapes: %d passed, %d failed\n' "$pass" "$fail" >&2
fi
exit $status
