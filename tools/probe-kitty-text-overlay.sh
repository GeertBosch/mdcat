#!/bin/sh
# probe-kitty-text-overlay.sh — how does a terminal stack TEXT and a Kitty IMAGE
# that occupy the same cells, in both paint orders?
#
# Why: the built-in Mermaid flowchart renderer (docs/plan-mermaid-flowcharts.md,
# ADR 0004) draws shapes and arrows as ONE transparent Kitty image and puts the
# labels in ordinary text cells over it. That only works if text stays visible
# and the image survives, whichever is painted first:
#   - direct output (mdcat) paints the image, then the text over it;
#   - a gmore repaint paints the text, then the image over it.
# The image uses z=-1 (below text) and is transparent wherever a label sits.
#
# The test image is 24x4 cells: a rounded box, two arrows, and two 8-cell label
# spans (row 1 cols 2-9, row 2 cols 13-20) that are guaranteed transparent. The
# text layer writes LABEL-A1 and LABEL-B2 into exactly those spans.
#
# One case per screen (the screen is cleared before each; press Enter to go on):
#   A. Text, then image: label rows, CUU, ESC7, image z=-1, ESC8 (gmore repaint).
#   B. Case A with z=0: does transparency alone keep the labels visible?
#   C. Image, then text: ESC7, image C=1 z=-1, ESC8, then the label rows written
#      with cursor moves (cells outside the labels are NOT written) (direct output).
#   D. Case C, but spaces are printed across the whole band. Do default-background
#      spaces hide or erase the image?
#   E. Cases A and C again, with LABEL-B2 on a non-default background (a code span).
#   F. Tall band: Case C with a 3-screen-tall image and band; scroll back after.
#   G. Does C=1 leave the cursor where it was? (no ESC7/ESC8)
#   H-L. Cases C-G again with z=0 instead of z=-1 (H=C, I=D, J=E, K=F, L=G). A
#      terminal that does not draw z=-1 images (VSCode) may still draw z=0 ones;
#      the image is then over the text, which only transparency keeps readable.
#
# How to read it: for each case compare the screen to the "you should see" line,
# and report OK or what went wrong (labels hidden / image erased / image shifted /
# cursor moved), plus the terminal name and whether it is over SSH. Screenshots
# are welcome. The results go in the table in docs/TERMINAL-GRAPHICS.md §10.
#
# Environment: PROBE_CASES="A C" runs only those cases (default "A B C D E F G H I J K L");
# PROBE_PAUSE=0 does not wait between cases; PROBE_CELL_W / PROBE_CELL_H override
# the measured cell size in pixels; PROBE_TALL_ROWS sets case F's band height.

dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$dir/probe-common.sh"
[ -t 1 ] || { echo "stdout is not a TTY; run this directly in a terminal" >&2; exit 1; }
command -v magick >/dev/null 2>&1 || { echo "ImageMagick (magick) not found in PATH" >&2; exit 1; }
command -v base64 >/dev/null 2>&1 || { echo "base64 not found in PATH" >&2; exit 1; }

COLS=24
ROWS=4
APC_END="${ESC}\\"
STROKE='#ff6a00'

tmpdir=$(mktemp -d 2>/dev/null || { mkdir /tmp/probe-overlay.$$ && echo /tmp/probe-overlay.$$; })
trap 'rm -rf "$tmpdir"' EXIT INT TERM

# --- cell size -----------------------------------------------------------------
CW=${PROBE_CELL_W:-}
CH=${PROBE_CELL_H:-}
if [ -z "$CW" ] || [ -z "$CH" ]; then
    r16=$(term_query '16t' t)
    cell=$(echo "$r16" | sed -n 's/^ESC\[*6;\([0-9]*\);\([0-9]*\)t.*/\1 \2/p')
    if [ -n "$cell" ]; then
        set -- $cell
        CH=$1; CW=$2
    else
        CW=9; CH=18
        echo "No reply to CSI 16t; assuming ${CW}x${CH} px cells (set PROBE_CELL_W/H)." >&2
    fi
fi
set -- $(stty size < /dev/tty 2>/dev/null)
SCREEN_ROWS=${1:-40}; SCREEN_COLS=${2:-80}
[ "$SCREEN_COLS" -ge "$COLS" ] || { echo "need at least $COLS columns" >&2; exit 1; }
SW=$((CW / 9)); [ "$SW" -ge 2 ] || SW=2

