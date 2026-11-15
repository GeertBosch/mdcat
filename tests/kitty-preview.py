#!/usr/bin/env python3
"""Text preview of mdcat output that carries Kitty images (docs/plan-mermaid-flowcharts.md).

Reads mdcat's output (a file argument or stdin) and emulates just enough of a terminal to place
each Kitty image: text, LF (as CR LF, like a tty with ONLCR), CR, CSI A/B/C/D, and ESC 7 / ESC 8.
SGR and every other CSI, and OSC (hyperlinks), are stripped. Kitty APC chunks are collected into
images, and each PNG is decoded: a bad signature, CRC, zlib stream or size is a failure (exit 1).

Output, one line per screen row, with trailing spaces removed. Before the first row of each image
comes a header line

    image @ROW,COL c=C r=R z=Z C=K png=WxH

and inside an image's cols x rows footprint every cell that holds no text shows a quadrant block
for which of its 2x2 sub-cells contain stroke pixels (alpha > 0). A cell holding text shows the
text. With --labels, only the text layer is printed: no headers and no blocks.

The PNG must measure c*cellW x r*cellH, with the cell size from MDCAT_CELL_W / MDCAT_CELL_H
(default 10 x 20), the same variables mdcat reads.

Usage: kitty-preview.py [--labels] [FILE]
"""

import base64
import os
import re
import struct
import sys
import unicodedata
import zlib

QUADRANTS = " ▘▝▀▖▌▞▛▗▚▐▜▄▙▟█"  # index = TL | TR<<1 | BL<<2 | BR<<3


class PreviewError(Exception):
    pass


def char_width(ch):
    if unicodedata.combining(ch) or unicodedata.category(ch) in ("Mn", "Me", "Cf"):
        return 0
    return 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1


