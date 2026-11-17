#!/bin/sh
# mdcat's built-in Mermaid flowcharts (docs/plan-mermaid-flowcharts.md, ADR 0004): on a Kitty
# terminal an acyclic ```mermaid flowchart of boxes joined by --> links is drawn as one transparent
# PNG (strokes) plus ordinary text rows (labels). Anything else is left to the mmdc / source
# fallback.
#
# Checked here, over tests/mermaid-basic.md (widths 80 and 30), tests/mermaid-dag.md (80 and 44),
# tests/mermaid-routing.md (80), tests/mermaid-crossings.md (80), tests/mermaid-syntax.md (80),
# tests/mermaid-links.md (80) and tests/mermaid-fallback.md:
#   - tests/kitty-preview.py's rendering of the output of `mdcat --mermaid builtin` (text plus each
#     image as quadrant blocks) equals tests/expected/<fixture>.w<N>.txt, and the
#     MDCAT_MERMAID_DEBUG=1 stderr equals <fixture>.w<N>.err (UPDATE=1 rewrites both). The
#     previewer also fails on any PNG with a bad signature, CRC, zlib stream or size
#     (cols*cellW x rows*cellH);
#   - --mermaid / MDCAT_MERMAID precedence: source shows the source, the flag beats the variable,
#     and an invalid mode exits 2;
#   - every APC's first chunk carries f=100, C=1, c=, r=, z=0 and q=2, and every chunk carries q=2;
#   - gmore anchors each image where the previewer does, with the same cells footprint, and its
#     text layer matches the previewer's on every row an image covers;
#   - gmore anchors single diagrams at the first diagram row, with the expected footprint;
#   - neither piped output without --img nor --img sixel contains a Kitty APC.
#
# Cell metrics are pinned (MDCAT_CELL_W/H) so the PNG bytes are deterministic. mmdc is hidden from
# PATH so a declined diagram falls back to its source regardless of the machine.
#
# Needs python3 (SKIP otherwise).
#
# Usage: tests/mermaid-kitty.sh [mdcat-binary]
# Exit status: 0 if all cases pass (or skipped), 1 otherwise.

set -u

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
mdcat=${1:-$root/mdcat}
gmore=$root/gmore

if [ ! -x "$mdcat" ] || [ ! -x "$gmore" ]; then
    echo "mermaid-kitty: mdcat or gmore is not an executable; run 'make' first" >&2
    exit 2
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fails=0
n=0
pass() { n=$((n + 1)); }
fail() { echo "mermaid-kitty: FAIL [$1]${2:+ ($2)}" >&2; fails=$((fails + 1)); }

export MDCAT_CELL_W=10 MDCAT_CELL_H=20 GMORE_CELLW=10 GMORE_CELLH=20
if ! command -v python3 >/dev/null 2>&1; then
    echo "mermaid-kitty: SKIP (python3 is not installed)"
    exit 0
fi
mc() { PATH=/usr/bin:/bin "$mdcat" "$@"; }
if PATH=/usr/bin:/bin command -v mmdc >/dev/null 2>&1; then
    echo "mermaid-kitty: SKIP (mmdc is on the minimal PATH)"
    exit 0
fi

preview() { python3 "$here/kitty-preview.py" "$@"; }

# Render <fixture> at <width> with the builtin only, and compare the previewer's text and the
# debug stderr against tests/expected/<name>.w<width>.{txt,err}. Leaves the output in $tmp/out and
# its preview in $tmp/summary.
golden_case() {
    name=$1
    width=$2
    md=$here/$name.md
    txt=$here/expected/$name.w$width.txt
    err=$here/expected/$name.w$width.err
    MDCAT_MERMAID_DEBUG=1 mc --img kitty --mermaid builtin --width "$width" "$md" \
        > "$tmp/out" 2> "$tmp/err"
    if [ "${UPDATE:-}" = 1 ]; then
        mkdir -p "$(dirname "$txt")"
        preview "$tmp/out" > "$txt"
        cp "$tmp/err" "$err"
        echo "mermaid-kitty: wrote $txt and $err"
    fi
    if preview "$tmp/out" > "$tmp/summary" 2> "$tmp/preview.err"; then
        if diff -u "$txt" "$tmp/summary" > "$tmp/diff"; then
            pass
        else
            fail "golden $name.w$width" "output differs from $txt"
            cat "$tmp/diff" >&2
        fi
    else
        fail "preview $name.w$width" "$(cat "$tmp/preview.err")"
    fi
    if [ -f "$err" ]; then
        if diff -u "$err" "$tmp/err" > "$tmp/diff"; then
            pass
        else
            fail "stderr $name.w$width" "debug output differs from $err"
            cat "$tmp/diff" >&2
        fi
    fi
}

