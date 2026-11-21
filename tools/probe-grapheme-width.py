#!/usr/bin/env python3
"""probe-grapheme-width.py - how many columns does this terminal give each kind of grapheme?

Why: mdcat measures text by grapheme cluster (unicode_width.h): an emoji ZWJ sequence, a
variation-selector emoji or a flag is one 2-column character. A terminal that measures code
points one by one makes such a line wider, autowraps it onto an extra row, and every row above it
on the screen moves up. In gmore that put a Mermaid diagram's labels one row above their image on
VSCode (the image is placed relative to the bottom of the window).

For each sample this writes the text at column 1 and asks the terminal where the cursor is
(CSI 6n), so the output is plain text you can paste. "mdcat" is the width in mdcat's grapheme mode;
a terminal that differs needs MDCAT_WIDTH=codepoint (see unicode_width.h).

Usage: tools/probe-grapheme-width.py    (run in the terminal under test, not under tmux)
"""

import os
import re
import select
import sys
import termios
import tty

SAMPLES = [
    ("ascii", "abc", 3),
    ("cjk", "漢字", 4),
    ("emoji", "🎉", 2),
    ("combining", "é", 1),
    ("vs16 heart", "❤️", 2),
    ("vs16 plane", "✈️", 2),
    ("zwj astronaut", "👩‍🚀", 2),
    ("zwj family", "👨‍👩‍👧‍👦", 2),
    ("zwj rainbow flag", "🏳️‍🌈", 2),
    ("flag", "🇺🇸", 2),
    ("skin tone", "👍🏽", 2),
    ("keycap", "1\ufe0f\u20e3", 2),
    ("vs16 umbrella", "\u2602\ufe0f", 2),
    ("vs15 text heart", "\u2764\ufe0e", 1),
    ("plain heart", "\u2764", 1),
    ("zwj no vs16", "🧑\u200d💻", 2),
    ("hangul jamo", "\u1100\u1161", 2),
    ("arabic", "عربية", 5),
    ("ambiguous", "±§", 2),
    ("misc symbol", "★☂✓", 3),
    ("a + zwj", "a‍", 1),
    ("1 + keycap mark", "1⃣", 1),
    ("a + enclosing ring", "a⃝", 1),
]


def main():
    env = {k: os.environ.get(k, "") for k in ("TERM_PROGRAM", "TERM_PROGRAM_VERSION", "TERM")}
    fd = os.open("/dev/tty", os.O_RDWR)
    old = termios.tcgetattr(fd)
    results = []
    try:
        tty.setraw(fd)
        for name, text, want in SAMPLES:
            os.write(fd, ("\r\033[2K" + text + "\033[6n").encode())
            buf = b""
            while select.select([fd], [], [], 0.5)[0]:
                buf += os.read(fd, 64)
                if buf.endswith(b"R"):
                    break
            m = re.search(rb"\[(\d+);(\d+)R", buf)
            results.append((name, text, want, int(m.group(2)) - 1 if m else None))
        os.write(fd, b"\r\033[2K\033[?2027$p")
        buf = b""
        while select.select([fd], [], [], 0.5)[0]:
            buf += os.read(fd, 64)
            if buf.endswith(b"y"):
                break
        m = re.search(rb"\?2027;(\d)\$y", buf)
        mode2027 = {"0": "not recognized", "1": "set", "2": "reset", "3": "permanently set",
                    "4": "permanently reset"}.get(m.group(1).decode(), "?") if m else "no reply"
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        os.close(fd)
    print(f"env {env}")
    print(f"grapheme cluster mode 2027: {mode2027}")
    print(f"{'sample':<18} {'mdcat':>5} {'term':>5}")
    for name, text, want, got in results:
        flag = "" if got == want else "   <-- differs"
        print(f"{name:<18} {want:>5} {str(got):>5}{flag}")


if __name__ == "__main__":
    main()
