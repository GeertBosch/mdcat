#!/bin/sh
# Property: gmore must re-emit every OSC 8 hyperlink it parses — the rendered
# output should contain exactly as many link-OPEN sequences (ESC]8;;<uri>ESC\,
# non-empty uri) as the input does. A regression once dropped links entirely in
# the interactive render path (see the commit "stop dropping OSC 8 hyperlinks").
#
# IMPORTANT: gmore only RENDERS when it thinks it is writing to a terminal;
# otherwise (stdout is a pipe/file) it passes the input through verbatim, so a
# naive `gmore file | grep` would just echo the input and prove nothing. Setting
# GMORE_KEYS forces the interactive render path even off-tty (that is its purpose
# — scripting a session for capture), so this test exercises the real renderer.
#
# Usage: tests/gmore-links.sh [gmore-binary]
# Exit status: 0 if all cases pass, 1 otherwise.

set -u

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
gmore=${1:-$root/gmore}

if [ ! -x "$gmore" ]; then
    echo "gmore-links: $gmore is not an executable; run 'make' first" >&2
    exit 2
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fails=0
n=0

# Count OSC 8 link-OPEN sequences: ESC ] 8 ; ; <non-empty uri> ESC. The link
# CLOSE (ESC]8;;ESC\, empty uri) is excluded by requiring a non-empty run before
# the terminating ESC, so this counts distinct hyperlinks, not open+close pairs.
count_links() { grep -aoE "$(printf '\033]8;;[^\033]+\033')"; }

# gen N -> N lines, each carrying one OSC 8 hyperlink, on stdout.
gen() {
    python3 -c '
import sys
n = int(sys.argv[1])
for i in range(n):
    print(f"item {i}: \033]8;;https://example.com/{i}\033\\link{i}\033]8;;\033\\ done")
' "$1"
}

# check LABEL N KEYS — render an N-link file through gmore with KEYS scripted,
# pressing keys that page through the WHOLE document so every link is painted at
# least once, then assert the rendered output has >= N link-opens (paging may
# repaint a row, so duplicates are fine; dropping any is the failure we catch).
check() {
    label=$1; nlinks=$2; keys=$3
    gen "$nlinks" > "$tmp/in"
    want=$(count_links < "$tmp/in" | wc -l | tr -d ' ')
    got=$(GMORE_KEYS="$keys" LINES=24 COLUMNS=80 "$gmore" "$tmp/in" 2>"$tmp/err" \
          | count_links | wc -l | tr -d ' ')
    if [ "$want" = "$nlinks" ] && [ "$got" -ge "$want" ]; then
        n=$((n + 1))
    else
        echo "gmore-links: FAIL [$label] keys='$keys'" >&2
        echo "  input links:    $want (expected $nlinks)" >&2
        echo "  rendered links: $got (expected >= $want)" >&2
        [ -s "$tmp/err" ] && { echo "  stderr:" >&2; sed 's/^/    /' "$tmp/err" >&2; }
        fails=$((fails + 1))
    fi
}

# Single screen (5 links, all on the first page): immediate quit, no paging.
check "single-screen"   5  'q'
# Exactly one full page (23 lines for LINES=24): first paint only.
check "one-full-page"   23 'q'
# Multi-page: page forward to the end (f f f covers >50 lines), then quit. Every
# link must appear as the view scrolls past it.
check "page-to-end"     50 'fffq'
# Line-stepping through a long doc also reveals every link.
check "line-step"       30 '20j20jq'

if [ "$fails" -eq 0 ]; then
    echo "gmore-links: OK ($n cases)"
    exit 0
fi
echo "gmore-links: FAIL ($fails of $((n + fails)) cases)" >&2
exit 1
