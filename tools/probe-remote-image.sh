#!/bin/sh
# probe-remote-image.sh — can timg fetch a remote image URL, or must mdcat?
#
# Why: mdcat renders a remote image (e.g. the README's CI badge) by handing the
# source to timg. The open question was whether timg can fetch an https URL
# itself (it links libav, which speaks http) or whether mdcat must fetch the
# bytes first. This probe answers it for the LOCAL timg build, so we know which
# path the code must take.
#
# Findings on the build measured (timg 1.6.3+, ffmpeg/libav 62, GraphicsMagick
# image path):
#   H1: timg <https-url> on the IMAGE path -> "No such file or directory"; the
#       URL is treated as a local file. FETCH FAILS.
#   H2: timg -V <https-url> (force video subsystem) -> still no output for a
#       static SVG/PNG. FETCH FAILS.
#   H3: curl the URL to a temp file, then timg <file> -> renders. WORKS.
# Conclusion: mdcat fetches with curl itself, then feeds timg a local path. See
# docs/TERMINAL-GRAPHICS.md §7.
#
# This probe needs network and the GitHub badge URL; it prints PASS/FAIL per
# hypothesis. It does not need a TTY (it inspects byte counts, not pixels).
#
# Usage: tools/probe-remote-image.sh [url]

set -u

url=${1:-'https://github.com/GeertBosch/mdcat/actions/workflows/ci.yml/badge.svg?branch=main'}

banner() { printf '\n===== %s =====\n' "$1"; }

if ! command -v timg >/dev/null 2>&1; then
    echo "timg not installed; cannot probe" >&2
    exit 2
fi
if ! command -v curl >/dev/null 2>&1; then
    echo "curl not installed; cannot probe" >&2
    exit 2
fi

banner "Context"
timg --version 2>&1 | head -1
printf 'url: %s\n' "$url"

banner "H1: timg <url> on the image path (expect: fails, treated as local file)"
out=$(timg -ps -g80x25 "$url" </dev/null 2>&1)
n=$(printf '%s' "$out" | wc -c | tr -d ' ')
printf 'stderr/stdout (first line): %s\n' "$(printf '%s' "$out" | head -1)"
case "$out" in
    *"No such file"*|"") echo "PASS: timg did not fetch the URL ($n bytes)";;
    *) echo "NOTE: timg produced $n bytes — this build may fetch URLs on the image path";;
esac

banner "H2: timg -V <url> (force video subsystem)"
nv=$(timg -ps -g80x25 -V "$url" </dev/null 2>/dev/null | wc -c | tr -d ' ')
if [ "$nv" -gt 64 ]; then
    echo "NOTE: -V produced $nv bytes — video subsystem CAN fetch this URL"
else
    echo "PASS: -V produced $nv bytes — no usable fetch via video subsystem"
fi

banner "H3: curl -> temp file -> timg <file> (expect: renders)"
tmp=$(mktemp).svg
trap 'rm -f "$tmp"' EXIT
if curl -fsSL --proto '=https' --max-time 15 --max-filesize 16000000 -o "$tmp" "$url"; then
    cb=$(wc -c < "$tmp" | tr -d ' ')
    rb=$(timg -pk -g80x25 "$tmp" </dev/null 2>/dev/null | wc -c | tr -d ' ')
    printf 'curl got %s bytes; timg -pk emitted %s bytes\n' "$cb" "$rb"
    if [ "$rb" -gt 64 ]; then
        echo "PASS: curl->timg renders the remote image"
    else
        echo "FAIL: timg produced no output from the fetched file"
    fi
else
    echo "FAIL: curl could not fetch the URL (network?)"
fi
