#!/bin/bash
# Run every Mermaid diagram in a corpus of Markdown files through mdcat's built-in renderer and
# report, per diagram, whether it was drawn.
#
# usage: tools/mermaid-sweep.sh MDCAT [CORPUS] [WIDTH]
#   MDCAT   absolute path of the mdcat binary to test
#   CORPUS  a git checkout to sweep (default: $MONGO, else ~/mongo)
#   WIDTH   terminal width to render at (default: 80)
#
# Prints one line per diagram, `file:line<TAB>OK` or `file:line<TAB><fallback reason>`, then a
# count per result. `mmdc` is kept off PATH and `--mermaid builtin` is forced, so the result is the
# built-in renderer's alone.
set -u
MDCAT=${1:?usage: mermaid-sweep.sh MDCAT [CORPUS] [WIDTH]}
CORPUS=${2:-${MONGO:-$HOME/mongo}}
WIDTH=${3:-80}
case $MDCAT in /*) ;; *) MDCAT=$PWD/$MDCAT ;; esac
results=$(mktemp)
trap 'rm -f "$results"' EXIT
cd "$CORPUS" || exit 1
git grep -l -E '^[[:space:]>]*(```|~~~)[[:space:]]*mermaid' -- '*.md' | while read -r f; do
    d=$(dirname "$f")
    b=$(basename "$f")
    errs=$(cd "$d" && MDCAT_MERMAID_DEBUG=1 MDCAT_CELL_W=10 MDCAT_CELL_H=20 PATH=/usr/bin:/bin \
        "$MDCAT" --img kitty --mermaid builtin --width "$WIDTH" "$b" 2>&1 >/dev/null)
    for l in $(grep -nE '^[[:space:]>]*(```|~~~)[[:space:]]*mermaid' "$f" | cut -d: -f1); do
        r=$(printf '%s\n' "$errs" | grep "^mdcat: $b:$l: " | sed 's/.*fallback: //' | head -1)
        printf '%s:%s\t%s\n' "$f" "$l" "${r:-OK}"
    done
done | tee "$results"
echo
cut -f2 "$results" | sort | uniq -c | sort -rn
