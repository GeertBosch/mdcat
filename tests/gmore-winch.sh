#!/bin/sh
# SIGWINCH resize re-flow, end to end under a pty: mdcat pages a long document,
# the window is resized (TIOCSWINSZ + SIGWINCH), and the pager must re-render at
# the new width. The witness is the H1 title rule ("━" repeated to the terminal
# width): it must appear at the old width before the resize and at exactly the
# new width after it.
#
# The signal is sent to the PROCESS (os.kill), so the kernel picks an arbitrary
# thread to deliver it — same as a real terminal. This is exactly the case the
# self-pipe in gmore_run.cpp exists for: a handler running on a render/image
# worker thread must still wake the pager's key loop.
#
# Usage: tests/gmore-winch.sh [mdcat-binary]
# Exit status: 0 pass, 1 fail, 2 setup problem (missing binary/python).

set -u

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
mdcat=${1:-$root/mdcat}

if [ ! -x "$mdcat" ]; then
    echo "gmore-winch: $mdcat is not an executable; run 'make' first" >&2
    exit 2
fi

# A python3 with pty support (the repo venv's python is fine too; probe it).
PY=""
for cand in python3 /usr/bin/python3; do
    if command -v "$cand" > /dev/null 2>&1 &&
        "$cand" -c 'import pty,fcntl,termios,struct,select' 2> /dev/null; then
        PY=$(command -v "$cand")
        break
    fi
done
if [ -z "$PY" ]; then
    echo "gmore-winch: no usable python3 with pty support" >&2
    exit 2
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Long plain-text document (no images): a title for the width-spanning rule plus
# enough paragraphs that the pager enters its interactive loop (doc > one screen).
{
    echo '# Resize Reflow Probe'
    echo
    i=1
    while [ $i -le 60 ]; do
        echo "Paragraph $i: lorem ipsum dolor sit amet, consectetur adipiscing elit."
        echo
        i=$((i + 1))
    done
} > "$tmp/doc.md"

TERM=xterm-256color "$PY" - "$mdcat" "$tmp/doc.md" << 'EOF'
import os, pty, re, select, signal, struct, sys, time, fcntl, termios

mdcat, doc = sys.argv[1], sys.argv[2]
W0, W1, H = 90, 48, 24

pid, fd = pty.fork()
if pid == 0:
    os.execv(mdcat, [mdcat, doc])
# Set the initial size right away; mdcat queries it during startup.
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', H, W0, 0, 0))

def rule_widths(buf):
    """Lengths of maximal runs of the heading-rule glyph in the output so far."""
    text = buf.decode('utf-8', 'replace')
    return set(len(m.group()) for m in re.finditer('━+', text))

def read_until(pred, timeout):
    buf = b''
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            if pred(buf):
                return buf, True
    return buf, pred(buf)

def fail(msg, buf):
    print('FAIL: ' + msg)
    print('  rule widths seen: %s' % sorted(rule_widths(buf)))
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
    sys.exit(1)

pre, ok = read_until(lambda b: W0 in rule_widths(b), 10)
if not ok:
    fail('no initial paint with a width-%d title rule' % W0, pre)

# Resize the pty and signal the process, as a terminal emulator would.
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', H, W1, 0, 0))
os.kill(pid, signal.SIGWINCH)

post, ok = read_until(lambda b: W1 in rule_widths(b), 10)
if not ok:
    fail('no re-flow: width-%d title rule never appeared after SIGWINCH' % W1, post)

os.write(fd, b'q')
end = time.time() + 5
status = None
while time.time() < end:
    got, status = os.waitpid(pid, os.WNOHANG)
    if got == pid:
        break
    # Keep draining so the child never blocks on a full pty buffer.
    r, _, _ = select.select([fd], [], [], 0.1)
    if r:
        try:
            os.read(fd, 65536)
        except OSError:
            pass
else:
    fail('pager did not exit on q after resize', post)
print('ok: rule %d -> %d after SIGWINCH' % (W0, W1))
EOF
