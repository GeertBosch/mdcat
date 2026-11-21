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

**Detach `timg`'s stdin (`</dev/null`).** `popen("r")` only captures stdout; stdin
stays on the controlling tty, and `timg` then puts the terminal into raw mode
(no ECHO/ICANON) to query it, restoring on exit. That is harmless serially, but once
several `timg` processes run **concurrently** (ADR 0003 image pool) their
save/restore races — each saves whatever state it happened to read, and the last to
exit restores a stale "echo off", leaving the user's shell with no echo after e.g.
`mdcat --img README.md | cat`. Redirecting stdin from `/dev/null` stops `timg`
touching terminal modes entirely; we already pass an explicit `-g` box and protocol,
so there is nothing for it to auto-detect. (`mmdc` gets the same `</dev/null` for
the same reason.)

**A detached `timg` no longer knows the terminal's cell size, so for sixel we
scale the `-g` box ourselves.** `-g` is in *character cells*, and `timg` converts
cells→pixels with whatever cell size it can find. Run headless (stdin detached) it
can't query the terminal and falls back to a **fixed 9 px wide × 18 px tall** cell —
measured from timg 1.6.3+: `-g33x` paints exactly 297 px (= 33 × 9). Sixel is
**pixel-absolute**: those 297 px then occupy 297 ÷ *the real cell width* of the
terminal's columns. On VSCode's ~6 px cell that's ~50 columns, not 33 — the image
overflows its reserved column into its neighbours (the symptom: oversized images
bleeding across table cells). So `runTimg` rescales each `-g` axis from real cells to
timg cells (`gCols = round(cols × realCellW / 9)`, `gRows` likewise with /18) before
invoking `timg`, making the painted pixels land on exactly the columns mdcat reserved.
Identity when the real cell is 9 px; a no-op for Kitty, which instead pins the
footprint after the fact (`kittyRewriteFootprint`) so the terminal does the scaling.

**Pass the terminal background to `timg -b`, or sixel padding paints black.** Sixel
data is written in 6-px-tall **bands**, so `timg` rounds an image's height *up* to the
next multiple of 6 (a 70 px mermaid diagram → 72 px raster) and fills those extra rows
— and any alpha — with its `-b` colour. The default is **black**, which draws a hard
dark line under a light-background image. The fix is image-agnostic: query the
terminal's background once with **OSC 11** (`ESC ] 11 ; ? ST` → `rgb:RRRR/GGGG/BBBB`,
done over `/dev/tty` like the cell-size queries so it survives a piped stdout) and pass
it as `timg -b '#rrggbb'`, so the pad matches the terminal and disappears. When the
terminal doesn't answer (headless/piped) `-b` is omitted and timg keeps its default —
harmless, since nothing is displayed there. Kitty needs none of this: it sends the PNG
verbatim with no banding or pad. (`MDCAT_BG` overrides the query for tests.)

**That same OSC 11 background also drives the code-block theme.** The query is
memoised, so it reaches the terminal **exactly once** per run (warmed on the main
thread before the image pool starts); `darkBackground()` classifies its Rec. 601 luma
(`< 128` ⇒ dark) and `initTheme()` picks the palette. Light: light-gray panel, dark
text, saturated highlight hues. Dark: dark-gray panel, light-gray text, the *same*
hues lightened. The highlighter holds two fixed `Palette` structs and flips one pointer
(`setHighlightTheme`); the reset-to-fg in each must match the panel fg (236 light / 252
dark). No answer ⇒ light (historical default); `$MDCAT_THEME=dark|light` forces it.

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
- **RIS rings the bell on xterm.** Real `xterm` treats RIS as a power-on reset and
  beeps — the same `^G` you hear from `reset(1)` — so every backward/jump repaint
  chirps. No escape sequence suppresses just the bell; the xterm resource
  `XTerm*bellOnReset: false` (in `~/.Xresources`) does. In gmore, `avoidRIS()`
  instead swaps RIS for a quiet `ESC[3J ESC[2J ESC[H` erase: auto-selected when
  `XTERM_VERSION` is set, and forced either way by `GMORE_NO_RIS` (`1`/`0`). The
  quiet erase does **not** drop a relocated sixel raster (RIS does), so a
  scrolled-off image can ghost — the accepted trade for a bell-free repaint, and
  why RIS stays the default off xterm. The Kitty delete-all + re-transmit is
  independent of the clear choice, so Kitty images stay clean under both.

