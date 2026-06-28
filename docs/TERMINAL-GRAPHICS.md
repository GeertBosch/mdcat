# Terminal graphics ground truth — a field guide

> Companion notes for the curious. The [README](../README.md) shows *what* mdcat
> and `gmore` render. This document is the *how we know it works* — the terminal
> behaviour we measured, the dead ends we walked into, and the facts we now treat
> as ground truth when placing images on a terminal.

Rendering Markdown to a terminal is mostly tractable. Putting **images** on a
terminal — and keeping them aligned inside tables, scrolling them in a pager, and
making them survive an SSH hop — is not. None of the relevant behaviour is
specified anywhere we could rely on; terminals disagree, and the disagreements are
exactly where image layout breaks. So we built ~two dozen probe scripts
([`tools/probe-*.sh`](../tools)), ran them on every terminal we target, and wrote
down what actually happened. This is that ledger.

Everything below was verified on real terminals (June 2026) unless marked
otherwise. Terminals exercised: **VSCode integrated terminal**, **iTerm2** (incl.
Retina/HiDPI), **Ghostty** (via Orbstack), **Kitty**, and plain **`xterm -ti vt340`**
— locally and, for the Kitty path, over real SSH.

---

## 1. The two graphics protocols we speak

| | Sixel | Kitty graphics protocol |
|---|---|---|
| Carrier | `ESC P q … ESC \` (DCS) | `ESC _ G <controls> ; <base64> ESC \` (APC) |
| Pixel model | **pixel-absolute** — terminal places pixels 1:1 | **cell-aware** — image carries a cell footprint (`c=`/`r=`) |
| Sizing input | needs the local cell px size | terminal does the cell-fitting itself |
| Works over SSH | only if the local cell size is known | **yes** — footprint travels with the image |
| Our producer | `timg -ps` | `timg -pk` |

We support **both**. Sixel is the universal fallback (even `xterm -ti vt340`);
Kitty is preferred when available because it carries an explicit cell footprint,
which is the key to remote sessions (§6) and to table alignment (§5).

The internal model is **raster-first**: one RGBA raster is canonical, and sixel +
Kitty are both *encoders* of it. See
[ADR 0002](adr/0002-remote-graphics-kitty.md) for the full architecture.

---

## 2. Cells, pixels, and the 6-vs-14 problem

A terminal is a grid of character cells; an image is pixels. Converting between
them is the root of most image bugs.

- **Cell size is queryable at runtime.** `CSI 16 t` → `ESC[6;<h>;<w>t` gives the
  cell pixel size; `CSI 14 t` (text-area px) and `CSI 18 t` (text-area cells)
  cross-check it (areaPx ÷ areaCells = cell px). On VSCode we measured cell =
  **6 px wide × 14 px tall**; other terminals differ, and HiDPI reports *physical*
  pixels.
- **The cell is usually fractional.** ~5.83 px wide is common. Rounding it to an
  integer *before* dividing mis-counts columns (round down → gaps; round up →
  overflow). Convert with the exact area ratio instead:
  `cols = ceil(px·areaCols/areaPx)`. (See `cellMetrics()` /
  `pxToCols`/`pxToRows`.)
- **Sixel bands are 6 px; cells are not a multiple of 6.** 14 px tall ≠ 4·6.
  This is *the* obstacle for any line-at-a-time pager — you cannot fill a 14 px
  row with whole 6 px sixel bands. The fix is in §4.

### Two cell sizes for two jobs (DPI independence)

On a Retina display, sizing an image by the real (physical) cell makes it render
tiny on HiDPI and large on low-DPI — the same document looks different on every
screen. So we split the two jobs:

- **Footprint** (painted pixels → cells, for column layout): uses the **real**
  cell, because the terminal paints into real pixels.
- **Geometry request** (how big to ask `timg` to draw): uses a **fixed nominal**
  cell (8×20), the same everywhere, so an image of a given size looks consistent
  across terminals.

---

## 3. Capture-and-replay, and the cursor

The single most important placement decision: **capture the producer's bytes and
replay them where *we* put the cursor** — never let `timg` write straight to the
tty.

- Run straight to the tty and `timg` positions the image *itself*, ignoring our
  cursor. That's wrong for grids (tables) where we need precise column placement.
- Capture the bytes (`popen`) and replay them at the current cursor, and the image
  lands wherever we moved the cursor. Column/row placement becomes pure cursor
  math — no CPR round-trips needed at paint time.

**Placement is made terminal-independent with DECSC/DECRC** (`ESC 7` … `ESC 8`).
After a sixel, terminals disagree on where the cursor ends up: **VSCode advances to
the row below the image; iTerm2 does not.** A bare replay that assumed one of these
broke on the other. Wrapping the image in save/restore makes the cursor land back
exactly where it started regardless of how the terminal advanced — so the *caller*
controls the layout, not the terminal.

`timg`'s own output embeds the painted pixel size in the sixel raster header
(`ESC P q "Pan;Pad;Ph;Pv`). We parse `Ph`/`Pv` to get the **real** footprint —
the `-g WxH` request is only a *max box*; aspect-fit can make the image smaller, so
the request is not the footprint.

---

## 4. Paging images: the 18-px strip, and scroll asymmetry

`gmore` reveals images one text row at a time. The 6-vs-14 problem (§2) means a
cell row can't be filled by whole sixel bands — so we **paint 18 px (= ceil(14/6)·6)
per text row, top-down**, and let each row's strip overwrite the ~4 px overspill of
the row above it with *identical* pixels. `probe-pager-stack.sh` proved this is
pixel-perfect; a 12 px control variant shows visible seams (confirming the test is
sensitive).

Scroll behaviour we measured — and it is **asymmetric**:

- **Down-scroll is cheap and incremental.** Full-screen scroll up (LF at the last
  row) carries existing sixels cleanly; paint one new strip at the bottom. No
  repaint needed.
- **Up-scroll must full-repaint the visible window.** Painting a strip clobbers the
  *whole* cells it touches, including the live neighbour below — harmless going
  down (overspill goes off-screen), but going up it clips the strip below it.
  `probe-overlap-order.sh` isolates this: top-down paint is clean, bottom-up clips.
- **Never use a DECSTBM scroll region.** Region scrolls move text but mangle sixel
  positions. The status line must be a transient bottom row repainted each frame,
  not a pinned region.
- **`ESC[2J` *does* erase sixels** on VSCode (we checked, because an up-scroll bug
  looked like sixel persistence — it wasn't).
- **`ESC c` (RIS) wipes scrollback** on VSCode, so forward paging must *not* use
  it — it append-scrolls only the newly revealed rows. RIS is reserved for
  backward/jump repaints. (And see §7: RIS also makes the terminal forget Kitty
  images.)

**External `less` is dead for images.** `less -R` passes SGR + OSC-8 only; a sixel
DCS is neither, so it prints the bytes as text. That's why `gmore` exists.

---

## 5. Images in tables: pin the footprint to the laid-out width

Table cells are the hardest case: several images on one grid row, each confined to
a column. Overlap here was the most persistent bug, with three distinct causes —
all the same lesson.

`timg -pk` (piped) sizes its `-g` box at a **fixed ~9 px/cell** and emits **no**
`c=`/`r=`. So the terminal lays the PNG out by dividing its pixels by *its own*
(different) real cell — a "33-cell" image becomes ~38 real cells and overflows the
column. **Fix: inject `c=`/`r=` ourselves** so the footprint is explicit:

- `c` = the column's final width; `r` = aspect-derived from the same painted pixels
  and real-cell ratio (round to nearest, not ceil — ceil distorted small images).
- **iTerm2 requires *both* `c` and `r`** and honours them exactly. `c` alone draws
  nothing; `c` + `r` stretches to fit. So always emit both
  (`probe-kitty-aspect.sh`).

The deeper lesson, learned three times:

1. Pin the footprint to the **final laid-out column width**, not the render-time
   width cap. Column width is only known *after* fair-share + tightening, so re-pin
   every image's `c=` at the end of table layout.
2. An interactive `mdcat file` is a TTY, so it routes through **`gmore`**, not the
   direct write path. Any image-sizing fix must be applied in **both** mdcat (the
   piped producer) **and** gmore (the interactive consumer) — and gmore must
   **honour the producer's `c=`/`r=`** rather than recomputing from pixels.

---

## 6. Over SSH: why Kitty, and how sizing still works

`sshd` does not forward `ws_xpixel`/`ws_ypixel`, so `TIOCGWINSZ` returns **0** for
pixels on the remote host. Sixel is pixel-absolute and `timg` refuses to emit it
without geometry — so the naive sixel path is dead remotely. Kitty wins because the
image carries its own cell footprint; the *local* terminal does the sizing.

Two findings make this robust:

- **`CSI 14t`/`16t`/`18t` round-trips reach the local terminal over the pty and
  answer over SSH.** Even though `TIOCGWINSZ` pixels are 0, the cell-size query
  works (measured self-consistent to the exact pixel over Orbstack/Ghostty). So
  the CSI query is the **primary** remote cell-metrics source, not just a fallback.
- **Environment variables do *not* reliably survive.** VSCode Remote-SSH (a
  flagship remote case) presents as bare `xterm-256color` with `TERM_PROGRAM`,
  `COLORTERM`, `KITTY_WINDOW_ID` all blank. Orbstack/Ghostty (shared loopback)
  kept them. So env survival is transport-dependent and **unreliable**.

Capability detection therefore can't lean on env alone. The chosen order:

```
MDCAT_GRAPHICS override  →  env allowlist (TERM_PROGRAM ghostty/iTerm.app/vscode,
                            KITTY_WINDOW_ID)  →  probe (Kitty a=q + Primary DA)  →
                            OPTIMISTIC KITTY DEFAULT on TOTAL SILENCE  →  text
```

**The probe must distinguish "speaks neither" from "no one answered."** A naive
"Kitty `a=q`, default to Kitty on no reply" leaks: **Apple Terminal over SSH**
(where `TERM_PROGRAM` is stripped, so the env allowlist's `Apple_Terminal` guard
never fires) ignores the Kitty query, has no sixel, and would receive a Kitty APC
it can't render — printing `a=q,f=24,s=1,v=1;AAAA` as text. Worse, Apple Terminal
*echoes the inner bytes* of the APC it passes through, so even the probe itself
leaks if echo isn't off.

So the probe sends the Kitty query **and Primary DA (`CSI c`) in one round-trip**:

- Kitty `;OK` in the reply → **Kitty**.
- else DA reply contains the sixel attribute `;4;` → **Sixel**.
- else DA replied but neither → **None** (text). *Every* real terminal answers DA,
  so a DA reply with no Kitty/sixel is a definite "speaks neither" — this is the
  Apple Terminal case, and it now falls back to text instead of leaking.
- else **total silence** → optimistic **Kitty** (the genuinely env-blank
  Kitty-capable remote, e.g. VSCode Remote-SSH, that just didn't answer in time).

Echo is disabled **before** writing the queries, so any reply (or APC pass-through)
is consumed in no-echo raw mode, never painted on screen. `MDCAT_GRAPHICS=kitty|
sixel|none` (or `--img <proto>`) is the escape hatch; `MDCAT_CELL_W/H` and
`MDCAT_AREA_W/H` override the metrics.

**`timg -pk` facts** that the encoder relies on:

- Emits a Kitty APC with `f=100` (= **PNG**, not raw RGB). The inner PNG's IDAT is
  **DEFLATE-compressed** (the early "uncompressed PNG" assumption was refuted) — a
  full decoder must inflate. `gmore` sidesteps this: it parses only the 16-byte
  PNG **IHDR** for pixel dimensions and never decodes the raster.
- `timg -pk` requires `-g WxH` when it can't read the terminal (non-tty), and
  **does** downscale into that box, with interpolation. We keep `timg` for the
  downscale specifically because VSCode's terminal scales with nearest-neighbour
  (visibly jaggy) when handed a native-size PNG.
- `timg` does **not** emit `c=`/`r=`, so we prepend our own (no conflict). It
  reuses the same `i=` across invocations of one image, so we **rewrite `i=` to a
  unique id** — a later `a=T` with a duplicate id re-lays earlier placements and
  corrupts them.

---

## 7. Hard-won gotchas (the ones that cost a round-trip)

These are the traps that *looked* like deep terminal bugs and were not. Listed so
we don't re-walk them.

- **Write granularity, not bytes.** A multi-chunk Kitty image leaked its base64 as
  text — but the emitted bytes were provably correct (`cmp` against a saved file
  that `cat` rendered perfectly). Cause: `std::cout`'s ~8 KB buffer flushed
  *mid-chunk*, splitting a 4096-byte APC chunk across two `write()`s; a live
  terminal seeing a chunk arrive in pieces gives up and dumps the partial base64.
  **Rule:** when a terminal garbles graphics but the captured bytes are correct,
  suspect streaming/buffering. Emit graphics with chunk-contiguous writes: never
  flush mid-chunk. The non-tty path streams **per block** (a `StreamingSink`
  streambuf that drains to `write()` only on `flush()`, which `render()` calls at
  each top-level block boundary) so text and finished images appear incrementally
  instead of the whole document being withheld until the slowest image's subprocess
  returns — and because a block emits its whole APC in one go and the flush happens
  at a line boundary between blocks, no chunk is ever split. (The tty/pager path
  still buffers the entire document: gmore scrolls over the complete text.) Probes
  that `cat` a complete stream *cannot* reproduce the original split-chunk bug.
- **`q=2` on every Kitty command.** Without `q=1`/`q=2`, the terminal replies
  `ESC_G…;OK ESC\` to each graphics command and the `OK` echoes onscreen as stray
  escapes. Set `q=2` on **every** transmit *and* placement APC.
- **RIS forgets Kitty images.** `ESC c` resets the terminal's image memory, so any
  transmit-once cache must be cleared whenever RIS is emitted, or placements
  reference an id the terminal no longer has → nothing drawn.
- **Never edit the graphics byte stream with `awk`/`sed`.** They are line-oriented
  and silently drop/move the `\n` that `timg` emits inside its APC, mis-placing the
  image. Splice byte-exactly with `perl -0777` or Python `bytes`.
- **`grep -c $'\x1b_G'` lies** — it counts matching *lines*, not matches, reporting
  "1 chunk" for a 30-chunk image. Count with Python `.count(b'\x1b_G')`.
- **A stale remote binary mimics every bug.** Twice, an SSH "regression" was just
  the remote running an old binary. `gmore_core.h` is a **header**, so `git pull`
  alone changes nothing — `gmore.cpp` must be **recompiled**. Before re-debugging
  any remote graphics symptom: confirm the remote was rebuilt after the pull and
  `PATH` isn't shadowing it.
- **Probe shell portability.** Reading a terminal reply with `read -n1 -t` is a
  bash extension; Ubuntu's `/bin/sh` is dash, which ignores it and blocks for a
  whole line — but a Kitty reply has no newline, so it never returns and the reply
  leaks later. Probes detect the shell and fall back to a `dd`+`stty raw` slurp.
  (The C++ code uses `read()` + `VTIME` and is unaffected.) Also: enter raw/no-echo
  mode **before** writing a query escape, or the reply gets echoed.

---

## 8. The probes

The probe scripts in [`tools/`](../tools) are durable, runnable proof of every
claim above. They are throwaway-grade (ImageMagick allowed, shell heuristics) and
exist to be re-run on a new terminal before trusting it. Notable ones:

| Probe | Establishes |
|---|---|
| `probe-cell-size.sh` | `CSI 16t`/`14t`/`18t` cell-size query + cross-check |
| `probe-pager-stack.sh` | 18 px strips reassemble pixel-perfect (§4) |
| `probe-pager-fullscreen.sh` | down-scroll clean / up-scroll gaps (§4) |
| `probe-overlap-order.sh` | top-down paint clean, bottom-up clips (§4) |
| `probe-scroll-sixel.sh` | full-screen scroll carries sixels |
| `probe-clear-scrolled-sixel.sh` | `ESC[2J` erases sixels |
| `probe-kitty-graphics.sh` | basic Kitty transmit/place + capability reply |
| `probe-kitty-rawpng.sh` | wrap a source PNG in a Kitty APC (no timg) |
| `probe-kitty-clip.sh` | source-crop `x,y,w,h` + `c,r` scroll-clip (gmore's mechanic) |
| `probe-kitty-aspect.sh` | iTerm needs **both** `c` and `r` (§5) |
| `probe-kitty-cell-footprint.sh` | overlap without `c=`/`r=` vs the fix (§5) |
| `probe-kitty-multichunk-*.sh` | multi-chunk transmit + footprint + placement |
| `probe-remote-cellinfo.sh` | CSI queries answer over SSH (§6) |

`tools/probe-common.sh` holds the shared raw-mode `term_query` helper (with the
dash workaround above).

---

## Related design records

- [ADR 0001 — gmore data model](adr/0001-gmore-data-model.md): cell-grid emulator
  vs line spans.
- [ADR 0002 — remote graphics over SSH](adr/0002-remote-graphics-kitty.md): the
  raster-first, Kitty-passthrough architecture.
- [gmore rendering notes](gmore-rendering-notes.md): the sixel encode/decode and
  paint-path invariants in `gmore.cpp`.

---

> **Maintainers:** keep this document honest. When a probe overturns a belief, or
> an architectural change shifts the ground truth (a new protocol, a new placement
> strategy, a new terminal in the support matrix), update the relevant section here
> in the same change — see the maintenance note in [CLAUDE.md](../CLAUDE.md).
