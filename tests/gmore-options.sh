#!/bin/sh
# gmore command-line options: --help prints usage on stdout and exits 0; an
# unknown option prints a diagnostic on stderr and exits 2.
#
# Usage: tests/gmore-options.sh [gmore-binary]
# Exit status: 0 if all cases pass, 1 otherwise.

set -u

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
gmore=${1:-$root/gmore}

if [ ! -x "$gmore" ]; then
    echo "gmore-options: $gmore is not an executable; run 'make' first" >&2
    exit 2
fi

fails=0
check() {
    if [ "$2" = "$3" ]; then
        echo "ok   - $1"
    else
        echo "FAIL - $1 (expected '$3', got '$2')"
        fails=$((fails + 1))
    fi
}

out=$("$gmore" --help 2>/dev/null </dev/null)
check "--help exit status" "$?" 0
case $out in usage:*) r=usage ;; *) r=other ;; esac
check "--help prints usage on stdout" "$r" usage

out=$("$gmore" -h 2>/dev/null </dev/null)
check "-h exit status" "$?" 0

out=$("$gmore" --bogus 2>&1 >/dev/null </dev/null)
check "unknown option exit status" "$?" 2
case $out in *"unrecognized option '--bogus'"*) r=named ;; *) r=other ;; esac
check "unknown option names the option on stderr" "$r" named

"$gmore" -x >/dev/null 2>&1 </dev/null
check "unknown short option exit status" "$?" 2

# +N opens with line N at the top (50-line file, 23-row page => maxTop 27).
seq 1 50 > "${TMPDIR:-/tmp}/gmore-options.$$"
trap 'rm -f "${TMPDIR:-/tmp}/gmore-options.$$"' EXIT
in="${TMPDIR:-/tmp}/gmore-options.$$"
trace() { GMORE_KEYS= LINES=24 COLUMNS=80 "$gmore" --nav-trace "$@" 2>&1; }
check "+10 starts at line 10" "$(trace +10 "$in")" 'top=9 bottom=32 total=50 pct=64% more'
check "+1 starts at the top" "$(trace +1 "$in")" 'top=0 bottom=23 total=50 pct=46% more'
check "+999 clamps to the last page" "$(trace +999 "$in")" 'top=27 bottom=50 total=50 pct=100% END'
check "+10 after the file" "$(trace "$in" +10)" 'top=9 bottom=32 total=50 pct=64% more'
check "+10 from stdin" "$(trace +10 <"$in")" 'top=9 bottom=32 total=50 pct=64% more'

"$gmore" +abc >/dev/null 2>&1 </dev/null
check "+abc exit status" "$?" 2
"$gmore" + >/dev/null 2>&1 </dev/null
check "bare + exit status" "$?" 2

[ "$fails" -eq 0 ]
