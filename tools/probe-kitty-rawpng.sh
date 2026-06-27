#!/bin/sh
# probe-kitty-rawpng.sh — can mdcat wrap a SOURCE png in a Kitty APC, no timg?
#
# Why: the implementation plan (ADR 0002) drops timg from the PNG path — mdcat will
# read a .png file and emit it straight as a Kitty image (a=T,f=100 + base64 PNG),
# letting the terminal decode and the c=/r= keys scale it. This must work for real
# source PNGs that timg never touched: arbitrary color types (grayscale/RGB/RGBA)
# and payloads large enough to need CHUNKING (Kitty caps a chunk at 4096 base64
# bytes: m=1 on every chunk but the last, m=0 on the last). This probe builds that
# exact wrap in awk/python and displays a few real PNGs to prove it renders.
#
# How to read it: screenshot. Each labeled PNG should appear, scaled to its c=/r=
# box. If they do, PNG needs no timg and the terminal handles all color types +
# chunking. + terminal + SSH.

dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$dir/probe-common.sh"
[ -t 1 ] || { echo "stdout is not a TTY; run this directly in a terminal" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 needed for this probe" >&2; exit 1; }

banner "0. Context"
printf 'TERM=[%s] TERM_PROGRAM=[%s] SSH_CONNECTION=[%s]\n' \
       "${TERM:-}" "${TERM_PROGRAM:-}" "${SSH_CONNECTION:-}"

# Emit a Kitty image straight from a source PNG file, no timg. Reads w/h from the
# IHDR, base64s the bytes, chunks at 4096, sets c=/r= to the requested cell box.
# This mirrors exactly what mdcat's C++ Kitty encoder will do.
# Aspect-preserving cell box. Cells are NOT square (we measured ~7x16 / ~10x21 px),
# so c/r must come from imageW/cellW and imageH/cellH, then scaled uniformly to the
# column budget — NOT hardcoded. Hardcoding a wrong c:r ratio stretches the image
# (a square renders as a rectangle). $3 = max columns; cell px read from CSI 16t,
# default 7x16. This mirrors mdcat's cellMetrics footprint math.
CELL=$(term_query '16t' t)         # ESC[6;H;W t  (or "ESCq6;H;Wt" on dash dd-path)
CELLH=$(printf '%s' "$CELL" | sed -n 's/.*6;\([0-9]*\);\([0-9]*\)t.*/\1/p')
CELLW=$(printf '%s' "$CELL" | sed -n 's/.*6;\([0-9]*\);\([0-9]*\)t.*/\2/p')
[ -n "$CELLW" ] || CELLW=7
[ -n "$CELLH" ] || CELLH=16

wrap_png() {  # $1=png-path  $2=id  $3=maxCols
    python3 - "$1" "$2" "$3" "$CELLW" "$CELLH" <<'PY'
import sys, base64, struct
path, iid, maxcols, cw, ch = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
d = open(path, 'rb').read()
if d[:8] != b'\x89PNG\r\n\x1a\n':
    sys.stderr.write(f"{path}: not a PNG\n"); sys.exit(0)
w, h = struct.unpack('>II', d[16:24])
# natural footprint at 1:1 px; scale uniformly to fit maxcols, preserving aspect
natCols, natRows = w/cw, h/ch
if natCols > maxcols:
    s = maxcols/natCols; natCols *= s; natRows *= s
cols, rows = max(1, round(natCols)), max(1, round(natRows))
b64 = base64.b64encode(d).decode()
ESC = '\x1b'
# Chunk the base64 at 4096 bytes. First chunk carries all the controls; subsequent
# chunks carry only m=. m=1 means "more follows", m=0 means "last".
CH = 4096
chunks = [b64[i:i+CH] for i in range(0, len(b64), CH)]
out = []
for n, ch in enumerate(chunks):
    first = (n == 0)
    last  = (n == len(chunks)-1)
    m = 0 if last else 1
    # q=2 on EVERY chunk, not just the first: a chunked continuation (m=) also
    # replies ;OK unless suppressed, so the final m=0 chunk leaks an OK otherwise.
    if first:
        ctrl = f"a=T,f=100,q=2,i={iid},c={cols},r={rows},m={m}"
    else:
        ctrl = f"q=2,m={m}"
    out.append(f"{ESC}_G{ctrl};{ch}{ESC}\\")
sys.stdout.write("".join(out))
sys.stdout.flush()
sys.stderr.write(f"{path}: {w}x{h}, {len(d)} bytes, {len(chunks)} chunk(s)\n")
PY
}

# Pick a few real PNGs spanning color types if available.
GRAY=tests/chess-piece.png                  # colortype 0 (grayscale), small, 1 chunk
RGBA=$(ls tests/img/example.png 2>/dev/null) # colortype 6 (RGBA), medium
RGB=README.md-1.png                          # colortype 2 (RGB), large -> multi-chunk

echo "(measured cell ${CELLW}x${CELLH} px; c/r computed to PRESERVE ASPECT RATIO)"

banner "1. Grayscale PNG (colortype 0, 1 chunk): $GRAY"
[ -f "$GRAY" ] && { wrap_png "$GRAY" 8001 12; printf '\n'; } || echo "missing"

banner "2. RGBA PNG (colortype 6): ${RGBA:-<none>}"
[ -n "$RGBA" ] && [ -f "$RGBA" ] && { wrap_png "$RGBA" 8002 16; printf '\n'; } || echo "missing — skip"

banner "3. Large RGB PNG (colortype 2, MULTI-CHUNK): $RGB"
[ -f "$RGB" ] && { wrap_png "$RGB" 8003 24; printf '\n'; } || echo "missing — skip"

banner "DONE"
echo "Report: did each PNG render UNDISTORTED (circle round, square square) at its"
echo "cell box? (stderr lists size/chunks.) If yes, the PNG path needs no timg, the"
echo "terminal handles color types + chunking, and aspect ratio is preserved."
