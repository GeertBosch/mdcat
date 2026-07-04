#!/bin/sh
# Property: the pager's private source-line provenance marker never leaks into non-tty
# output.
#
# The integrated gmore pager restores the reading position after a window resize by
# tagging each rendered block with the markdown source line it came from, via a private
# OSC sequence:  ESC ] 9999 ; <n> ST  (see gEmitProvenance in mdcat.cpp, srcAtRow in
# gmore_emulator.cpp). That marker is emitted ONLY on the paging path (rendering into the
# pipe that feeds gmore, where gmore silently consumes it), gated by gEmitProvenance which
# is set solely on the pager render thread. When stdout is not a tty mdcat never pages, so
# the marker must never appear in ordinary redirected output, or it would corrupt
# concatenated output and break the golden --dump and property-concat guarantees.
#
# So: render every sample document to non-tty stdout, both at the default width and with an
# explicit --width, and assert the marker's introducer (ESC ] 9 9 9 9 ;) never occurs.
#
# Usage: tests/property-provenance.sh [mdcat-binary] [file ...]
# Defaults: the ./mdcat next to the repo root, run over tests/*.md in sorted order.
# Exit status: 0 if no marker appears on any non-tty path, 1 otherwise.

set -eu

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")

mdcat=${1:-$root/mdcat}
if [ "$#" -gt 0 ]; then shift; fi

if [ "$#" -gt 0 ]; then
    set -- "$@"
else
    set -- "$here"/*.md
fi

if [ ! -x "$mdcat" ]; then
    echo "property-provenance: $mdcat is not an executable; run 'make' first" >&2
    exit 2
fi

# The marker introducer as a literal ESC byte + "]9999;". grep -a treats the output as
# text (it contains binary escape/sixel bytes) so a match isn't silently skipped.
esc=$(printf '\033')
needle="$esc]9999;"

status=0
n=0
for f in "$@"; do
    n=$((n + 1))
    # Non-tty stdout at the default width and at an explicit width. Neither pages, so
    # neither may emit the marker.
    for mode in "" "--width 40"; do
        if "$mdcat" $mode "$f" 2>/dev/null | grep -aq "$needle"; then
            echo "property-provenance: FAIL - provenance marker leaked into" \
                 "'mdcat ${mode:-<plain>} $f' output" >&2
            status=1
        fi
    done
done

if [ "$status" -eq 0 ]; then
    echo "property-provenance: OK ($n file(s), no marker on plain/--width non-tty output)"
fi
exit "$status"
