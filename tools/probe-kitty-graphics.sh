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

# Emit a timg -pk APC for $1, forcing a UNIQUE image id ($2) and optionally
# prepending control keys ($3, e.g. "c=10,r=4,"). Two things make this correct:
#   * UNIQUE i= per call. timg assigns the SAME i= to every render of a given file,
#     so two displays in one run collide: a later a=T with the same id REPLACES the
#     image and re-lays earlier placements, corrupting the first display (observed:
#     section 2 turned into a grey block once section 3 re-sent the same id). We
#     rewrite i= so each section is an independent image.
#   * BYTE-EXACT edit. The payload is binary (APC + base64 PNG) with a newline
#     between the APC terminator (ESC \) and a trailing ESC[?25h; line tools
#     (awk/sed) mangle it. `perl -0777` slurps the whole stream as one record.
emit_kitty() {  # $1=png  $2=unique-id  $3=prepend-keys (may be empty)
    $TIMG_PK "$1" 2>/dev/null | perl -0777 -pe "s/i=\\d+/i=$2/; s/\\x1b_G/\\x1b_G$3/"
}

banner "2. Display WITH a cell footprint (c=COLS,r=ROWS) — what mdcat will emit"
COLS=10; ROWS=4
echo "timg's PNG WITH c=${COLS},r=${ROWS} injected (the real mdcat path), via a"
echo "byte-exact perl splice, as a unique image id. You should see the source image"
echo "($PROBE_PNG) rendered correctly, scaled to ~${COLS}x${ROWS} CELLS:"
emit_kitty "$PROBE_PNG" 9001 "c=${COLS},r=${ROWS},"
printf '\n'

banner "3. Display at native size (no footprint) — reference, distinct image id"
echo "The same image with NO c=/r= injected, as a DIFFERENT image id (so it does"
echo "not disturb #2). Renders at timg's native pixel size; compare #2's cell"
echo "scaling against it. Both should be a recognizable image, different sizes:"
emit_kitty "$PROBE_PNG" 9002 ""
printf '\n'

banner "DONE"
echo "Report: query reply (;OK?), is #2 a correct image at ~${COLS}x${ROWS} cells?,"
echo "is #3 a correct image at native size?, terminal name. (Env in section 0.)"
