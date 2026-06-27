#!/bin/sh
# mdcat's Kitty graphics backend (producer side): asserts the structural
# invariants of the Kitty APC stream mdcat emits, which gmore and terminals
# rely on. The companion gmore-side ingest/placement is covered by
# tests/gmore-repaint.sh; this pins what mdcat PRODUCES.
#
# Invariants checked, from a two-image document:
#   - each <img> becomes a Kitty APC transmission (ESC _ G ... ESC \);
#   - every command carries q=2 (so the terminal's ;OK reply stays silent and
#     can't leak onscreen);
#   - the two images get DISTINCT image ids (a reused i= would make the second
#     a=T re-lay the first placement and corrupt it — see ADR 0002);
#   - no stray cursor-management codes leak (timg's own ESC[?25l / ESC[?25h are
#     stripped by runTimgKitty, since mdcat positions images itself);
#   - the `none` backend draws no graphics at all and falls back to text;
#   - `--img sixel` selects the sixel backend (no Kitty APC), proving the CLI
#     protocol override.
#
# The image protocol is forced via `--img <proto>` so the test is independent of
# the terminal it runs under. Cell size is pinned with MDCAT_CELL_W/H so timg's
# geometry (and thus the byte stream) is deterministic.
#
# Requires `timg` (the image renderer); skips cleanly when it is absent.
#
# Usage: tests/mdcat-kitty.sh [mdcat-binary]
# Exit status: 0 if all cases pass (or skipped), 1 otherwise.

set -u

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
mdcat=${1:-$root/mdcat}

if [ ! -x "$mdcat" ]; then
    echo "mdcat-kitty: $mdcat is not an executable; run 'make' first" >&2
    exit 2
fi
if ! command -v timg >/dev/null 2>&1; then
    echo "mdcat-kitty: SKIP (timg not installed)"
    exit 0
fi
if [ ! -f "$here/chess-piece.png" ]; then
    echo "mdcat-kitty: SKIP (tests/chess-piece.png missing)"
    exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fails=0
n=0

# A document with two lone-<img> paragraphs (the only form that renders as an
# image). Both point at the same PNG to prove ids are still made distinct. The
# src is absolute (mdcat resolves a relative src against the .md file's directory,
# which here is $tmp), so the document is location-independent.
png="$here/chess-piece.png"
cat > "$tmp/two.md" <<EOF
<img src="$png" alt="A black chess knight">

<img src="$png" alt="A black chess knight">
EOF

# Force the backend + cell size so the byte stream is deterministic.
render() {  # render PROTO -> bytes on stdout
    MDCAT_CELL_W=8 MDCAT_CELL_H=16 "$mdcat" --img "$1" "$tmp/two.md" 2>/dev/null
}

pass() { n=$((n + 1)); }
fail() { echo "mdcat-kitty: FAIL [$1] ($2)" >&2; fails=$((fails + 1)); }

# check LABEL: run the python assertion in $2 over the rendered bytes (var d).
pycheck() {  # pycheck LABEL PROTO PY
    label=$1; proto=$2; py=$3
    render "$proto" > "$tmp/out"
    if msg=$(MDCAT_OUT="$tmp/out" python3 - "$py" <<'PY'
import os, re, sys
d = open(os.environ["MDCAT_OUT"], "rb").read()
chunks = re.findall(rb"\x1b_G(.*?)\x1b\\", d, re.S)        # control;payload of each APC
ctrls  = [c.split(b";", 1)[0] for c in chunks]            # control part of each
firstids = [m for c in ctrls for m in re.findall(rb"i=(\d+)", c)]
ok, why = eval(sys.argv[1], {
    "d": d, "chunks": chunks, "ctrls": ctrls, "firstids": firstids,
    "n_apc": len(chunks),
})
print("OK" if ok else why)
sys.exit(0 if ok else 1)
PY
)
    then pass
    else fail "$label" "$msg"
    fi
}

# Kitty: two images -> two APC transmissions, each with q=2, distinct ids, no leaks.
pycheck "kitty: two APC transmissions" kitty \
    '(n_apc == 2, f"expected 2 APCs, got {n_apc}")'
pycheck "kitty: every command q=2" kitty \
    '(all(b"q=2" in c for c in ctrls), "an APC is missing q=2")'
pycheck "kitty: distinct image ids" kitty \
    '(len(set(firstids)) == 2 and len(firstids) == 2, f"expected 2 distinct ids, got {firstids}")'
pycheck "kitty: no cursor-code leak" kitty \
    '(d.count(b"\x1b[?25l") == 0 and d.count(b"\x1b[?25h") == 0, "stray ESC[?25l/h present")'
pycheck "kitty: no ;OK reply request" kitty \
    '(d.count(b"a=q") == 0, "an a=q query leaked into output")'

# none: no graphics bytes, falls back to the alt text.
pycheck "none: no graphics" none \
    '(n_apc == 0 and d.count(b"\x1bP") == 0, "graphics bytes present under --img none")'
pycheck "none: alt-text fallback" none \
    '(b"knight" in d, "alt text missing from text fallback")'

# sixel: the CLI override selects the sixel backend (DCS, no Kitty APC).
pycheck "sixel: DCS not APC" sixel \
    '(n_apc == 0 and d.count(b"\x1bP") >= 2, "expected sixel DCS, not Kitty APC")'

if [ "$fails" -eq 0 ]; then
    echo "mdcat-kitty: OK ($n cases)"
    exit 0
fi
echo "mdcat-kitty: FAIL ($fails of $((n + fails)) cases)" >&2
exit 1
