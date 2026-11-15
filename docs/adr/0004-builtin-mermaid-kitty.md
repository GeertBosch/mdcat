# ADR 0004 — Built-in Mermaid flowcharts: shapes as a Kitty image, labels as text

## Status

Accepted. The gating behaviour was measured with
`tools/probe-kitty-text-overlay.sh`; see `docs/TERMINAL-GRAPHICS.md` §10. The
implementation plan is `docs/plan-mermaid-flowcharts.md`.

## Context

A ```` ```mermaid ```` block is rendered today by `mmdc` (headless Chrome) to a PNG,
which `timg` converts. That is slow (seconds per diagram), needs Node and Chrome,
and produces an opaque raster: the labels cannot be searched in `gmore`, selected,
or themed. Without `mmdc`, or without graphics, the block is shown as source.

The first attempt at a built-in renderer drew everything with Unicode box-drawing
characters. It could not meet the quality bar: arrowheads cannot touch an outline
exactly, lines cannot be inset by half a cell, and diagonals and curves are
impossible.

## Decision

### 1. Two layers

Layout happens on the character-cell grid. The drawer produces:

- a **graphics layer**: one transparent RGBA raster, `cols × cellW` by
  `rows × cellH` pixels, holding only strokes (outlines, edges, arrowheads);
- a **text layer**: `rows` strings of exactly `cols` columns, holding the labels in
  the terminal's own font and the theme's colours.

Labels stay crisp, themed, searchable in `gmore`, and copyable. Strokes are
rasterized at their exact final size and are never scaled. Label cells are always
transparent and free of strokes, with a margin around them.

### 2. Image first, then text, with no cursor-up

Ordinary images reserve rows with blank lines, move up with `CUU`, and paint.
A diagram cannot do that, for two reasons:

- A cursor-up stops at the top of the screen, and a diagram can be taller than the
  screen (the README's top-down chain is about 48 rows).
- `gmore` recognizes a band taller than the screen only when the band is made of
  consecutive blank LFs.

So a diagram is emitted as

```
[ESC[<pad>C]  ESC7  <APC … C=1,z=0 …>  ESC8  \r  <text row 1>\n … <text row N>\n
```

The image is placed at the band's top-left, `C=1` and the `ESC7`/`ESC8` pair keep
the cursor still, and the text rows then scroll normally with the image anchored to
them. `gmore` stores the text in its cell grid and the image in its image layer;
on a repaint it writes the text and then places the image.

Direct output therefore draws image-then-text, and a `gmore` repaint draws
text-then-image. The image is sent with `z=0` (over the text) and is transparent
wherever a label sits, so that the order does not matter. The text-over-image probe
(§10 of the field guide) confirmed this on Ghostty, iTerm2, Kitty and VSCode.
`z=-1` is not used: VSCode draws no image at it. Two VSCode findings shape the
emission: spaces written over the image make it disappear, so text rows skip the
band with cursor moves; and a band taller than the screen shows misaligned text,
so such diagrams fall back there.

### 3. An exception to ADR 0002: no `timg`

ADR 0002 keeps `timg` for all image formats; its concern was the quality of
*downscaling* photographs and screenshots. A generated diagram has none of that: it
is rasterized at its final size and is never scaled, so the quality argument does
not apply. mdcat therefore encodes the PNG itself (`src/png_write.cpp`) and writes
the Kitty APC itself, chunked at 4096 bytes with `q=2` on every chunk, per the
field guide's §5 and §8. `timg` stays the path for every other image.

This is safe because the producer controls both ends of the contract: the PNG is
a plain 8-bit RGBA image, the APC format is the one `timg` already produces, and the writer renumbers Kitty ids in document
order (ADR 0003), so ids cannot collide.

### 4. Kitty only

Sixel clobbers whole cells (field guide §4): painting a sixel over a character cell
erases what was there, so text over a sixel cannot work. The built-in renderer
therefore runs only when the graphics backend is Kitty. Sixel terminals keep the
`mmdc` image, or the source.

A terminal that fails the text-over-image probe's case A (text, then image) or case
C (image, then text) is denied the built-in renderer by `TERM_PROGRAM` and falls
back like a sixel terminal.

### 5. Fallback chain

For each block mdcat tries, in order:

1. **builtin**: the renderer described here, when the backend is Kitty and the
   diagram is in the supported subset and passes the legibility gates;
2. **image**: the existing `mmdc` → PNG → `timg` path, when `mmdc` is available;
3. **source**: the Mermaid source in a code panel.

`--mermaid <mode>` / `MDCAT_MERMAID` selects `auto`, `builtin`, `image` or `source`
(plan Step 3). Rejections are reported with `MDCAT_MERMAID_DEBUG=1`. When in doubt,
the builtin falls back: it must never emit an illegible or misleading diagram.

## Consequences

- Diagrams render in milliseconds, with no Node or Chrome, on any Kitty-protocol
  terminal that passes the probe, including over SSH.
- Labels join the text layer: they are searchable in `gmore`, copyable, and take
  the theme's colours.
- mdcat carries a small PNG encoder and rasterizer, and a layout engine for the
  supported flowchart subset. Layouts need not match Mermaid's; they must be
  correct and legible.
- `gmore` gains `C=1` support (plan Step 1); its crop placements need no `z=`.
- Unsupported syntax, and terminals without Kitty graphics, keep today's behaviour.