golden_case mermaid-fallback 80
golden_case mermaid-dag 44
golden_case mermaid-dag 80
golden_case mermaid-routing 80
golden_case mermaid-crossings 80
golden_case mermaid-syntax 80
golden_case mermaid-links 80
golden_case mermaid-basic 30
golden_case mermaid-basic 80

# The quadrant previews are too coarse to show the dashes of a dotted link (a dash is 3s px, under
# half a cell), so read the pixels: along a dotted link every dash but the first and last is 3s px
# long and 3s px or more apart, and a dotted run starts and ends on a dash.
dots="$tmp/dots.md"
printf '```mermaid\ngraph TD\n    a[Top] -.- b[Bottom]\n```\n' > "$dots"
mc --img kitty --mermaid builtin --width 80 "$dots" > "$tmp/dots.out" 2>/dev/null
if msg=$(python3 - "$here/kitty-preview.py" "$tmp/dots.out" <<'PY'
import base64, importlib.util, re, sys
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("kp", sys.argv[1])
kp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kp)
d = open(sys.argv[2], "rb").read()
chunks = re.findall(rb"\x1b_G[^;]*;(.*?)\x1b\\", d, re.S)
w, h, rows = kp.decode_png(base64.b64decode(b"".join(chunks)))
best = []
for x in range(w):
    col = [rows[y][4 * x + 3] > 0 for y in range(h)]
    runs, y = [], 0
    while y < h:
        if col[y]:
            e = y
            while e < h and col[e]:
                e += 1
            runs.append((y, e))
            y = e
        else:
            y += 1
    if len(runs) > len(best):
        best = runs
if len(best) < 5:
    sys.exit("expected a dashed column, got %d runs" % len(best))
for (a, b) in best[1:-1]:
    if b - a != 3:
        sys.exit("a dash is %d px, expected 3: %r" % (b - a, best))
for (_, b), (a, _) in zip(best, best[1:]):
    if a - b < 3:
        sys.exit("a gap is %d px, expected at least 3: %r" % (a - b, best))
PY
); then
    pass
else
    fail "dotted link dashes" "$msg"
fi

# Structure of every APC. (The previewer has already decoded every PNG: signature, CRC, zlib
# stream, and size c*cellW x r*cellH.)
if msg=$(python3 - "$tmp/out" <<'PY'
import re, sys
d = open(sys.argv[1], "rb").read()
chunks = re.findall(rb"\x1b_G(.*?)\x1b\\", d, re.S)
firsts = 0
for c in chunks:
    ctrl = c.split(b";", 1)[0]
    if b"q=2" not in ctrl:
        sys.exit("a chunk lacks q=2")
    if b"a=" not in ctrl:
        continue
    firsts += 1
    keys = dict(k.split(b"=") for k in ctrl.split(b","))
    for k, v in ((b"f", b"100"), (b"C", b"1"), (b"z", b"0"), (b"q", b"2")):
        if keys.get(k) != v:
            sys.exit("first chunk lacks %s=%s: %r" % (k.decode(), v.decode(), ctrl))
    if b"c" not in keys or b"r" not in keys:
        sys.exit("first chunk lacks c=/r=")
if firsts != 3:
    sys.exit("expected 3 images, got %d" % firsts)
PY
); then
    pass
else
    fail "apc structure" "$msg"
fi

# gmore must anchor each image where the previewer does, with the same footprint, and its text
# layer must match the previewer's on every row an image covers.
preview --labels "$tmp/out" > "$tmp/labels"
"$gmore" --imginfo < "$tmp/out" 2>&1 | sed -E 's/^image [0-9]+ @([0-9]+,[0-9]+) [0-9]+x[0-9]+px ([0-9]+)x([0-9]+)cells$/\1 \2x\3/' > "$tmp/gm-info"
sed -nE 's/^image @([0-9]+,[0-9]+) c=([0-9]+) r=([0-9]+) .*$/\1 \2x\3/p' "$tmp/summary" > "$tmp/pv-info"
if [ -s "$tmp/pv-info" ] && cmp -s "$tmp/pv-info" "$tmp/gm-info"; then
    pass
else
    fail "gmore vs preview anchors" "preview: $(tr '\n' ';' < "$tmp/pv-info") gmore: $(tr '\n' ';' < "$tmp/gm-info")"