# --- test images ---------------------------------------------------------------
# Strokes are opaque; everything else, and every label span, is transparent.
make_box_png() {  # $1 = output file
    w=$((COLS * CW)); h=$((ROWS * CH))
    x0=$((CW / 2));  y0=$((CH / 2))
    x1=$((w - CW / 2)); y1=$((h - CH / 2))
    ya=$((CH + CH / 2));      # arrow 1 on row 1, from col 11 to col 22
    yb=$((2 * CH + CH / 2));  # arrow 2 on row 2, from col 1 to col 12
    hl=$CW; hw=$((CW * 2 / 5 + 1))
    xa0=$((11 * CW)); xa1=$((22 * CW + CW / 2))
    xb0=$CW;          xb1=$((12 * CW))
    magick -size "${w}x${h}" xc:none -fill none -stroke "$STROKE" -strokewidth "$SW" \
        -draw "roundrectangle $x0,$y0 $x1,$y1 $CW,$CW" \
        -draw "line $xa0,$ya $((xa1 - hl)),$ya" \
        -draw "line $xb0,$yb $((xb1 - hl)),$yb" \
        -fill "$STROKE" \
        -draw "polygon $xa1,$ya $((xa1 - hl)),$((ya - hw)) $((xa1 - hl)),$((ya + hw))" \
        -draw "polygon $xb1,$yb $((xb1 - hl)),$((yb - hw)) $((xb1 - hl)),$((yb + hw))" \
        PNG32:"$1"
}

# A tall band: the box outline plus a tick line on every 10th row that ends 1 cell
# before an 8-cell label span (cols 12-19), so alignment is visible along the band.
make_tall_png() {  # $1 = output file, $2 = band rows
    w=$((COLS * CW)); h=$(($2 * CH))
    ticks="$tmpdir/ticks.mvg"
    printf 'stroke %s\nstroke-width %s\n' "$STROKE" "$SW" > "$ticks"
    r=10
    while [ "$r" -lt $(($2 - 1)) ]; do
        y=$((r * CH + CH / 2))
        printf 'line %s,%s %s,%s\n' "$((2 * CW))" "$y" "$((11 * CW))" "$y" >> "$ticks"
        r=$((r + 10))
    done
    magick -size "${w}x${h}" xc:none -fill none -stroke "$STROKE" -strokewidth "$SW" \
        -draw "roundrectangle $((CW / 2)),$((CH / 2)) $((w - CW / 2)),$((h - CH / 2)) $CW,$CW" \
        -draw "@$ticks" PNG32:"$1"
}

# --- Kitty transmit ------------------------------------------------------------
# Send a PNG as one image: $1 = the first chunk's control keys, $2 = PNG file.
# Chunks are at most 4096 base64 bytes; every continuation chunk carries q=2 too.
# Each chunk is one printf, so it reaches the terminal contiguously.
kitty_send() {
    b64="$tmpdir/b64"
    base64 < "$2" | tr -d '\n' | fold -w 4096 > "$b64"
    n=$(grep -c '' "$b64")
    i=0
    while IFS= read -r chunk || [ -n "$chunk" ]; do
        i=$((i + 1))
        if [ "$i" -lt "$n" ]; then more=1; else more=0; fi
        if [ "$i" -eq 1 ]; then ctl="$1,m=$more"; else ctl="q=2,m=$more"; fi
        printf '%s_G%s;%s%s' "$ESC" "$ctl" "$chunk" "$APC_END"
    done < "$b64"
}

# --- text layers ---------------------------------------------------------------
# LBSGR, if set, is the SGR parameter string for LABEL-B2's background.
label_b() {
    if [ -n "$LBSGR" ]; then
        printf '%s%sm' "$CSI" "$LBSGR"
        printf 'LABEL-B2'
        printf '%s0m' "$CSI"
    else
        printf 'LABEL-B2'
    fi
}

# Label rows made only of the labels and spaces (what a gmore repaint writes).
text_rows_spaces() {
    printf '%24s\n' ''
    printf '  LABEL-A1%14s\n' ''
    printf '%13s' ''; label_b; printf '%3s\n' ''
    printf '%24s\n' ''
}

# Label rows that skip every other cell with cursor moves (what mdcat's direct
# output can do): cells outside the labels are not written at all.
text_rows_moves() {
    printf '\n'
    printf '%s2CLABEL-A1\n' "$CSI"
    printf '%s13C' "$CSI"; label_b; printf '\n'
    printf '\n'
}

# --- screen plumbing -----------------------------------------------------------
clear_screen() {
    printf '%s_Ga=d,d=A,q=2%s' "$ESC" "$APC_END"
    printf '%s2J%sH' "$CSI" "$CSI"
}

pause() {
    [ "${PROBE_PAUSE:-1}" = 0 ] && return 0
    printf 'Press Enter for the next case... '
    read _x < /dev/tty
}

start_case() {  # $1 = title, $2 = "you should see" line
    clear_screen
    printf 'CASE %s%s\n' "$1" "${ALIAS:+ (reported as case $ALIAS)}"
    printf 'You should see: %s\n\n' "$2"
}

GOOD="a rounded orange box with two arrows; LABEL-A1 and LABEL-B2 fully readable inside it, nothing struck through"

# --- the cases -----------------------------------------------------------------
ALIAS=
ZV=-1   # z-index for the image in cases C-G; H-L set it to 0

