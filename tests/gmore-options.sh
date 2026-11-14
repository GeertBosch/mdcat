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

[ "$fails" -eq 0 ]
