#!/bin/sh
# probe-kitty-graphics.sh — does this terminal speak the Kitty graphics protocol,
# and can it size an image in CELLS (so a remote host needn't know cell pixels)?
#
# Why: over SSH the remote host can't read the local terminal's cell pixel size
# (TIOCGWINSZ ws_xpixel/ypixel is 0). The Kitty protocol lets us send pixel data
# plus an explicit cell footprint (c=cols, r=rows), so the LOCAL terminal scales
# the image to fit and we never need the remote px size. This probe proves (or
# disproves) the assumptions mdcat's remote-graphics plan (ADR 0002) depends on.
#
# METHOD: we drive everything off `timg -pk`, which already emits a working Kitty
# APC ( ESC _ G a=T,...,f=100,m=...; <base64 PNG> ESC \ ). That is the EXACT path
# mdcat will use, so the probe tests reality, not a hand-rolled payload. For the
# cell-sizing test we take timg's APC and INJECT c=/r= into its control keys —
# proving mdcat can size a passed-through PNG purely in cells.
#
# Checks:
#   1. CAPABILITY QUERY (a=q): a supporting terminal replies ESC _ G i=..;OK ESC \.
#      Silence = no support OR slow link (can't tell which — see ADR 0002).
#   2. BASIC DISPLAY: timg -pk as-is. You should SEE the source image.
#   3. CELL-BASED SIZING: same timg PNG, but with c=COLS,r=ROWS injected. You
#      should see it scaled to ~COLSxROWS CELLS regardless of its pixels. THIS is
#      the property that makes remote layout possible (ADR 0002 validation gate).
#
# How to read it: screenshot it. Report (a) query reply contained ";OK"? (b) did
# image #2 appear? (c) did image #3 occupy ~COLSxROWS cells? + terminal + SSH?.

dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$dir/probe-common.sh"
[ -t 1 ] || { echo "stdout is not a TTY; run this directly in a terminal" >&2; exit 1; }
command -v timg >/dev/null 2>&1 || { echo "timg not found in PATH" >&2; exit 1; }
[ -f "$PROBE_PNG" ] || { echo "PROBE_PNG not found: $PROBE_PNG" >&2; exit 1; }

APC_START="${ESC}_G"
APC_END="${ESC}\\"

# timg -pk needs an explicit -g geometry unless it can read the terminal size; we
# always pass one (as mdcat will) so the probe is robust on any stdout. The
# terminal still does the final cell fit, and for test #3 we override with c=/r=.
TIMG_PK="timg -pk -g24x24"

banner "1. Kitty capability query (a=q)"
echo "Sending a transmit+query; a Kitty-capable terminal replies with an APC string."
# Use timg to make a real PNG payload, then turn its 'a=T' into 'a=q' (query only:
# the terminal validates + replies ;OK/;E without drawing). This reuses a payload
# the terminal is known to accept rather than hand-building one.
qtmp=$(mktemp 2>/dev/null || echo /tmp/probe-kq.$$)
$TIMG_PK "$PROBE_PNG" > "$qtmp" 2>/dev/null
# Rewrite the first control segment: a=T -> a=q. (Controls are between ESC_G and ';'.)
LC_ALL=C sed 's/a=T/a=q/' "$qtmp" > /dev/tty
rm -f "$qtmp"
reply=""
to=2
while IFS= read -r -s -t "$to" -n 1 c < /dev/tty 2>/dev/null; do
    to=1
    case "$c" in
        "$ESC") reply="${reply}ESC" ;;
        "") ;;
        *) reply="${reply}${c}"; case "$reply" in *ESC'\') break;; esac ;;
    esac
done
printf '\nRAW QUERY REPLY: [%s]\n' "$reply"
case "$reply" in
    *';OK'*) echo "=> Contains ;OK  -> Kitty graphics SUPPORTED." ;;
    *ESC_G*) echo "=> Got an APC reply (see above) -> terminal answered the query." ;;
    "")      echo "=> NO REPLY. Either unsupported, or the link/terminal was too slow." ;;
    *)       echo "=> Unexpected reply; inspect the raw bytes above." ;;
esac

banner "2. Basic display (timg -pk, as mdcat would emit it)"
echo "You should see the source image ($PROBE_PNG) below:"
$TIMG_PK "$PROBE_PNG" 2>/dev/null
printf '\n'

# Inject one or more control keys into a timg -pk APC: insert "<keys>," right
# after the "ESC_G" so they precede timg's own a=T,... controls (timg does NOT
# emit c=/r= itself, so there's no conflict). Kitty reads the whole comma list up
# to ';'. Done with awk on the raw bytes (binary-safe: the base64 body has no
# ESC/NUL). This is exactly how mdcat will add a cell footprint to timg's PNG.
inject_keys() {  # $1=png  $2=keys e.g. "c=10,r=4"
    $TIMG_PK "$1" 2>/dev/null | LC_ALL=C awk -v k="$2" '
        { n=index($0,"\033_G");
          if (n>0) { printf "%s\033_G%s,%s", substr($0,1,n-1), k, substr($0,n+3) }
          else printf "%s", $0 }'
}

banner "3. Cell-based sizing (c=COLS,r=ROWS) — the remote-layout property"
COLS=10; ROWS=4
echo "Same timg PNG, but with c=${COLS},r=${ROWS} injected. You should see it"
echo "scaled to ~${COLS}x${ROWS} CELLS (NOT its native pixel size). If so, remote"
echo "sizing works and ADR 0002's core premise holds:"
inject_keys "$PROBE_PNG" "c=${COLS},r=${ROWS}"
printf '\n'

banner "DONE"
echo "Report: query reply (;OK?), image #2 visible?, image #3 size in cells?,"
echo "your terminal name, and whether this ran over SSH (\$SSH_CONNECTION=${SSH_CONNECTION:-unset})."