fi
"$gmore" --dump < "$tmp/out" 2>&1 | perl -pe 's/\e\[[0-9;?]*[A-Za-z]//g; s/\s+$//; $_ .= "\n"' > "$tmp/gm-dump"
while read -r pos size; do
    row=${pos%,*}
    rows=${size#*x}
    from=$((row + 1))
    to=$((row + rows))
    sed -n "${from},${to}p" "$tmp/gm-dump" > "$tmp/gm-rows"
    sed -n "${from},${to}p" "$tmp/labels" > "$tmp/pv-rows"
    if cmp -s "$tmp/pv-rows" "$tmp/gm-rows"; then
        pass
    else
        fail "gmore vs preview labels" "rows $from-$to differ"
        diff "$tmp/pv-rows" "$tmp/gm-rows" >&2
    fi
done < "$tmp/pv-info"

# gmore: image anchor, footprint, and the label row. Each diagram is rendered alone, so the image
# must sit at row 0 of the document.
lr="$tmp/lr.md"
cat > "$lr" <<'EOF'
```mermaid
graph LR
    parse[Parse] --> render[Render]
    render --> output[Output]
```
EOF
mc --img kitty --width 80 "$lr" 2>/dev/null | "$gmore" --imginfo > "$tmp/info" 2>&1
if [ "$(cat "$tmp/info")" = "image 1 @0,21 370x60px 37x3cells" ]; then
    pass
else
    fail "gmore anchor (LR)" "got: $(cat "$tmp/info")"
fi
mc --img kitty --width 80 "$lr" 2>/dev/null | "$gmore" --dump > "$tmp/dump" 2>&1
if sed -n 2p "$tmp/dump" | grep -q '^ *Parse  *Render  *Output *$'; then
    pass
else
    fail "gmore label row (LR)" "grid row 1: $(sed -n 2p "$tmp/dump")"
fi

td="$tmp/td.md"
{
    echo '```mermaid'
    echo 'graph TD'
    echo '    a[Start] --> b[Step 1]'
    echo '    b --> c[End]'
    echo '```'
} > "$td"
mc --img kitty --width 80 "$td" 2>/dev/null | "$gmore" --imginfo > "$tmp/info" 2>&1
if [ "$(cat "$tmp/info")" = "image 1 @0,34 110x260px 11x13cells" ]; then
    pass
else
    fail "gmore anchor (TD)" "got: $(cat "$tmp/info")"
fi

# Mode precedence. The chain is supported by the builtin, so only the mode decides what shows.
src_has() { grep -q 'graph LR' "$1"; }
mc --img kitty --width 80 --mermaid source "$lr" > "$tmp/m-source" 2>/dev/null
if src_has "$tmp/m-source" && ! grep -q "$(printf '\033_G')" "$tmp/m-source"; then
    pass
else
    fail "--mermaid source" "expected the source and no image"
fi
MDCAT_MERMAID=source mc --img kitty --width 80 --mermaid builtin "$lr" > "$tmp/m-flag" 2>/dev/null
if grep -q "$(printf '\033_G')" "$tmp/m-flag" && ! src_has "$tmp/m-flag"; then
    pass
else
    fail "flag beats MDCAT_MERMAID" "expected the builtin diagram"
fi
MDCAT_MERMAID=source mc --img kitty --width 80 "$lr" > "$tmp/m-env" 2>/dev/null
if src_has "$tmp/m-env" && ! grep -q "$(printf '\033_G')" "$tmp/m-env"; then
    pass
else
    fail "MDCAT_MERMAID=source" "expected the source and no image"
fi
mc --img kitty --mermaid=builtin --width 80 "$lr" 2>/dev/null | grep -q "$(printf '\033_G')" \
    && pass || fail "--mermaid=builtin" "expected the builtin diagram"
# image mode never runs the builtin, so with no mmdc on PATH it shows the source.
mc --img kitty --width 80 --mermaid image "$lr" > "$tmp/m-image" 2>/dev/null
if src_has "$tmp/m-image" && ! grep -q "$(printf '\033_G')" "$tmp/m-image"; then
    pass
else
    fail "--mermaid image" "expected the source (no mmdc)"
fi
mc --mermaid bogus "$lr" > /dev/null 2> "$tmp/bad.err"
rc=$?
if [ "$rc" -eq 2 ] && grep -q 'usage:' "$tmp/bad.err"; then
    pass
else
    fail "invalid --mermaid" "exit $rc"
fi
MDCAT_MERMAID=bogus mc "$lr" > /dev/null 2>&1
rc=$?
if [ "$rc" -eq 2 ]; then pass; else fail "invalid MDCAT_MERMAID" "exit $rc"; fi

# No Kitty APC without --img (piped), nor under --img sixel.
if mc "$lr" 2>/dev/null | grep -q "$(printf '\033_G')"; then
    fail "piped without --img" "contains a Kitty APC"
else
    pass
fi
if mc --img sixel --width 80 "$lr" 2>/dev/null | grep -q "$(printf '\033_G')"; then
    fail "--img sixel" "contains a Kitty APC"
else
    pass
fi

if [ "$fails" -eq 0 ]; then
    echo "mermaid-kitty: OK ($n cases)"
    exit 0
fi
echo "mermaid-kitty: FAIL ($fails of $((n + fails)) cases)" >&2
exit 1
