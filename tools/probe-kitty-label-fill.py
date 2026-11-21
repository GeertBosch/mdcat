#!/usr/bin/env python3
"""probe-kitty-label-fill.py - how does a terminal show a text label on a filled node?

Why: mdcat's built-in Mermaid renderer draws a filled node as one Kitty image (z=0, over the
text) whose label cells are transparent, and writes the label as text with the fill as its SGR
background. On iTerm a dark bar appeared at the start of the labels of filled nodes, though the
bytes emitted are exactly that. This draws the same node several ways so a screenshot shows which
variant is clean.

Each block is a 12x3-cell node (peach fill, dark outline); its label "Fix" is on the middle row.
The image is 120x60 px for 12x3 cells and is scaled by the terminal to c=12,r=3.

  A  as mdcat does it: hole of exactly the label's 3 cells; "Fix" with SGR bg, reached by CUF.
  B  hole 1 cell wider each side; label padded with U+00A0 in the same bg.
  C  hole as A, but the cells left of the label are written as bg-coloured U+00A0 too, so the
     whole run starts at the hole's edge without a cursor move in between.
  D  no hole (opaque fill) and the image at z=-1, under the text; label has no bg.
  E  hole across the node's whole middle row; the entire row is written as bg-coloured text.
  F  hole as A, but the label is reached with real spaces (bg colour set first) not CUF.

Round 1 (A-F) showed a dark bar in the first cell of the label run in A, B and E, all of which
reach the label with a cursor-forward (CUF); C and F, which start at column 0, and D were clean.
Round 2 asks whether another way to skip the cells avoids it:

  G  CUF, one default-background U+00A0 as a sacrificial cell, then the label run as in A.
  H  the background set before the CUF, then the label.
  J  an absolute column (CHA, ESC[nG) instead of CUF, then the label as in A.
  M  as A, with the hole inset by 1 px on every side (does that remove the hairlines above and
     below the label, where the hole meets the text background?). M also uses CHA.
  N  as A, but CUF is written as two moves, ESC[2C ESC[2C.

Read: for each block, is "Fix" on an unbroken peach fill, with no dark bar beside it?

Usage: tools/probe-kitty-label-fill.py      (run in the terminal under test; PROBE_ROUND=1 for A-F)
"""

import base64
import os
import struct
import sys
import zlib

W, H, C, R = 120, 60, 12, 3
FILL = (255, 221, 170)
LINE = (51, 51, 51)
ESC = "\033"


def png(hole, opaque_hole=False, inset=0):
    rows = []
    for y in range(H):
        row = bytearray([0])
        for x in range(W):
            edge = x < 2 or x >= W - 2 or y < 2 or y >= H - 2
            inhole = (hole and hole[0] + inset <= x < hole[1] - inset and
                      20 + inset <= y < 40 - inset)
            if edge:
                px = LINE + (255,)
            elif inhole and not opaque_hole:
                px = FILL + (0,)
            else:
                px = FILL + (255,)
            row += bytes(px)
        rows.append(bytes(row))
    raw = zlib.compress(b"".join(rows))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", raw) + chunk(b"IEND", b""))


def apc(data, z):
    b64 = base64.b64encode(data).decode()
    out = []
    for i in range(0, len(b64), 4096):
        more = 1 if i + 4096 < len(b64) else 0
        if i == 0:
            out.append(f"{ESC}_Ga=T,f=100,q=2,C=1,c={C},r={R},z={z},m={more};{b64[i:i+4096]}{ESC}\\")
        else:
            out.append(f"{ESC}_Gq=2,m={more};{b64[i:i+4096]}{ESC}\\")
    return "".join(out)


bg = f"{ESC}[48;2;{FILL[0]};{FILL[1]};{FILL[2]}m"
fg = f"{ESC}[38;2;0;0;0m"
off = f"{ESC}[49m{ESC}[39m"
NB = " "
CELL = W // C  # px per cell in the image: 10

# (title, image bytes, z, text for the middle row, relative to the node's left edge)
ROUND = os.environ.get("PROBE_ROUND", "2")
v1 = [
    ("A", png((40, 70)), 0, f"{ESC}[4C{bg}{fg}Fix{off}"),
    ("B", png((30, 80)), 0, f"{ESC}[3C{bg}{fg}{NB}Fix{NB}{off}"),
    ("C", png((40, 70)), 0, f"{bg}{fg}{NB * 4}Fix{off}"),
    ("D", png(None), -1, f"{ESC}[4C{fg}Fix{off}"),
    ("E", png((10, 110)), 0, f"{ESC}[1C{bg}{fg}{NB * 3}Fix{NB * 4}{off}"),
    ("F", png((40, 70)), 0, f"{bg}{fg}    Fix{off}"),
]
v2 = [
    ("A", png((40, 70)), 0, f"{ESC}[4C{bg}{fg}Fix{off}"),
    ("G", png((40, 70)), 0, f"{ESC}[3C{NB}{bg}{fg}Fix{off}"),
    ("H", png((40, 70)), 0, f"{bg}{ESC}[4C{fg}Fix{off}"),
    ("J", png((40, 70)), 0, f"{ESC}[5G{bg}{fg}Fix{off}"),
    ("M", png((40, 70), inset=1), 0, f"{ESC}[5G{bg}{fg}Fix{off}"),
    ("N", png((40, 70)), 0, f"{ESC}[2C{ESC}[2C{bg}{fg}Fix{off}"),
]
variants = v1 if ROUND == "1" else v2

for k, v in (("TERM_PROGRAM", None), ("TERM", None), ("SSH_CONNECTION", None)):
    print(f"{k}={os.environ.get(k, '')}")
print()
for title, data, z, mid in variants:
    sys.stdout.write(f"{title}\n")
    sys.stdout.write(f"{ESC}7{apc(data, z)}{ESC}8")
    sys.stdout.write("\r\n" + mid + "\r\n\r\n")
    sys.stdout.flush()
