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

banner "0. Context (include this in your screenshot)"
printf 'TERM=[%s] TERM_PROGRAM=[%s] KITTY_WINDOW_ID=[%s]\n' \
       "${TERM:-}" "${TERM_PROGRAM:-}" "${KITTY_WINDOW_ID:-}"
printf 'SSH_CONNECTION=[%s] SSH_TTY=[%s] MDCAT_GRAPHICS=[%s]\n' \
       "${SSH_CONNECTION:-}" "${SSH_TTY:-}" "${MDCAT_GRAPHICS:-}"

banner "1. Kitty capability query (a=q)"
echo "Sending the canonical minimal query (1x1 RGB, a=q); a Kitty-capable terminal"
echo "replies ESC_G i=1;OK ESC\\. NOTE: silence does NOT mean unsupported (ADR 0002)"
echo "-- some terminals/links don't answer; sections 2-3 are the real capability test."
# Canonical Kitty query from the spec: a 1x1 RGB pixel (f=24,s=1,v=1) as base64,
# action a=q (validate + reply, don't draw). The base64 'AAAA' is 3 zero bytes =
# one black pixel; using a fixed constant avoids fragile shell binary generation.
printf '%si=1,a=q,f=24,s=1,v=1;AAAA%s' "$APC_START" "$APC_END" > /dev/tty
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

# Inject one or more control keys into a timg -pk APC: insert "<keys>," right
# after the "ESC_G" so they precede timg's own a=T,... controls (timg does NOT
# emit c=/r= itself, so there's no conflict). Kitty reads the whole comma list up
# to ';'. Done with awk on the raw bytes (binary-safe: the base64 body has no
# ESC/NUL). The trailing newline is preserved with ORS. This is exactly how mdcat
# will add a cell footprint to timg's PNG.
inject_keys() {  # $1=png  $2=keys e.g. "c=10,r=4"
    $TIMG_PK "$1" 2>/dev/null | LC_ALL=C awk -v k="$2" 'BEGIN{ORS=""}
        { n=index($0,"\033_G");
          if (n>0) { printf "%s\033_G%s,%s", substr($0,1,n-1), k, substr($0,n+3) }
          else printf "%s", $0 }'
}

banner "2. Display WITH a cell footprint (c=COLS,r=ROWS) — what mdcat will emit"
COLS=10; ROWS=4
echo "timg's PNG WITH c=${COLS},r=${ROWS} injected (the real mdcat path). You should"
echo "see the source image ($PROBE_PNG) rendered correctly at ~${COLS}x${ROWS} CELLS:"
inject_keys "$PROBE_PNG" "c=${COLS},r=${ROWS}"
printf '\n'

banner "3. Display WITHOUT a footprint — EXPECTED to look wrong (control case)"
echo "The SAME timg PNG with NO c=/r= (bare 'timg -pk'). Some terminals (e.g."
echo "VSCode) mis-render a footprint-less PNG as a distorted/grey block. If THIS"
echo "looks wrong but #2 looks right, that's the expected result: it shows mdcat"
echo "MUST always inject c=/r= (timg ignores -g and never sets them itself):"
$TIMG_PK "$PROBE_PNG" 2>/dev/null
printf '\n'

banner "DONE"
echo "Report: query reply (;OK?), #2 correct at ${COLS}x${ROWS} cells?, #3 distorted?,"
echo "terminal name. (Env context is printed in section 0 above.)"
