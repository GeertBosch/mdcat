#!/bin/sh
# mdcat's built-in Mermaid flowcharts (docs/plan-mermaid-flowcharts.md, ADR 0004): on a Kitty
# terminal a simple-chain ```mermaid block is drawn as one transparent PNG (strokes) plus ordinary
# text rows (labels). Anything else is left to the mmdc / source fallback.
#
# Checked here, over tests/mermaid-basic.md:
#   - the output, with escapes stripped and each Kitty APC summarised to one line, equals
#     tests/expected/mermaid-basic.w80.txt (UPDATE=1 rewrites it). Cursor-forward moves are shown
#     as spaces, so the labels appear at their columns;
#   - every APC's first chunk carries f=100, C=1, c=, r=, z=0 and q=2, and every chunk carries q=2;
#   - each PNG decodes (zlib, CRC) and its size is cols*cellW x rows*cellH;
#   - gmore anchors each image at the first diagram row, with the expected cells footprint, and
#     puts the labels on the matching grid row;
#   - neither piped output without --img nor --img sixel contains a Kitty APC.
#
# Cell metrics are pinned (MDCAT_CELL_W/H) so the PNG bytes are deterministic. mmdc is hidden from
# PATH so the branching diagram falls back to its source regardless of the machine.
#
# Usage: tests/mermaid-kitty.sh [mdcat-binary]
# Exit status: 0 if all cases pass (or skipped), 1 otherwise.

set -u

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
mdcat=${1:-$root/mdcat}
gmore=$root/gmore
fixture=$here/mermaid-basic.md
golden=$here/expected/mermaid-basic.w80.txt

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
mc() { PATH=/usr/bin:/bin "$mdcat" "$@"; }
if PATH=/usr/bin:/bin command -v mmdc >/dev/null 2>&1; then
    echo "mermaid-kitty: SKIP (mmdc is on the minimal PATH)"
    exit 0
fi

mc --img kitty --width 80 "$fixture" > "$tmp/out" 2>/dev/null

# Escapes stripped; each image (first chunk plus continuations) becomes one summary line.
summarize() {
    perl -0777 -pe '
        s{\e_G([^;]*a=[^;]*);([^\e]*)\e\\((?:\e_G[^;a]*;[^\e]*\e\\)*)}{
            my ($c, $p, $rest) = ($1, $2, $3);
            my $bytes = length($p);
            while ($rest =~ m{\e_G[^;]*;([^\e]*)\e\\}g) { $bytes += length($1) }
            my %k = map { split(/=/, $_, 2) } split(/,/, $c);
            "<kitty c=$k{c} r=$k{r} z=$k{z} C=$k{C} bytes=$bytes>\n"
        }gse;
        s{\e\[(\d+)C}{" " x $1}ge;
        s{\e\[[0-9;?]*[A-Za-z]}{}g;
        s{\e[78]}{}g;
        s{\r}{}g;
    ' "$1"
}

if [ "${UPDATE:-}" = 1 ]; then
    mkdir -p "$(dirname "$golden")"
    summarize "$tmp/out" > "$golden"
    echo "mermaid-kitty: wrote $golden"
fi
summarize "$tmp/out" > "$tmp/summary"
if diff -u "$golden" "$tmp/summary" > "$tmp/diff"; then
    pass
else
    fail "golden" "output differs from $golden"
    cat "$tmp/diff" >&2
fi

# Structure of every APC, and a decode of every PNG.
if command -v python3 >/dev/null 2>&1; then
    if msg=$(python3 - "$tmp/out" <<'PY'
import re, struct, sys, zlib, base64
d = open(sys.argv[1], "rb").read()
chunks = re.findall(rb"\x1b_G(.*?)\x1b\\", d, re.S)
images = []
for c in chunks:
    ctrl, payload = c.split(b";", 1)
    if b"q=2" not in ctrl:
        sys.exit("a chunk lacks q=2")
    if b"a=" in ctrl:
        images.append([ctrl, payload])
    else:
        images[-1][1] += payload
if len(images) != 2:
    sys.exit("expected 2 images, got %d" % len(images))
for ctrl, payload in images:
    keys = dict(k.split(b"=") for k in ctrl.split(b","))
    for k, v in ((b"f", b"100"), (b"C", b"1"), (b"z", b"0"), (b"q", b"2")):
        if keys.get(k) != v:
            sys.exit("first chunk lacks %s=%s: %r" % (k.decode(), v.decode(), ctrl))
    if b"c" not in keys or b"r" not in keys:
        sys.exit("first chunk lacks c=/r=")
    png = base64.b64decode(payload)
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit("bad PNG signature")
    pos, idat = 8, b""
    while pos < len(png):
        n = struct.unpack(">I", png[pos:pos + 4])[0]
        typ, body = png[pos + 4:pos + 8], png[pos + 8:pos + 8 + n]
        if zlib.crc32(typ + body) != struct.unpack(">I", png[pos + 8 + n:pos + 12 + n])[0]:
            sys.exit("bad CRC in %r chunk" % typ)
        if typ == b"IDAT":
            idat += body
        pos += 12 + n
    w, h = struct.unpack(">II", png[16:24])
    if (w, h) != (int(keys[b"c"]) * 10, int(keys[b"r"]) * 20):
        sys.exit("PNG is %dx%d for c=%s r=%s" % (w, h, keys[b"c"].decode(), keys[b"r"].decode()))
    if len(zlib.decompress(idat)) != h * (w * 4 + 1):
        sys.exit("decompressed size is wrong")
PY
    ); then
        pass
    else
        fail "apc structure" "$msg"
    fi
else
    echo "mermaid-kitty: SKIP APC/PNG checks (python3 not installed)"
fi

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
