# gmore — sixel rendering & pager notes

Implementation notes for the graphics-aware pager in [`gmore.cpp`](../gmore.cpp).
These are the non-obvious invariants behind sixel decode/encode and the pager's
paint paths — the things that are easy to break in a refactor. For the choice of
the cell-grid data model, see [adr/0001-gmore-data-model.md](adr/0001-gmore-data-model.md).

## Sixel colour is a PERCENTAGE (0–100), not 0–255

A sixel colour register `#n;2;R;G;B` specifies the RGB components as **percentages
0–100**, not 8-bit 0–255. This cuts both ways:

- `decodeSixel()` expands the incoming 0–100 to the raster's 8-bit RGBA:
  `component * 255 / 100`.
- `encodeSixel()` must invert that back to 0–100 **with rounding**:
  `(component * 100 + 127) / 255`.

Emitting raw 0–255 makes terminals clamp anything > 100 to white, which washes out
and speckles every image. The round-trip `0–100 → 0–255 → 0–100` is exact with the
rounding above (verified: zero colour drift between source and re-encoded streams).
Do **not** "simplify" the encoder to pass 0–255 through.

## One clipped sixel per image, not a stack of per-row strips

An image paints as a SINGLE sixel on its topmost visible grid row, clipped to the
visible window — not as a stack of one-cell-tall strips, one per grid row.

The old per-row-strip scheme relied on each sixel advancing the cursor exactly one
cell so the next row's strip overwrote the prior's ~4px overspill (a strip is
`ceil(cellH/6)*6` px, e.g. 18 for cellH=14, so it overspills the cell it sits in).
That holds on some terminals but NOT iTerm2, where a sixel leaves the cursor on the
*same* row (see `tools/probe-cursor-after-sixel.sh`), so the strips tore. A single
sixel stacks its own 6px bands internally and renders seamlessly everywhere, so we
emit one — replaying the producer's ORIGINAL bytes verbatim (highest fidelity; see
the replaySixel note below) and letting `replaySixel` band-clip the top (rows
scrolled above the window) and bottom (rows past `viewBot`). Both directions only
drop whole bands, never colours.

## renderRow brackets each sixel in DECSC/DECRC, not relative cursor moves

For each image visible on grid row `r`, `renderRow()` emits:
`ESC 7` (DECSC, save cursor) → optional `CSI <col+1> G` → the (clipped) sixel →
`ESC 8` (DECRC, restore cursor). The caller's single `\n` then advances one cell row.

Using DECSC/DECRC — rather than a relative `CSI <rows> A` to climb back — is
essential: terminals differ in where the cursor lands after a sixel, and with
multiple images on one grid row (a table) a guessed relative move drifts. The
save/restore returns the cursor regardless of how far the sixel advanced it. (This
is also why a mid-paint scroll breaks it — see the first-paint note below.)

## DECSC/DECRC must save & restore `top`, not just the screen-relative cursor

This is the subtle one. mdcat places **table-cell and standalone images** bracketed
in DECSC/DECRC (see `replaySixel` / `emitImageParagraph` / the table overlay pass in
`mdcat.cpp`): up to the band top, across to the column, `ESC 7` + sixel + `ESC 8`,
then down to the band bottom.

When gmore parses such a bracketed sixel, `finishSixel()` advances the cursor and may
`scrollUp()` — which bumps `top` (the absolute index of screen row 0). If DECRC
restores only the screen-relative `cr/cc/pen` and leaves `top` advanced, every image
after the first in a table row anchors one (or more) rows lower → a visible downward
drift across the row. The fix: `decsc()` also saves `top` (field `stop`); `decrc()`
restores it and calls `ensure()`. This makes the bracket truly position-neutral,
matching the producer's intent.

timg does **not** bracket its sixels (it uses a grid protocol of sixel + absorbed LF
+ `CSI A` up + `CSI C` right), so this change does not affect timg streams.

Debug aid: `gmore --imginfo <stream>` prints `image N @row,col WxHpx CxRcells` for
every decoded image — the tool that diagnoses anchor drift (a correctly aligned
table row shows the same `@row` for all its images).

## Pager paint paths: first paint reserves, every scroll full-repaints

Now that each image paints as ONE clipped sixel (not a stack of per-row strips —
see the renderRow note above), an incremental scroll can't cleanly relocate a
sixel whose top has moved off-screen. So **every move full-repaints** the window:

- **Scroll** (`advance` down — space/`f`/`j`/Enter — and `retreat` up — `b`/`k`/`y`)
  and **Ctrl-L redraw** all share one `repaint()`: wipe with a **full reset
  (`ESC c` / RIS)**, then paint `[viewTop, viewTop+pageH)` top-down, each row's image
  clipped to that window. RIS — not `ESC[2J` — because on VSCode's terminal `ESC[2J`
  erases text cells but NOT a relocated sixel raster, so scrolled-off images ghosted
  at the top; `ESC[3J`, soft reset, cell-overwrite and a cover sixel all left
  artifacts too. Only RIS fully clears the raster (verified live). gmore relies on no
  state RIS disturbs (it re-emits every SGR/colour per row, uses no scroll region or
  custom charset; raw mode is kernel-side).

- **First paint** is the exception — it must NOT clear the screen (short output that
  doesn't page should leave the surrounding scrollback intact, e.g.
  `mdcat --img doc | head -50 | gmore`). Instead it **reserves rows** first: print N
  newlines, then `CSI N A` to home back up to the top of the freshly-cleared region,
  then paint there. N = `pageH` (pager path) or `total` (short, fits-one-screen path).

  Why reserve: gmore models a clean grid from row 0, but the REAL terminal may start
  non-blank with the cursor near the bottom (repro:
  `clear; for j in $(seq 38); do echo $j; done; ./mdcat …`). Painting top-down from a
  low cursor makes each tall sixel force the terminal to scroll to make room — and a
  scroll MID-ROW shifts the screen out from under the `ESC7`/`ESC8` (DECSC/DECRC)
  bracket around each image, so the 2nd+ images in a table row cascade down one row
  each. Reserving guarantees room below so no row emission scrolls; it's the same trick
  mdcat's `emitImageParagraph` uses (newlines reserve a band — scrolling existing
  content into scrollback — then a relative `CSI N A` back up). Net cursor motion is
  down N / up N / down N while painting, so the blank-screen case is unchanged.

  A later page-up/down repaints via RIS and so masks the bug — which is exactly why the
  symptom was "first paint only."

## Dump / debug modes

- `--dump` — render the text grid only (no images). Deterministic; the surface for
  the emulator tests.
- `--dump-images` — text + re-encoded sixel strips. The surface for render tests.
- `--imginfo` — decoded image metadata + ASCII raster; anchor-drift debug tool.

Cell pixel size (needed to size strips) is taken from env `GMORE_CELLW`/`GMORE_CELLH`
(pinned in tests for determinism), else the kernel's `TIOCGWINSZ` pixels, else a
`CSI 16t` query over `/dev/tty` (VSCode reports size only this way), else 8×16.
