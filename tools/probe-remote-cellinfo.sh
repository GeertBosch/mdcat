#!/bin/sh
# probe-remote-cellinfo.sh — what cell/area info is available where mdcat runs?
#
# Why: mdcat sizes images by converting pixels <-> cells. Its best source is the
# kernel's TIOCGWINSZ pixel fields (ws_xpixel/ws_ypixel); its fallback is the
# CSI 16t/14t/18t terminal round-trip. Over SSH the kernel pixel fields are
# usually 0 (sshd doesn't forward them) but the CSI round-trip still reaches the
# LOCAL terminal through the pty. This probe shows EXACTLY what each source
# returns in the current context, so we know what mdcat can rely on remotely.
#
# Run it BOTH locally and over SSH into a remote host, in each target terminal
# (VSCode, iTerm2, Orbstack). Compare. The hypotheses to confirm/refute:
#
#   H1: Over SSH, `stty size` still reports cols/rows (sshd forwards those).
#   H2: Over SSH, TIOCGWINSZ pixel fields are 0 (need a tiny C check, below;
#       `stty -a` doesn't show them, so we infer from CSI 14t vs a local run).
#   H3: Over SSH, CSI 16t/14t/18t STILL get answered by the local terminal
#       (possibly slower). If so, the round-trip is a viable remote fallback.
#   H4: Relevant env vars (TERM, TERM_PROGRAM, KITTY_WINDOW_ID, SSH_*) — which
#       survive the SSH hop? (They mostly DON'T unless SendEnv/SetEnv is set.)
#
# How to read it: compare the local vs SSH columns. If CSI replies are present
# over SSH, the round-trip fallback works. If env vars are blank over SSH, we
# can't rely on them for capability detection and must probe or default.

dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$dir/probe-common.sh"
[ -t 1 ] || { echo "stdout is not a TTY; run this directly in a terminal" >&2; exit 1; }

banner "Context"
printf 'over SSH?           SSH_CONNECTION=[%s] SSH_TTY=[%s]\n' "${SSH_CONNECTION:-}" "${SSH_TTY:-}"
printf 'TERM=[%s] TERM_PROGRAM=[%s] TERM_PROGRAM_VERSION=[%s]\n' \
       "${TERM:-}" "${TERM_PROGRAM:-}" "${TERM_PROGRAM_VERSION:-}"
printf 'KITTY_WINDOW_ID=[%s] COLORTERM=[%s] MDCAT_GRAPHICS=[%s]\n' \
       "${KITTY_WINDOW_ID:-}" "${COLORTERM:-}" "${MDCAT_GRAPHICS:-}"

banner "Kernel winsize (stty)"
echo "stty size (rows cols): $(stty size 2>/dev/null)"
echo "(stty does not expose ws_xpixel/ws_ypixel; CSI 14t below is the proxy.)"

banner "Terminal round-trip queries (reach the LOCAL terminal even over SSH)"
r16=$(term_query '16t' t); echo "CSI 16t (cell px)   -> [$r16]"
r14=$(term_query '14t' t); echo "CSI 14t (area px)   -> [$r14]"
r18=$(term_query '18t' t); echo "CSI 18t (area cells)-> [$r18]"

# Derive cell size from area-px / area-cells if 16t was unsupported but 14t+18t work.
aw=$(echo "$r14" | sed -n 's/.*4;\([0-9]*\);\([0-9]*\)t.*/\2/p')
ah=$(echo "$r14" | sed -n 's/.*4;\([0-9]*\);\([0-9]*\)t.*/\1/p')
cc=$(echo "$r18" | sed -n 's/.*8;\([0-9]*\);\([0-9]*\)t.*/\2/p')
cr=$(echo "$r18" | sed -n 's/.*8;\([0-9]*\);\([0-9]*\)t.*/\1/p')
if [ -n "$aw" ] && [ -n "$cc" ] && [ "$cc" -gt 0 ] 2>/dev/null; then
    printf 'derived cell ~ %sx%s px (areaPx %sx%s / areaCells %sx%s)\n' \
           "$(( aw / cc ))" "$(( ah / cr ))" "$aw" "$ah" "$cc" "$cr"
else
    echo "could not derive cell size from 14t/18t (one of them was empty)."
fi

banner "DONE"
echo "Run this locally AND over SSH in each terminal; compare the two outputs."
echo "Key questions: are CSI replies present over SSH? are env vars present over SSH?"
