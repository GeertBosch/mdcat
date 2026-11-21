#!/usr/bin/env python3
"""probe-kitty-place.py - do gmore's Kitty re-placements line up with their text rows?

Why: on VSCode a built-in Mermaid diagram paged through gmore (`./mdcat README.md`) shows every
label one row above its hole in the image, and Ctrl-L does not fix it. Ghostty, Kitty and iTerm
are fine. mdcat itself places a diagram with one `a=T,C=1` (image first, then text); gmore writes
the text first and then re-places the image with a payload-free `a=p` crop that has no `C=1`.
VSCode's xterm.js scales a placement to r x (its CSS cell height), which can be fractional, while
CSI 16t reports a rounded height. These cases separate those differences.

The test image is a ruler of R rows by C columns: alternating blue and yellow bands, one band per
cell row, a dark line along each band's top, and a transparent hole in columns 2..5 of every band.
Each row's number is written as text ("r00", "r01", ...) into its hole.

READ: in each case, does every "rNN" sit inside its own hole on its own band? If not, by how many
rows is the text above (-) or below (+) the hole, and is it the same on every row or growing?

  A  mdcat's way: a=T,C=1 image, then the text rows (expected OK).
  B  gmore's repaint: text rows, CUU, DECSC, a=t then a=p (full crop, no C=1), DECRC, CUD.
  C  as B, but the top 3 rows are cropped off (y=3 rows), r=R-3: an image scrolled partly above
     the window. The top visible row must be r03.
  D  as B, but the image ends on the screen's second-last row, as at the bottom of gmore's window.
  E  as B, with C=1 on the a=p (a candidate fix).
  F  gmore's line scroll: rows scroll in at the bottom one at a time, each placed as a 1-row a=p
     slice right after its text, as gmore's advance() does.

Usage: tools/probe-kitty-place.py    (run in the terminal under test, not under tmux)
       PROBE_CELL_H=<px> overrides the cell height used for the PNG (default: CSI 16t).
"""

import base64
import os
import re
import select
import struct
import sys
import termios
import tty
import zlib

ESC = "\033"
R, C = 8, 24
ID = 4242
CW = 8


def query(seq, end=b"t", timeout=0.5):
    """Send a query to /dev/tty in raw mode and return the reply (bytes, up to `end`)."""
    fd = os.open("/dev/tty", os.O_RDWR)
    old = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        os.write(fd, seq.encode())
        buf = b""
        while select.select([fd], [], [], timeout)[0]:
            buf += os.read(fd, 64)
            if buf.endswith(end):
                break
        return buf
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        os.close(fd)


def nums(reply):
    return [int(x) for x in re.findall(rb"\d+", reply)]


def png(cw, ch):
    w, h = C * cw, R * ch
    blue, yellow, line = (190, 215, 255, 255), (255, 240, 170, 255), (40, 40, 40, 255)
    rows = []
    for y in range(h):
        band, yin = divmod(y, ch)
        row = bytearray([0])
        for x in range(w):
            col = x // cw
            if yin == 0:
                px = line
            elif 2 <= col < 6:
                px = (0, 0, 0, 0)
            else:
                px = blue if band % 2 == 0 else yellow
            row += bytes(px)
        rows.append(bytes(row))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))


def apc(data, action, extra=""):
    b64 = base64.b64encode(data).decode()
    out, pos = [], 0
    while True:
        part, pos = b64[pos:pos + 4096], pos + 4096
        more = pos < len(b64)
        ctl = f"a={action},f=100,i={ID},q=2,c={C},r={R},z=0{extra}" if not out else "q=2"
        out.append(f"{ESC}_G{ctl},m={1 if more else 0};{part}{ESC}\\")
        if not more:
            return "".join(out)


def place(y0, rows, ch, cursor_stays=False):
    extra = ",C=1" if cursor_stays else ""
    return (f"{ESC}_Ga=p,i={ID},q=2,x=0,y={y0 * ch},w={C * CW},h={rows * ch},"
            f"c={C},r={rows}{extra}{ESC}\\")


def label_row(k):
    # Skip to the hole with a cursor move (spaces over a VSCode image erase it), write the label.
    return f"{ESC}[2Cr{k:02d}"


def text_rows(first, count):
    return "".join(label_row(k) + "\n" for k in range(first, first + count))


def gmore_place(y0, rows, ch, transmit, cursor_stays=False):
    # The cursor is at column 1 on the line below the `rows` text lines: up, paint, back down.
    s = f"{ESC}[{rows}A{ESC}[1G{ESC}7" + transmit + place(y0, rows, ch, cursor_stays)
    return s + f"{ESC}8{ESC}[{rows}B\r"


def header(name, text):
    sys.stdout.write(f"{ESC}c{ESC}_Ga=d,d=A,q=2{ESC}\\== {name}: {text}\n\n")


def pause(name):
    sys.stdout.write(f"\n[{name}] screenshot, then Enter for the next case ")
    sys.stdout.flush()
    sys.stdin.readline()


def main():
    global CW
    env = {k: os.environ.get(k, "") for k in
           ("TERM_PROGRAM", "TERM_PROGRAM_VERSION", "TERM", "SSH_CONNECTION")}
    c16 = nums(query(f"{ESC}[16t"))
    c14 = nums(query(f"{ESC}[14t"))
    c18 = nums(query(f"{ESC}[18t"))
    sz = os.get_terminal_size(sys.stdout.fileno())
    ch = int(os.environ.get("PROBE_CELL_H") or (c16[1] if len(c16) >= 3 else 16))
    CW = c16[2] if len(c16) >= 3 else 8
    exact = c14[1] / c18[1] if len(c14) >= 3 and len(c18) >= 3 and c18[1] else 0
    info = (f"env {env}\nCSI16t={c16} CSI14t={c14} CSI18t={c18} winsize={sz.lines}x{sz.columns}\n"
            f"PNG cell {CW}x{ch}   exact cell height (14t/18t) = {exact:.3f}\n\n")
    data = png(CW, ch)
    transmit = apc(data, "t")

    header("A", "mdcat: a=T,C=1 image, then text")
    sys.stdout.write(info + f"{ESC}7{apc(data, 'T', ',C=1')}{ESC}8" + text_rows(0, R))
    pause("A")

    header("B", "gmore repaint: text, then a=t + a=p (no C=1)")
    sys.stdout.write(text_rows(0, R) + gmore_place(0, R, ch, transmit))
    pause("B")

    header("C", "as B, top 3 rows cropped off: the first row must be r03")
    sys.stdout.write(text_rows(3, R - 3) + gmore_place(3, R - 3, ch, transmit))
    pause("C")

    header("D", "as B, the image ending on the screen's second-last row")
    sys.stdout.write("\n" * max(0, sz.lines - 3 - R - 1))
    sys.stdout.write(text_rows(0, R) + gmore_place(0, R, ch, transmit) + "(row below the image)")
    pause("D")

    header("E", "as B, with C=1 on the a=p")
    sys.stdout.write(text_rows(0, R) + gmore_place(0, R, ch, transmit, cursor_stays=True))
    pause("E")

    header("F", "line scroll: 1-row a=p slices as rows scroll in at the bottom")
    sys.stdout.write("\n" * (sz.lines - 4) + transmit)
    for k in range(R):
        sys.stdout.write(label_row(k) + "\n" + gmore_place(k, 1, ch, ""))
    pause("F")
    sys.stdout.write(f"{ESC}_Ga=d,d=A,q=2{ESC}\\\n")


if __name__ == "__main__":
    main()