case_text_then_image() {  # $1 = z value
    text_rows_spaces
    printf '%s%sA' "$CSI" "$ROWS"
    printf '%s7' "$ESC"
    kitty_send "a=T,f=100,q=2,c=$COLS,r=$ROWS,z=$1" "$tmpdir/box.png"
    printf '%s8' "$ESC"
    printf '%s%sB\n' "$CSI" "$ROWS"
}

case_image_then_text() {  # $1 = "moves" or "spaces"
    printf '%s7' "$ESC"
    kitty_send "a=T,f=100,q=2,C=1,c=$COLS,r=$ROWS,z=$ZV" "$tmpdir/box.png"
    printf '%s8' "$ESC"
    if [ "$1" = moves ]; then text_rows_moves; else text_rows_spaces; fi
}

run_case() {
    case "$1" in
    H|I|J|K|L)
       case "$1" in H) m=C;; I) m=D;; J) m=E;; K) m=F;; L) m=G;; esac
       ZV=0; ALIAS=$1; run_case "$m"; ZV=-1; ALIAS=; return ;;
    A) start_case "A. text, then image (z=-1)" "$GOOD"
       LBSGR=; case_text_then_image -1 ;;
    B) start_case "B. text, then image (z=0)" "$GOOD. If the labels are hidden, transparency alone is not enough"
       LBSGR=; case_text_then_image 0 ;;
    C) start_case "C. image (C=1, z=$ZV), then text over it" "$GOOD. If the image vanished or the box is broken, writing text erased it"
       LBSGR=; case_image_then_text moves ;;
    D) start_case "D. image (z=$ZV), then spaces over the whole band" "$GOOD. If the image is gone, default-background spaces erase or hide it"
       LBSGR=; case_image_then_text spaces ;;
    E) start_case "E1. text, then image (z=$ZV); LABEL-B2 on a blue background" "$GOOD, with LABEL-B2 white on blue and the box strokes visible over the blue"
       LBSGR='44;97'; case_text_then_image $ZV
       printf '\n'
       printf 'CASE E2. image (z=%s), then text; LABEL-B2 on a blue background (same expectation)\n\n' "$ZV"
       case_image_then_text moves ;;
    F) rows=${PROBE_TALL_ROWS:-$((SCREEN_ROWS * 3))}
       start_case "F. tall band: $rows rows, image (z=$ZV) first, then text" "an orange outline $rows rows tall; every 10th row has an orange tick ending one cell before its 'ROW nnnn' label. Scroll back after it finishes: ticks and labels must stay aligned from top to bottom, and the box must not be cut"
       make_tall_png "$tmpdir/tall.png" "$rows"
       printf '%s7' "$ESC"
       kitty_send "a=T,f=100,q=2,C=1,c=$COLS,r=$rows,z=$ZV" "$tmpdir/tall.png"
       printf '%s8' "$ESC"
       r=0
       while [ "$r" -lt "$rows" ]; do
           if [ $((r % 10)) -eq 0 ] && [ "$r" -gt 0 ]; then
               printf '%s12CROW %04d\n' "$CSI" "$r"
           else
               printf '\n'
           fi
           r=$((r + 1))
       done ;;
    G) start_case "G. does C=1 keep the cursor still (z=$ZV)?" "'[' and ']' directly next to each other on one line, '<- cursor' after them, with the orange box's left edge starting under the ']' (the image is anchored where the cursor was, one cell in, and the cursor did not move); then a line 'AFTER' below the box"
       printf '['
       kitty_send "a=T,f=100,q=2,C=1,c=$COLS,r=$ROWS,z=$ZV" "$tmpdir/box.png"
       printf ']<- cursor\n\n\n\n\n\nAFTER\n' ;;
    *) echo "unknown case $1" >&2; return 1 ;;
    esac
    printf '\n'
}

# --- main ----------------------------------------------------------------------
banner "0. Context (include this in your report)"
printf 'TERM=[%s] TERM_PROGRAM=[%s] SSH_CONNECTION=[%s]\n' \
       "${TERM:-}" "${TERM_PROGRAM:-}" "${SSH_CONNECTION:-}"
printf 'cell=%sx%s px, screen=%sx%s cells, band=%sx%s cells\n' \
       "$CW" "$CH" "$SCREEN_COLS" "$SCREEN_ROWS" "$COLS" "$ROWS"

make_box_png "$tmpdir/box.png" || { echo "magick failed" >&2; exit 1; }
pause

for c in ${PROBE_CASES:-A B C D E F G H I J K L}; do
    run_case "$c" || continue
    pause
done

banner "DONE"
echo "Report, per case A-G: OK, or what went wrong (labels hidden / image erased /"
echo "image shifted / cursor moved), plus the terminal name and whether it was over SSH."
echo "Record the results in docs/TERMINAL-GRAPHICS.md §10 'Text over Kitty images'."