**External `less` is dead for images.** `less -R` passes SGR + OSC-8 only; a sixel
DCS is neither, so it prints the bytes as text. That's why `gmore` exists.

**An image taller than the screen is windowed, not shrunk.** `renderImageBlock`
does not clamp `-g` to the terminal's row count — an image (or Mermaid diagram)
can reserve more rows than fit in one screen. `gmore` scrolls through it like any
other content: each repaint re-derives the visible vertical slice from the scroll
offset and re-places only that band, without re-invoking `timg` or re-encoding
pixels. Kitty does this with a payload-free `a=p` placement that re-crops the
already-transmitted image via `x=/y=/w=/h=` (`kittyPlace`,
gmore_emulator.cpp); sixel has no native source-rectangle, so the same band is
sliced verbatim out of the original raster bytes by rewriting `Pv` and cutting
the kept rows (`replaySixel`, gmore_emulator.cpp). Landed in `ec33233` (edge
clipping) and `6ff3249`/`9d564e1` (Kitty transmit-once + crop placement,
`probe-kitty-clip.sh`). A prior attempt to instead cap image height to the
screen at render time was reverted — it fights this mechanism and needlessly
shrinks diagrams that already scroll correctly.

The producer reserves such a band as N newlines, `CSI N A`, DECSC, the image,
DECRC, `CSI N B`, and N can exceed the screen height. A cursor-up clamped at
the screen edge cannot reach the reserved band's logical start in scrollback.
gmore recognizes this image placement and anchors it at the start of the
reserved band instead, while retaining normal clamped cursor movements. Otherwise
the image is anchored too low and overlaps the following text.

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

### 5a. The same overlay, degenerate to one row: leading-image headings/paragraphs

A heading or paragraph that *starts* with a Markdown image followed by more text
(`# ![Logo](logo.png) Project Name` — a common README pattern) reuses this exact
reserve-then-overlay technique (`renderLeadingImageBlock`), just with a single
image column and a single text column instead of a full table grid: text lines
are laid out first with the image's column reserved as blank padding, then the
image is painted into that reserved space via relative cursor moves bracketed by
DECSC/DECRC (`replaySixel`). Two differences from the table case:

- The image is **capped to a few text-lines' height** (currently 3), since a
  heading logo sized to its own aspect ratio could otherwise be arbitrarily tall.
  The cap is enforced by requesting a smaller height and re-rendering — up to a
  few retries — because the requested pixel height (nominal cell) and the
  measured footprint (real cell) round independently, so a single attempt at
  exactly the cap can still land one row over.
