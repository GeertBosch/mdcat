#!/usr/bin/env python3
"""probe-cluster-cursor.py - can save/restore + a relative move hide a terminal's over-wide clusters?

Why: Terminal.app advances the cursor by per-code-point widths (a ZWJ family is 11 columns) but
draws the sequence as one 2-column glyph with the following text right after it, so lines holding
such sequences look short (docs/TERMINAL-GRAPHICS.md section 8). The idea under test: write the
sequence between DECSC and DECRC (ESC 7 / ESC 8), then move right by its drawn width (CSI 2 C), so
the next text lands just after the glyph and mdcat could lay out by the drawn width.

Each case prints a column ruler, then "[" + sequence + "]x" written one way, then where the cursor
ended (CSI 6n). Samples: ZWJ family, astronaut, rainbow flag, keycap, skin tone.

  P  plain: the sequences written as text (today's output).
  S  save/restore: ESC 7, sequence, ESC 8, CSI 2 C, then the "]".
  E  as S, but starting 6 columns before the right edge: does the hidden 11-column advance wrap
     the glyph onto the next line before ESC 8 brings the cursor back?

READ, for S compared with P:
  1. Is each glyph still drawn whole (all four people, the rocket, the rainbow), or did the "]x"
     written after it erase part of it?
  2. Does "]" sit right after the glyph, at the column the ruler and the reported cursor give?
  3. In E, is the glyph on the same line as its "[", and the line below it clean?
  4. Optional: select line S with the mouse, copy, paste here. Do the sequences survive?

Usage: tools/probe-cluster-cursor.py    (run in the terminal under test, not under tmux)
"""

import os
import re
import select
import sys
import termios
import tty

ESC = "\033"
SAMPLES = [
    ("family", "\U0001F468‍\U0001F469‍\U0001F467‍\U0001F466"),
    ("astronaut", "\U0001F469‍\U0001F680"),
    ("rainbow", "\U0001F3F3️‍\U0001F308"),
    ("keycap", "1️⃣"),
    ("skin tone", "\U0001F44D\U0001F3FD"),
]


def cursor(fd):
    os.write(fd, f"{ESC}[6n".encode())
    buf = b""
    while select.select([fd], [], [], 0.5)[0]:
        buf += os.read(fd, 32)
        if buf.endswith(b"R"):
            break
    m = re.search(rb"\[(\d+);(\d+)R", buf)
    return (int(m.group(1)), int(m.group(2))) if m else None


def ruler(width):
    tens = "".join(str((c // 10) % 10) if c % 10 == 0 else " " for c in range(1, width + 1))
    ones = "".join(str(c % 10) for c in range(1, width + 1))
    return tens + "\r\n" + ones + "\r\n"


def main():
    env = {k: os.environ.get(k, "") for k in ("TERM_PROGRAM", "TERM_PROGRAM_VERSION", "TERM")}
    cols = os.get_terminal_size(sys.stdout.fileno()).columns
    fd = os.open("/dev/tty", os.O_RDWR)
    old = termios.tcgetattr(fd)
    report = []
    try:
        tty.setraw(fd)
        w = lambda s: os.write(fd, s.encode())
        w(f"{ESC}[2J{ESC}[H== probe-cluster-cursor  env {env}  cols {cols}\r\n\r\n")
        for name, seq in SAMPLES:
            w(f"-- {name}\r\n" + ruler(40))
            w("P [" + seq + "]x")
            p = cursor(fd)
            w("\r\nS [" + f"{ESC}7" + seq + f"{ESC}8{ESC}[2C" + "]x")
            s = cursor(fd)
            w(f"\r\nE {ESC}[{cols - 6}G[" + f"{ESC}7" + seq + f"{ESC}8{ESC}[2C" + "]x")
            e = cursor(fd)
            w("\r\n\r\n")
            report.append((name, p, s, e))
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        os.close(fd)
    print("cursor (row, col) after each line; '[' is at col 3 in P and S, so ']x' ends at")
    print("col 3 + 1 + 2 + 2 = 8 if the sequence takes 2 columns, more if it takes its advance:")
    for name, p, s, e in report:
        print(f"  {name:<10} P {p}  S {s}  E {e}")


if __name__ == "__main__":
    main()
