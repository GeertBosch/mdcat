# TEST: image rendering and text fallback

A paragraph that is solely an `<img ...>` tag pointing at a local PNG renders as
a real image via timg sixel graphics — but only on a terminal that supports
sixel. On terminals that don't (Apple's Terminal.app, or when output is piped or
redirected and thus not a TTY), it falls back to a one-line text label: the alt
text if present, otherwise the src filename, otherwise the literal tag.

**Expected on a graphics-capable TTY:** the first and third blocks show the
actual image; the second (a missing file) falls back to its filename.

**Expected on Apple_Terminal or any piped run:** no image is drawn. The first
line shows the alt text, the second shows `missing.png` (no alt, so the
filename), and the third shows the alt text again. Force this path with
`TERM_PROGRAM=Apple_Terminal ./mdcat tests/images.md`.

A 64x64 `chess-piece.png` (a black chess knight) is provided alongside this file,
so the real-image path works on a graphics-capable TTY; `missing.png` is left
absent on purpose to exercise the filename fallback.

---

<img src="chess-piece.png" alt="A black chess knight (alt text)">

<img src="missing.png" alt="A missing image (alt text)">

<img src="chess-piece.png" alt="A black chess knight (alt text)" width="64" height="64">

<img src="img/joan-mitchell.jpg" alt="JPEG image of Joan L. Mitchell who co-invented the format (alt text)">

<img src="img/sunflower.gif" alt="GIF image of a sunflower (alt text)">

<img src="img/chessboard.svg" alt="SVG image of a 3x4 square chessboard with all distinct chess pieces (alt text)" height="180">
