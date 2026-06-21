#!/bin/sh
# Unicode display-width handling for mdcat AND gmore's emulator.
#
# mdcat lays out tables (and reflow) by display columns, not code points or bytes, so
# a fullwidth emoji or CJK character must count as two columns and a combining mark or
# ZWJ/variation-selector as zero. If it miscounts, a table column with such a character
# is padded wrong and the whole grid misaligns. This test renders tables/paragraphs
# containing widely-recognised wide emoji, CJK text, combining sequences and a couple of
# famously-tricky clusters (a 4-person ZWJ family, a regional-indicator flag), then
# checks the result with an INDEPENDENT width oracle (a small Python wcwidth-style
# function — deliberately NOT mdcat's own code, so the two can disagree).
#
# Asserted properties:
#   1. mdcat: every line of a rendered table is the same display width (columns line up).
#   2. mdcat: reflow at width N never produces a line wider than N display columns.
#   3. gmore: feeding mdcat's output through the emulator preserves the exact bytes of
#      these clusters (the grapheme clustering in put() reassembles them losslessly).
#
# Usage: tests/unicode-width.sh [mdcat-binary] [gmore-binary]
# Exit status: 0 if all properties hold, 1 otherwise (printing the offending lines).

set -u

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
mdcat=${1:-$root/mdcat}
gmore=${2:-$root/gmore}

for bin in "$mdcat" "$gmore"; do
    if [ ! -x "$bin" ]; then
        echo "unicode-width: $bin is not an executable; run 'make' first" >&2
        exit 2
    fi
done

py=$(command -v python3 || command -v python || true)
if [ -z "$py" ]; then
    echo "unicode-width: SKIP (no python3 for the width oracle)" >&2
    exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
status=0

# --- the independent display-width oracle -----------------------------------
# Mirrors a wcwidth: combining/joiner/VS = 0, East-Asian Wide/Fullwidth and the emoji
# pictograph blocks = 2, a regional-indicator pair = 2 (one flag). It does NOT share
# code with mdcat, so it is a genuine cross-check.
cat > "$tmp/oracle.py" <<'PY'
import sys, re, unicodedata

def cpw(cp):
    if cp == 0:
        return 0
    if unicodedata.combining(chr(cp)):
        return 0
    if cp in (0x200B, 0x200C, 0x200D, 0xFEFF):
        return 0
    if 0xFE00 <= cp <= 0xFE0F or 0xE0100 <= cp <= 0xE01EF:
        return 0
    if (0x1F300 <= cp <= 0x1FAFF) or (0x1F000 <= cp <= 0x1F2FF):
        return 2
    if unicodedata.east_asian_width(chr(cp)) in ('W', 'F'):
        return 2
    return 1

def dwidth(s):
    s = re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', s)           # CSI
    s = re.sub(r'\x1b\]8;;.*?(\x07|\x1b\\)', '', s)        # OSC 8 hyperlink
    w = 0
    i = 0
    while i < len(s):
        cp = ord(s[i])
        if 0x1F1E6 <= cp <= 0x1F1FF and i + 1 < len(s) and 0x1F1E6 <= ord(s[i+1]) <= 0x1F1FF:
            w += 2          # regional-indicator pair -> one flag, two columns
            i += 2
            continue
        w += cpw(cp)
        i += 1
    return w

mode = sys.argv[1]
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 0
lines = sys.stdin.read().split('\n')
if lines and lines[-1] == '':
    lines.pop()

if mode == 'equal':
    widths = [dwidth(l) for l in lines if l.strip() != '']
    if len(set(widths)) > 1:
        sys.stderr.write('  table lines have differing display widths: %r\n' % widths)
        for l in lines:
            sys.stderr.write('    %2d | %s\n' % (dwidth(l), l))
        sys.exit(1)
elif mode == 'maxwidth':
    bad = [(dwidth(l), l) for l in lines if dwidth(l) > limit]
    if bad:
        for wdt, l in bad:
            sys.stderr.write('  line exceeds %d columns (%d): %s\n' % (limit, wdt, l))
        sys.exit(1)
sys.exit(0)
PY

check_equal() {
    label=$1; shift
    if printf '%s' "$1" | "$mdcat" | "$py" "$tmp/oracle.py" equal 2>"$tmp/err"; then
        :
    else
        echo "unicode-width: FAIL [$label] — table columns misaligned" >&2
        cat "$tmp/err" >&2
        status=1
    fi
}

check_maxwidth() {
    label=$1; n=$2; input=$3
    if printf '%s' "$input" | COLUMNS="$n" "$mdcat" | "$py" "$tmp/oracle.py" maxwidth "$n" 2>"$tmp/err"; then
        :
    else
        echo "unicode-width: FAIL [$label] — reflow exceeded $n columns" >&2
        cat "$tmp/err" >&2
        status=1
    fi
}

# check_roundtrip LABEL INPUT CLUSTER...: render INPUT with mdcat, feed it through the
# gmore emulator (--dump), and assert each CLUSTER still appears byte-for-byte. The
# emulator may trim trailing blanks, so we test presence, not whole-line equality.
check_roundtrip() {
    label=$1; input=$2; shift 2
    printf '%s' "$input" | "$mdcat" > "$tmp/m"
    LINES=60 COLUMNS=120 GMORE_CELLW=8 GMORE_CELLH=16 "$gmore" --dump "$tmp/m" > "$tmp/g"
    missing=""
    for cluster in "$@"; do
        if ! grep -qF "$cluster" "$tmp/g"; then
            missing="$missing $cluster"
        fi
    done
    if [ -n "$missing" ]; then
        echo "unicode-width: FAIL [$label] — emulator dropped cluster(s):$missing" >&2
        status=1
    fi
}

# --- fixtures ---------------------------------------------------------------
# Widely-recognised wide emoji + CJK + a combining sequence, all in one table. If any
# is mis-measured the rows won't share a display width.
emoji_table='| Symbol | Meaning |
|--------|---------|
| 😀 | grinning face |
| 🚀 | rocket |
| 🇯🇵 | flag of Japan |
| 中文 | Chinese text |
| 한국어 | Korean |
| café (combining) | accent |
| plain | ascii |
'

# A second table that mixes wide and narrow within one cell, the case most likely to
# expose an off-by-one in column sizing.
mixed_table='| Code | Note |
|------|------|
| a😀b | wide in middle |
| 中x文 | cjk around ascii |
| eé (combining) | mark |
'

# Hard clusters: a 4-person family built from ZWJ, a flag, a skin-toned wave, and a
# base+combining letter — these must survive reflow and the emulator intact.
hard_para='A paragraph with a ZWJ family 👨‍👩‍👧‍👦 then a flag 🇯🇵 then a waving hand 👋🏽 and an accented cafe (combining mark) word word word word word word word word done.
'

check_equal "emoji/cjk/combining table" "$emoji_table"
check_equal "mixed wide+narrow table"   "$mixed_table"
check_maxwidth "reflow with hard clusters" 40 "$hard_para"
check_maxwidth "reflow with hard clusters (narrow)" 24 "$hard_para"

# Round-trip the clusters through gmore's emulator and confirm none are lost.
# (Cluster literals embedded directly so the grep is byte-exact.)
check_roundtrip "emulator preserves table clusters" "$emoji_table" \
    "😀" "🚀" "中文" "한국어" "🇯🇵"
check_roundtrip "emulator preserves hard clusters" "$hard_para" \
    "👨‍👩‍👧‍👦" "🇯🇵" "👋🏽"

if [ "$status" -eq 0 ]; then
    echo "unicode-width: OK"
fi
exit "$status"
