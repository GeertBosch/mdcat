#!/bin/sh
# probe-kitty-graphics.sh — does this terminal speak the Kitty graphics protocol,
# and can it size an image in CELLS (so a remote host needn't know cell pixels)?
#
# Why: over SSH the remote host can't read the local terminal's cell pixel size
# (TIOCGWINSZ ws_xpixel/ypixel is 0). The Kitty protocol lets us send pixel data
# plus an explicit cell footprint (c=cols, r=rows), so the LOCAL terminal scales
# the image to fit and we never need the remote px size. This probe proves (or
# disproves) three things mdcat's remote-graphics plan depends on:
#
#   1. CAPABILITY QUERY: send a Kitty "query" graphic (a=q). A supporting
#      terminal replies with an APC string  ESC _ G i=<id>;OK ESC \  (or ;E...).
#      Silence = no support OR slow link (can't tell which — see the plan).
#   2. BASIC TRANSMIT+DISPLAY: transmit a tiny RGB image inline (f=24, direct
#      base64 RGB, one chunk) and display it (a=T). You should SEE a colored
#      square. This is the minimal "can it draw at all" check.
#   3. CELL-BASED SIZING: display the SAME image forced into a known cell box
#      (c=COLS, r=ROWS). You should see it scaled to exactly that many cells,
#      independent of the image's pixel size. THIS is the property that makes
#      remote layout possible.
#
# How to read it: the script prints the raw query reply (ESC shown literally),
# then draws two squares. Take a screenshot. Report: (a) did the query reply
# contain ";OK"? (b) did square #2 appear? (c) did square #3 occupy ~COLSxROWS
# cells regardless of its pixels? Also note your terminal + whether over SSH.

dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$dir/probe-common.sh"
[ -t 1 ] || { echo "stdout is not a TTY; run this directly in a terminal" >&2; exit 1; }

APC_START="${ESC}_G"
APC_END="${ESC}\\"

# Build a base64 payload of a solid WxH RGB image (f=24 = 24-bit RGB, no alpha).
# Colors chosen bright so the square is obvious. Pure shell + printf + base64.
solid_rgb_b64() {  # $1=w $2=h $3=R $4=G $5=B
    _w=$1; _h=$2; _r=$3; _g=$4; _b=$5
    _n=$(( _w * _h ))
    # Emit _n RGB triplets as raw bytes, then base64 (no line wrapping).
    {
        _i=0
        while [ "$_i" -lt "$_n" ]; do
            printf '\\%03o\\%03o\\%03o' "$_r" "$_g" "$_b"
            _i=$(( _i + 1 ))
        done
    } | while IFS= read -r line; do printf "$line"; done | base64 | tr -d '\n'
}

banner "1. Kitty capability query (a=q)"
echo "Sending a 1x1 query graphic; a Kitty-capable terminal replies with an APC string."
# Transmit a 1x1 RGB pixel as a query (a=q): terminal should respond OK/E without drawing.
q_b64=$(solid_rgb_b64 1 1 255 0 0)
printf '%s' "${APC_START}i=31,a=q,f=24,s=1,v=1,t=d;${q_b64}${APC_END}" > /dev/tty
# Read the reply from /dev/tty (APC ... ST). Show ESC literally.
reply=""
to=2
while IFS= read -r -s -t "$to" -n 1 c < /dev/tty 2>/dev/null; do
    to=1
    case "$c" in
        "$ESC") reply="${reply}ESC" ;;
        "") ;;
        '\') reply="${reply}\\"; case "$reply" in *ESC'\') break;; esac ;;
        *) reply="${reply}${c}" ;;
    esac
done
printf '\nRAW QUERY REPLY: [%s]\n' "$reply"
case "$reply" in
    *';OK'*) echo "=> Contains ;OK  -> Kitty graphics SUPPORTED." ;;
    *ESC_G*) echo "=> Got an APC reply (see above) -> terminal answered the query." ;;
    "")      echo "=> NO REPLY. Either unsupported, or the link/terminal was too slow." ;;
    *)       echo "=> Unexpected reply; inspect the raw bytes above." ;;
esac

banner "2. Basic transmit + display (a=T, native pixels)"
echo "You should see a 32x32 GREEN square below:"
g_b64=$(solid_rgb_b64 32 32 0 200 0)
printf '%s' "${APC_START}i=32,a=T,f=24,s=32,v=32,t=d;${g_b64}${APC_END}"
printf '\n'

banner "3. Cell-based sizing (c=COLS,r=ROWS) — the remote-layout property"
COLS=10; ROWS=4
echo "You should see a 16x16 BLUE source image scaled to ~${COLS}x${ROWS} CELLS"
echo "(NOT 16x16 px). If it fills ${COLS}x${ROWS} cells, remote sizing works:"
b_b64=$(solid_rgb_b64 16 16 0 80 255)
printf '%s' "${APC_START}i=33,a=T,f=24,s=16,v=16,c=${COLS},r=${ROWS},t=d;${b_b64}${APC_END}"
printf '\n'

banner "DONE"
echo "Report: query reply (;OK?), square #2 visible?, square #3 size in cells?,"
echo "your terminal name, and whether this ran over SSH (\$SSH_CONNECTION=${SSH_CONNECTION:-unset})."