def decode_png(png):
    """Return (width, height, rows), where rows[y] is the unfiltered RGBA bytes of row y."""
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        raise PreviewError("bad PNG signature")
    pos, idat, ihdr = 8, b"", None
    while pos < len(png):
        if pos + 12 > len(png):
            raise PreviewError("truncated PNG chunk")
        (n,) = struct.unpack(">I", png[pos : pos + 4])
        typ, body = png[pos + 4 : pos + 8], png[pos + 8 : pos + 8 + n]
        crc = png[pos + 8 + n : pos + 12 + n]
        if len(crc) != 4 or zlib.crc32(typ + body) != struct.unpack(">I", crc)[0]:
            raise PreviewError("bad CRC in %s chunk" % typ.decode("latin-1"))
        if typ == b"IHDR":
            ihdr = body
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    if ihdr is None:
        raise PreviewError("PNG has no IHDR")
    w, h, depth, ctype, _comp, _filt, interlace = struct.unpack(">IIBBBBB", ihdr)
    if (depth, ctype, interlace) != (8, 6, 0):
        raise PreviewError("PNG is not 8-bit non-interlaced RGBA")
    raw = zlib.decompress(idat)
    stride = w * 4
    if len(raw) != h * (stride + 1):
        raise PreviewError("PNG data is %d bytes, expected %d" % (len(raw), h * (stride + 1)))
    rows, prev = [], bytearray(stride)
    for y in range(h):
        ft = raw[y * (stride + 1)]
        cur = bytearray(raw[y * (stride + 1) + 1 : (y + 1) * (stride + 1)])
        for i in range(stride):
            a = cur[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if ft == 0:
                p = 0
            elif ft == 1:
                p = a
            elif ft == 2:
                p = b
            elif ft == 3:
                p = (a + b) // 2
            elif ft == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                p = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
            else:
                raise PreviewError("bad PNG filter type %d" % ft)
            cur[i] = (cur[i] + p) & 0xFF
        rows.append(cur)
        prev = cur
    return w, h, rows


def parse(text):
    """Run the terminal emulation. Returns (screen, images); screen is {(row, col): char}."""
    screen, images = {}, []
    row = col = 0
    saved = (0, 0)
    pending = None  # [controls, payload parts, anchor] of an image still receiving chunks
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch == "\x1b" and i + 1 < n:
            nxt = text[i + 1]
            if nxt == "[":
                j = i + 2
                while j < n and not ("@" <= text[j] <= "~"):
                    j += 1
                if j >= n:
                    raise PreviewError("unterminated CSI")
                params, final = text[i + 2 : j], text[j]
                if final in "ABCD" and re.fullmatch(r"\d*", params):
                    k = int(params) if params else 1
                    if final == "A":
                        row = max(0, row - k)
                    elif final == "B":
                        row += k
                    elif final == "C":
                        col += k
                    else:
                        col = max(0, col - k)
                i = j + 1
            elif nxt in "]_":  # OSC or APC, ended by ST (ESC \) or, for OSC, BEL
                j = i + 2
                while j < n and not (text[j : j + 2] == "\x1b\\" or (nxt == "]" and text[j] == "\x07")):
                    j += 1
                if j >= n:
                    raise PreviewError("unterminated %s" % ("OSC" if nxt == "]" else "APC"))
                body = text[i + 2 : j]
                i = j + (1 if text[j] == "\x07" else 2)
                if nxt == "_" and body.startswith("G"):
                    ctrl, _, payload = body[1:].partition(";")
                    keys = dict(kv.split("=", 1) for kv in ctrl.split(",") if "=" in kv)
                    if "a" in keys:
                        if pending is not None:
                            raise PreviewError("image started while another is unfinished")
                        pending = [keys, [payload], (row, col)]
                    elif pending is None:
                        raise PreviewError("continuation chunk with no image")
                    else:
                        pending[1].append(payload)
                    if pending is not None and keys.get("m", "0") == "0":
                        images.append((pending[0], "".join(pending[1]), pending[2]))
                        pending = None
            elif nxt == "7":
                saved = (row, col)
                i += 2
            elif nxt == "8":
                row, col = saved
                i += 2
            else:
                i += 2
        elif ch == "\n":
            row, col = row + 1, 0
            i += 1
        elif ch == "\r":
            col = 0
            i += 1
        elif ch < " " or ch == "\x7f":
            i += 1
        else:
            w = char_width(ch)
            if w == 0:
                if col > 0 and (row, col - 1) in screen:
                    screen[(row, col - 1)] += ch
                elif col > 1 and (row, col - 2) in screen:
                    screen[(row, col - 2)] += ch
            else:
                screen[(row, col)] = ch
                if w == 2:
                    screen[(row, col + 1)] = ""
                col += w
            i += 1
    if pending is not None:
        raise PreviewError("image is missing its final chunk (m=0)")
    screen.setdefault((row, 0), "")
    return screen, images


def stroke_blocks(keys, payload, cell_w, cell_h):
    """Decode one image; return (png size, {(cell row, cell col): quadrant index})."""
    c, r = int(keys.get("c", 0)), int(keys.get("r", 0))
    if not c or not r:
        raise PreviewError("image has no c=/r= footprint")
    png = base64.b64decode(payload, validate=True)
    w, h, rows = decode_png(png)
    if (w, h) != (c * cell_w, r * cell_h):
        raise PreviewError(
            "PNG is %dx%d, expected %dx%d for c=%d r=%d" % (w, h, c * cell_w, r * cell_h, c, r)
        )
    half_w, half_h = cell_w // 2, cell_h // 2
    blocks = {}
    for y in range(h):
        row = rows[y]
        cy = y // cell_h
        qy = 1 if (y % cell_h) >= half_h else 0
        for x in range(w):
            if row[x * 4 + 3]:
                qx = 1 if (x % cell_w) >= half_w else 0
                key = (cy, x // cell_w)
                blocks[key] = blocks.get(key, 0) | (1 << (qy * 2 + qx))
    return (w, h), blocks


def render(text, labels_only, cell_w, cell_h):
    screen, images = parse(text)
    headers = {}
    blocks = {}
    for keys, payload, (arow, acol) in images:
        size, found = stroke_blocks(keys, payload, cell_w, cell_h)
        headers.setdefault(arow, []).append(
            "image @%d,%d c=%s r=%s z=%s C=%s png=%dx%d"
            % (arow, acol, keys.get("c"), keys.get("r"), keys.get("z"), keys.get("C"), *size)
        )
        for (cy, cx), q in found.items():
            blocks[(arow + cy, acol + cx)] = blocks.get((arow + cy, acol + cx), 0) | q
    last = max(r for r, _ in screen)
    out = []
    for r in range(last + 1):
        if not labels_only:
            out.extend(headers.get(r, []))
        cells = {c: ch for (rr, c), ch in screen.items() if rr == r}
        width = max([c + 1 for c in cells] + [c + 1 for (rr, c) in blocks if rr == r], default=0)
        line = []
        for c in range(width):
            if c in cells:
                line.append(cells[c] or "")
            elif not labels_only and blocks.get((r, c)):
                line.append(QUADRANTS[blocks[(r, c)]])
            else:
                line.append(" ")
        out.append("".join(line).rstrip())
    return "\n".join(out) + "\n"


def main(argv):
    args = [a for a in argv if a != "--labels"]
    labels_only = len(args) != len(argv)
    if len(args) > 1:
        sys.stderr.write(__doc__)
        return 2
    data = open(args[0], "rb").read() if args else sys.stdin.buffer.read()
    cell_w = int(os.environ.get("MDCAT_CELL_W", "10"))
    cell_h = int(os.environ.get("MDCAT_CELL_H", "20"))
    try:
        result = render(data.decode("utf-8", "replace"), labels_only, cell_w, cell_h)
    except (PreviewError, ValueError, zlib.error) as e:
        sys.stderr.write("kitty-preview: %s\n" % e)
        return 1
    sys.stdout.buffer.write(result.encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
