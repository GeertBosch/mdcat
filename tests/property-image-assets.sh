#!/bin/sh
# Property: every local image referenced by tests/*.md resolves to a file on disk.
#
# A fixture that references a missing image doesn't fail loudly: mdcat falls back
# to rendering the filename as plain text (see the fallback path in
# renderImageBlock), so a hand-checked test can silently stop exercising image
# rendering at all and nobody notices until someone happens to look closely at the
# output. This caught tests/images-narrow-col.md losing its backing PNGs when an
# unrelated commit trimmed tests/img/ for the README without checking who else
# depended on those files.
#
# Checks both Markdown (`![alt](src)`) and HTML (`<img src="...">`) image
# references, resolving relative paths against the .md file's own directory (the
# same resolution mdcat itself uses). Remote (http/https) sources are skipped, and
# so is the literal basename "missing.png" -- tests/images.md's documented
# convention for a file left absent on purpose to exercise mdcat's fallback path.
#
# Usage: tests/property-image-assets.sh [file ...]
# Defaults: every tests/*.md in sorted order.
# Exit status: 0 if every reference resolves, 1 otherwise (printing the misses).

set -eu

here=$(CDPATH= cd "$(dirname "$0")" && pwd)

if [ "$#" -gt 0 ]; then
    set -- "$@"
else
    set -- "$here"/*.md
fi

fails=0
n=0

for f in "$@"; do
    dir=$(dirname "$f")
    # Strip backtick-delimited inline code spans first: docs like images-leading.md
    # show `![Logo](logo.png)` as a literal example of the syntax being described,
    # not a real reference to render.
    stripped=$(sed -E 's/`[^`]*`//g' "$f")
    # Markdown images: ![alt](src) -- src is the first token up to whitespace or ')'.
    # HTML images: <img ... src="...">.
    srcs=$( { printf '%s\n' "$stripped" | grep -oE '!\[[^]]*\]\([^) ]+' | sed -E 's/.*\(//'
              printf '%s\n' "$stripped" | grep -oE '<img[^>]*src="[^"]*"' | sed -E 's/.*src="([^"]*)".*/\1/'; } || true)
    [ -z "$srcs" ] && continue
    old_ifs=$IFS
    IFS='
'
    for src in $srcs; do
        IFS=$old_ifs
        [ -z "$src" ] && continue
        case "$src" in
            http://*|https://*) continue ;;
        esac
        [ "$(basename "$src")" = "missing.png" ] && continue
        n=$((n + 1))
        case "$src" in
            /*) path=$src ;;
            *) path="$dir/$src" ;;
        esac
        if [ ! -f "$path" ]; then
            echo "property-image-assets: FAIL $f references missing image: $src (resolved: $path)" >&2
            fails=$((fails + 1))
        fi
        IFS='
'
    done
    IFS=$old_ifs
done

if [ "$fails" -eq 0 ]; then
    echo "property-image-assets: OK ($n references checked)"
    exit 0
fi
echo "property-image-assets: FAIL ($fails of $n references missing)" >&2
exit 1
