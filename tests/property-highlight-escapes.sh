#!/bin/sh
# Property: syntax highlighting must not leak italic or bold across line boundaries.
#
# For every line in the output of a fenced code block, the number of italic-on escapes
# (ESC[...3m containing SGR parameter 3) must equal the number of italic-off escapes
# (containing SGR 23), and likewise for bold (1 vs 22).  A full reset (ESC[0m or ESC[m)
# counts as closing both attributes.
#
# A leak would cause the terminal to carry the attribute into the next line's background
# colour sequence and into subsequent content outside the code block.
#
# Usage: tests/property-highlight-escapes.sh [mdcat-binary] [file ...]
# Defaults: the ./mdcat next to the repo root, run over tests/code-blocks.md.
# Exit status: 0 if no leaks found, 1 if any leak is detected.

set -eu

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")

mdcat=${1:-$root/mdcat}
if [ "$#" -gt 0 ]; then shift; fi

if [ "$#" -gt 0 ]; then
    set -- "$@"
else
    set -- "$here/code-blocks.md"
fi

if [ ! -x "$mdcat" ]; then
    echo "property-highlight-escapes: $mdcat is not an executable; run 'make' first" >&2
    exit 2
fi

# Scan line by line.  For each line:
#   1. Extract all CSI parameter strings (the part between ESC[ and m).
#   2. Split each on ";" and examine individual SGR fields.
#   3. Count opens/closes for italic (3/23) and bold (1/22).
#   4. An empty or "0" parameter string is a full reset — closes both.
"$mdcat" "$@" | awk '
BEGIN { ESC = "\033"; status = 0 }
{
    line = $0
    ital_o = 0; ital_c = 0
    bold_o = 0; bold_c = 0

    rest = line
    while (1) {
        # Find next ESC[
        idx = index(rest, ESC "[")
        if (idx == 0) break
        rest = substr(rest, idx + 2)   # skip past ESC[
        # Find closing "m"
        end = index(rest, "m")
        if (end == 0) break
        params = substr(rest, 1, end - 1)
        rest   = substr(rest, end + 1)

        # Full reset: bare ESC[m or ESC[0m
        if (params == "" || params == "0") {
            ital_c++; bold_c++
            continue
        }

        # Split params on ";" and check each numeric field individually.
        nf = split(params, flds, ";")
        for (k = 1; k <= nf; k++) {
            v = flds[k] + 0
            if (v == 3)  ital_o++
            if (v == 23) ital_c++
            if (v == 1)  bold_o++
            if (v == 22) bold_c++
        }
    }

    if (ital_o > ital_c) {
        printf "property-highlight-escapes: FAIL — italic leak on line %d\n", NR > "/dev/stderr"
        status = 1
    }
    if (bold_o > bold_c) {
        printf "property-highlight-escapes: FAIL — bold leak on line %d\n", NR > "/dev/stderr"
        status = 1
    }
}
END {
    if (status == 0)
        print "property-highlight-escapes: OK"
    exit status
}
'
