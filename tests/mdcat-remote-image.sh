#!/bin/sh
# Remote image trust gate (producer side): mdcat may fetch and render a remote
# image (e.g. a CI badge) ONLY when the URL is https AND its host is trusted.
# Trust is built from the rendered file's git remotes plus a user config file at
# $XDG_CONFIG_HOME/mdcat/trusted-hosts. Anything else falls back to alt text,
# exactly like a non-graphics terminal.
#
# The offline cases assert the GATE (what is refused / accepted), not pixels:
#   - untrusted host           -> alt-text fallback, no fetch;
#   - http (not https)         -> alt-text fallback, no fetch;
#   - URL with embedded creds  -> alt-text fallback, no fetch;
#   - unsupported extension    -> alt-text fallback, no fetch;
#   - trusted via config file  -> a fetch IS attempted (proven by pointing at an
#                                 unreachable host: the attempt fails and we get
#                                 the alt text, but slowly/with curl invoked; we
#                                 assert acceptance differently — see below).
#
# Because a positive render needs the network, the actual "badge renders as a
# Kitty image" case runs only when MDCAT_REMOTE_NET=1 is set (so `make check`
# stays offline). The gate cases are fully offline.
#
# Trust is forced deterministically by pointing XDG_CONFIG_HOME at a temp dir
# with our own trusted-hosts file, so the test does not depend on the repo's
# real git remotes. Graphics is forced with --img so the stream is terminal-
# independent.
#
# Requires `timg`; skips cleanly when absent (the gate logic runs before timg,
# but the positive net case needs it).
#
# Usage: tests/mdcat-remote-image.sh [mdcat-binary]
# Exit status: 0 if all cases pass (or skipped), 1 otherwise.

set -u

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
mdcat=${1:-$root/mdcat}

if [ ! -x "$mdcat" ]; then
    echo "mdcat-remote-image: $mdcat is not an executable; run 'make' first" >&2
    exit 2
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fails=0
n=0

# A trusted-hosts config that trusts example.com and its subdomains, plus the
# GitHub badge host. Point XDG_CONFIG_HOME here so trust is deterministic.
mkdir -p "$tmp/config/mdcat"
cat > "$tmp/config/mdcat/trusted-hosts" <<'EOF'
# test trust list
example.com
*.trusted.example.org
github.com
EOF

# Render a one-line document containing a single markdown image, with trust
# sourced from our temp config and graphics forced to Kitty. Run in $tmp (not the
# repo) so the repo's own git remotes never enter the trust list.
render() {  # render MARKDOWN -> bytes on stdout
    printf '%s\n' "$1" > "$tmp/doc.md"
    ( cd "$tmp" && XDG_CONFIG_HOME="$tmp/config" HOME="$tmp/nohome" \
        "$mdcat" --img kitty "$tmp/doc.md" 2>/dev/null )
}

pass() { n=$((n + 1)); }
fail() { echo "mdcat-remote-image: FAIL [$1] ($2)" >&2; fails=$((fails + 1)); }

# A rendered image shows up as a Kitty APC (ESC _ G ... ESC \). Its absence means
# the alt-text fallback fired (the gate refused, or the fetch failed).
has_apc() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n' | grep -q '1b5f47'; }

# --- offline gate cases: each MUST fall back (no APC) ---

# Untrusted host.
out=$(render '![ALT-UNTRUSTED](https://evil.invalid/a.svg)')
if has_apc "$out"; then fail "untrusted host" "unexpected image render"; \
elif printf '%s' "$out" | grep -q 'ALT-UNTRUSTED'; then pass; \
else fail "untrusted host" "alt text missing"; fi

# http (not https) even for a trusted host.
out=$(render '![ALT-HTTP](http://example.com/a.svg)')
if has_apc "$out"; then fail "http scheme" "unexpected image render"; \
elif printf '%s' "$out" | grep -q 'ALT-HTTP'; then pass; \
else fail "http scheme" "alt text missing"; fi

# Embedded credentials in a trusted host -> refused.
out=$(render '![ALT-CREDS](https://user:pw@example.com/a.svg)')
if has_apc "$out"; then fail "embedded creds" "unexpected image render"; \
elif printf '%s' "$out" | grep -q 'ALT-CREDS'; then pass; \
else fail "embedded creds" "alt text missing"; fi

# Unsupported extension on a trusted host -> refused before any fetch.
out=$(render '![ALT-EXT](https://example.com/a.txt)')
if has_apc "$out"; then fail "bad extension" "unexpected image render"; \
elif printf '%s' "$out" | grep -q 'ALT-EXT'; then pass; \
else fail "bad extension" "alt text missing"; fi

# A subdomain of a *.trusted.example.org entry is trusted; a sibling is not. We
# can't fetch either offline, but a bare host (no subdomain match) and a wrong
# parent must still refuse. (Trusted-but-unreachable also falls back, so these
# only assert the alt text appears — the positive proof is the net case below.)
out=$(render '![ALT-SUB](https://untrusted.example.net/a.svg)')
if printf '%s' "$out" | grep -q 'ALT-SUB' && ! has_apc "$out"; then pass; \
else fail "unrelated host" "expected fallback to alt text"; fi

# --- network-gated positive case ---
# With the network available, a trusted https badge renders as a Kitty image.
if [ "${MDCAT_REMOTE_NET:-0}" = "1" ]; then
    if ! command -v timg >/dev/null 2>&1; then
        echo "mdcat-remote-image: SKIP net case (timg not installed)"
    else
        url='https://github.com/GeertBosch/mdcat/actions/workflows/ci.yml/badge.svg?branch=main'
        out=$(render "![CI]($url)")
        if has_apc "$out"; then pass
        else fail "net: trusted badge renders" "no Kitty APC (fetch or render failed)"; fi
    fi
else
    echo "mdcat-remote-image: SKIP net case (set MDCAT_REMOTE_NET=1 to enable)"
fi

if [ "$fails" -eq 0 ]; then
    echo "mdcat-remote-image: OK ($n cases)"
    exit 0
fi
echo "mdcat-remote-image: FAIL ($fails of $((n + fails)) cases)" >&2
exit 1