- Unlike a table (which needs every image's footprint before it can lay out
  columns, so all of a table's images are joined together), a leading image is
  the only thing sizing its block, so it can defer through the ordinary
  single-slot SlotSink path (ADR 0003) like a standalone image paragraph.

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
*draws the inner bytes* of the APC itself, so even the probe's own query leaks. Turning
echo off is not enough: that only stops the tty driver from echoing the replies, and
Terminal.app over SSH (to an OrbStack VM) still showed the text. So the probe takes a cursor
position report (`CSI 6n`) before and after its query, and if the cursor moved, moves back
(`CUP`) and erases from there (`ED`). The width probe (section 8) cleans up the same way.

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

## 7. Remote images: fetch it ourselves, don't let timg do it

The CI badge at the top of the README is a remote `https://…/badge.svg`. Two
ground-truth facts shaped how mdcat renders it.

**timg's URL support is a trap on the image path.** timg links `libav`, which
*can* open `http(s)` URLs — but only through the **video** subsystem (`-V`). The
**image** path (GraphicsMagick / librsvg, which is what a static `.svg`/`.png`
goes through) opens the argument as a **local file**: handed a URL it reports
`No such file or directory` and emits nothing. Forcing `-V` on a static SVG
didn't fetch it either on the build we measured (timg 1.6.3+, ffmpeg libav 62).
So "let timg fetch the URL" is not reliable for badges. mdcat fetches the bytes
itself with `curl` into a temp file and hands timg an ordinary **local** path —
the exact pipeline used for `<img src="local.png">`. (`tools/probe-remote-image.sh`
records the timg-URL failure and the curl→timg success.)

**Fetching is a trust boundary, so it is gated.** Viewing a Markdown file must
not pull from arbitrary servers. mdcat fetches only over **https** and only when
the URL's host is on a trust list built from (1) the git remotes of the rendered
file's repository — both `git@host:…` and `https://host/…` forms are parsed for
their host — and (2) `$XDG_CONFIG_HOME/mdcat/trusted-hosts` (bare host = exact;
`.host`/`*.host` = subdomains too). No hosts are trusted by default. The check is
on the **initial** URL's host only; `curl -L` then follows redirects to other
hosts (GitHub's `badge.svg` 302s to a `*.githubusercontent`/pipelines host),
which is the accepted cost of making badges work. `curl` runs `--proto '=https'`,
with a connect+total timeout and a `--max-filesize` cap so a render can't hang or
pull an unbounded body. Anything refused falls back to alt text, identical to a
non-graphics terminal. The temp file is unlinked on every return path (an RAII
guard in `renderImageBlock`).

---

## 8. Hard-won gotchas (the ones that cost a round-trip)

These are the traps that *looked* like deep terminal bugs and were not. Listed so
we don't re-walk them.

- **A row offset can come from text, not images.** On VSCode, a built-in Mermaid
  diagram paged through gmore had every label one row above its hole in the image, and
  Ctrl-L did not help. Every Kitty placement gmore makes was correct in isolation
  (`tools/probe-kitty-place.py`, cases A–F all aligned). Replaying the exact bytes of
  a README session (`GMORE_KEYS=fffq` at the user's 96×70) reproduced it. Cause: a
  README line *below* the diagram, with emoji ZWJ and VS16 sequences. mdcat measures
  each grapheme as 2 columns (96 total), while VSCode's xterm.js measures code points
  one by one (104), so the terminal autowrapped it onto an extra row. Every row above
  it moved up one, while the image, placed by `CUU` from the bottom of the window,
  stayed put. Ghostty, iTerm and Kitty cluster graphemes like mdcat, so they were fine.
  **Fix:** measure text the way the terminal does. `tools/probe-grapheme-width.py` on
  VSCode 1.145 showed it simply adds up each code point's exact East Asian Width
  (0 for Mn/Me/Cf and Hangul medial/final jamo, 2 for W/F, else 1): ❤️ 1, 👩‍🚀 4,
  👨‍👩‍👧‍👦 8, 🏳️‍🌈 3 (🏳 is narrow), 🇺🇸 2 (each flag half is narrow), 👍🏽 4, 1️⃣ 1.
  It answers DECRQM for grapheme mode 2027 with "not recognised". Apple Terminal
  455.1 (no DECRQM reply) adds up code points the same way, except that a ZWJ and the
  enclosing keycap mark U+20E3 take one column each: 👩‍🚀 5, 👨‍👩‍👧‍👦 11, 🏳️‍🌈 4,
  🧑‍💻 5, 1️⃣ 2, and still ❤️ 1, 👍🏽 4, 🇺🇸 2.
  `src/unicode_width.h` has two modes: `Grapheme` (the old rule: a cluster is as wide
  as its widest part, VS16 makes it 2) and `CodePoint` (the parts add up, from the
  exact table in `src/unicode_width_table.h`, generated by `tools/gen-unicode-width.py`,
  with the ZWJ and enclosing-mark widths as parameters). A cluster is never split
  across lines in either mode. mdcat and gmore choose at startup (`src/width_probe.h`):
  `MDCAT_WIDTH` (`grapheme`, or `codepoint[,zwj=N][,enclosing=N]`); else, when stdout
  is the terminal, three samples measured in one exchange with `CSI 6n` before and
  after each: 👩‍🚀 (2 = grapheme, more = code point), `a`+ZWJ and `1`+U+20E3 (their
  extra columns are the two parameters); else the values measured above, by
  `TERM_PROGRAM` (`vscode`, `Apple_Terminal`). gmore's emulator
  gives a follower with a width its own cells in `CodePoint` mode, so its grid
  wraps exactly where the terminal does. **Rule:** when text and image disagree by
  whole rows, look for a line the terminal wraps, and check its widths with the probe.

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
- **`q=1` on every Kitty command — and don't trust timg to set it.** Without a
  quiet flag the terminal replies `ESC_G…;OK ESC\` to each graphics command and the
  `OK` echoes onscreen as stray escapes; over SSH the replies pile up in the remote
  pty and spill onto the shell prompt after the command exits. **timg's Kitty header
  is not uniform across versions:** macOS timg 1.6.3 emits `a=T,i=<n>,q=2,f=100,m=0`
  (id + quiet flag present), but Debian's timg 1.4.3 emits a bare `a=T,f=100,m=0` —
  **no `i=` and no `q=`**, so the terminal defaults to image id 0 *and* q=0 (always
  reply). The leaked replies read `ESC_Gi=0;OK ESC\` — id 0 is the tell. So mdcat
  must *insert* `i=` (a unique per-image id; without it every image collides on 0)
  and `q=1`, not merely rewrite them: `kittyRewriteId` and `kittyQ1` both insert when
  the key is absent, and first chunks are detected by the action key `a=` (not `i=`,
  which older timg omits). gmore's own placements set `q=2` directly.
- **RIS forgets Kitty images.** `ESC c` resets the terminal's image memory, so any
  transmit-once cache must be cleared whenever RIS is emitted, or placements
  reference an id the terminal no longer has → nothing drawn.
- **…but on iTerm2, RIS wipes the raster while leaving stale *placements* behind.**
  A single small image survives RIS cleanly (an early probe said so). But with
  several images on a large window (gmore's repaint after a not-found search, 55×272),
  a bare `ESC c` followed by re-transmit + re-place leaves the old placements alive
  and they **ghost at the top of the screen** — while the freshly placed copies also
  render in their correct band. Ghostty and VSCode were unaffected. The cure is an
  explicit Kitty **delete-all** right after RIS: `ESC_Ga=d,d=A,q=2 ESC\` (`q=2` so the
  delete reply doesn't leak). This was isolated with a `GMORE_REPAINT_TEST` flag:
  re-placing *without* re-transmitting still ghosted (proving RIS kept the
  placements), and adding `a=d,d=A` fixed it. Note: `C=1` on the placement (suppress
  cursor advance) did *not* help — the ghosts are stale placements, not a scroll.
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

## 9. The probes

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
| `probe-remote-image.sh` | timg can't fetch a URL on the image path; curl→timg works (§7) |
| `probe-kitty-text-overlay.sh` | text over a transparent Kitty image, both paint orders (§10) |
| `probe-kitty-label-fill.py` | a text label on a filled node: image hole + SGR background, six variants (§10) |
| `probe-kitty-place.py` | gmore's `a=p` re-placements (repaint, top crop, window bottom, `C=1`, 1-row slices) against text rows (§8, §10) |
| `probe-grapheme-width.py` | columns the terminal gives ZWJ, VS16, flag and skin-tone graphemes, via `CSI 6n` (§8) |

`tools/probe-common.sh` holds the shared raw-mode `term_query` helper (with the
dash workaround above).

---

## 10. Text over Kitty images

The built-in Mermaid flowchart renderer ([ADR 0004](adr/0004-builtin-mermaid-kitty.md))
draws shapes and arrows as **one transparent Kitty image** and writes the labels as
ordinary text in the cells over it. It relies on how a terminal stacks text and an
image in the same cells, which nothing in this document measured until now. Both
paint orders occur in practice:

- **Direct output** (mdcat to the terminal) draws the image, then the text.
- **A `gmore` repaint** draws the text, then the image.

The image was first designed with `z=-1` (under text) and is fully transparent wherever
a label sits, so in theory the order is irrelevant. `tools/probe-kitty-text-overlay.sh`
checks that on each terminal. Its test image is 24×4 cells (a rounded box and two
arrows, with two guaranteed-transparent 8-cell label spans), and every case clears
the screen first:

| Case | What it does | Question |
|---|---|---|
| A | label rows, `CUU`, `ESC7`, image `z=-1`, `ESC8` | text then image (a `gmore` repaint): are the labels visible? |
| B | case A with `z=0` | does transparency alone keep the labels visible? |
| C | `ESC7`, image `C=1,z=-1`, `ESC8`, then label rows written with cursor moves | image then text (direct output): does writing text erase image cells, or the whole image? |
| D | case C, but spaces are printed across the whole band | do default-background spaces hide or erase the image? |
| E | cases A and C with one label on a non-default background (a code span) | does a cell background hide the image strokes, or the label? |
| F | case C with a 3-screen-tall image and band, then scroll back | do image and labels stay aligned over the whole height? |
| G | `C=1`, no `ESC7`/`ESC8`, a marker printed right after the APC | does `C=1` keep the cursor still? |
| H–L | cases C–G again with `z=0` instead of `z=-1` (H=C, I=D, J=E, K=F, L=G) | on a terminal that does not draw `z=-1` images, does `z=0` work in every case? |

**Results** (run the probe, then fill in a cell with `OK` or the failure):

| Terminal | A | B | C | D | E | F | G | H | I | J | K | L |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Ghostty | OK | OK | OK | OK | OK | OK | OK (see note) | OK | OK | OK | OK | OK |
| iTerm2 | OK | OK | OK | OK | OK | OK | OK (see note) | OK | OK | OK | OK | OK |
| VSCode | No image | OK | No image | No image | No image | No image | No image | OK | No image | OK | Text shifted downward | OK |
| Kitty | OK | OK | OK | OK | OK | OK | OK (see note) | OK | OK | OK | OK | OK |

Notes:

- **Case G:** on Ghostty, iTerm2 and Kitty, `C=1` keeps the cursor still: `]` lands
  right after `[`, and `AFTER` appears below the box. The probe's marker text
  (`<- cursor`) sits on the band's first row, so the box outline runs behind it. The
  text is drawn over the strokes, as `z=-1` requires, so this is expected and not a
  failure.
- **Case G placement:** the image is anchored where the cursor is when the APC
  arrives, which is after `[` and before `]`. So the box starts under the `]`, on
  every terminal. An earlier caption said "under the `[`"; that was a probe error,
  not a terminal difference (the Ghostty screenshot shows the same).
- **VSCode** draws no image at `z=-1` (A, C–G). With `z=0` it draws it in cases B,
  H (image then text, written with cursor moves), J (coloured label background) and
  L (`C=1` keeps the cursor still), so a `z=0` image works there. Three findings
  shape the emission:
  - **Case I fails:** printing spaces over a `z=0` image makes it disappear. Text
    written after the image must skip non-label cells with cursor moves (case H),
    never write spaces across the band.
  - **Case K fails:** with a tall band (3 screens) the text is shifted downward
    relative to the image. Until that is understood, a diagram taller than the
    screen is not safe on VSCode; treat it as a fallback condition there.
  - `z=0` puts the image over the text. Labels stay readable only because their
    cells are transparent, which every terminal measured so far honours (B).

**Conclusion.** `z=0` passes every case on Ghostty, iTerm2 and Kitty (H–L), and on
VSCode passes B, H, J and L. So the builtin renderer sends `z=0` everywhere, writes
label runs with cursor moves rather than spaces (VSCode case I), and falls back for
bands taller than the screen on VSCode (case K).

**Filled nodes.** The image is over the text, so an opaque node fill would hide its label.
The PNG therefore leaves the label's cells transparent, and the label text carries the
fill as its SGR background (case J), with U+00A0 for blanks because plain spaces are
skipped on output. Not yet probed on VSCode: whether U+00A0 cells behave like case J.

**iTerm draws the image about half a cell left of the text grid** (measured with
`tools/probe-kitty-label-fill.py`: the node's left edge is at x≈0 while text column 0 starts at
x≈5, with the window's side margin). The transparent label hole moves with the image, so its left
edge exposes the terminal's own background in the cell before the label, as a dark bar. How the
cells were skipped (CUF, CHA, a default-background NBSP, two moves) made no difference.

The offset is fixed with the Kitty `X=` key (a pixel offset inside the first cell), which iTerm
honours: probe round 3 drew opaque images at `X=` 0, 3, 5, 7 and 10 above a row of text cells in
the same colour, and `X=10` was flush on a 2x display, the default 5 pt side margin in device
pixels. So on iTerm mdcat sends `X=` of 5 pt times the display scale, taken as 2 for a cell at
least 12 px wide; `MDCAT_IMAGE_X` overrides it. A vertical offset (the 2 pt top margin) was not
measured and is not corrected. gmore re-places each image itself with a payload-free `a=p` crop,
so it reads the `X=` from the producer's `a=T` and sends it again on every placement; without that
the paged diagram slid back left of its text.

**gmore's re-placements on VSCode.** `tools/probe-kitty-place.py` checks the ways gmore places a
diagram against text rows: text first then `a=t` + an `a=p` crop with no `C=1` (a repaint), the
top rows cropped off, the image ending at the window's last row, the same with `C=1`, and 1-row
slices as rows scroll in. All of them line up on VSCode (1.145.2, 8×16 cells). VSCode's xterm.js
writes a Kitty image into the cells from the cursor down with real line feeds, and restores the
cursor afterwards only for `C=1`; it scales the image to `r` × its CSS cell height, which may be
fractional while `CSI 16t` reports it rounded (16.5 → 17 at line height 1.1, in a headless run of
VSCode's bundled add-on). Neither mattered here; a row offset seen
there was a wrapped text line (section 8).

---

## Related design records

- [ADR 0001 — gmore data model](adr/0001-gmore-data-model.md): cell-grid emulator
  vs line spans.
- [ADR 0002 — remote graphics over SSH](adr/0002-remote-graphics-kitty.md): the
  raster-first, Kitty-passthrough architecture.
- [ADR 0003 — parallel image conversion jobs](adr/0003-parallel-image-jobs.md): the
  thread pool, deferred slots, and writer thread that convert images in parallel —
  the concurrency that forces the `</dev/null` stdin detach in §3.
- [ADR 0004 — built-in Mermaid flowcharts](adr/0004-builtin-mermaid-kitty.md): the
  two-layer (text over one Kitty image) diagram renderer that §10 gates.
- [gmore rendering notes](gmore-rendering-notes.md): the sixel encode/decode and
  paint-path invariants in `gmore.cpp`.

---

> **Maintainers:** keep this document honest. When a probe overturns a belief, or
> an architectural change shifts the ground truth (a new protocol, a new placement
> strategy, a new terminal in the support matrix), update the relevant section here
> in the same change — see the maintenance note in [CLAUDE.md](../CLAUDE.md).
