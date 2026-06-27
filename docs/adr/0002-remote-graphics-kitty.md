# ADR 0002 — Remote graphics over SSH: Kitty protocol, raster-first, PNG passthrough

- Status: Accepted (VSCode probe confirmed the core premise; see "Validation gate")
- Date: 2026-06-27
- Component: `mdcat` (image rendering) and `gmore` (the graphics-aware pager)

## Context

Image rendering in mdcat/gmore is **sixel end-to-end** today: mdcat shells out to
`timg -ps`, captures the sixel bytes, reads the painted pixel size back from the
sixel raster header, and reserves cells by dividing those pixels by the local
terminal's cell size (from `TIOCGWINSZ` `ws_xpixel/ws_ypixel`, else `CSI 16t`).
gmore decodes that sixel to an RGBA raster for layout and replays the original
sixel bytes verbatim.

This breaks over SSH (and similar remote sessions) for two independent reasons:

1. **No local cell pixel size.** `sshd` does not forward `ws_xpixel/ws_ypixel`, so
   `TIOCGWINSZ` returns 0 for pixels on the remote host. Sixel is pixel-absolute:
   without the local cell size we cannot convert an image's pixels into the right
   number of cells, and `timg` refuses to emit sixel at all when it cannot
   determine geometry.
2. **Protocol availability.** `timg` will only emit sixel; the terminals our users
   actually run remotely (VSCode's terminal, iTerm2, Orbstack) speak the **Kitty
   graphics protocol**, which sixel-incapable contexts do not.

Sixel itself is a transparent byte stream — SSH carries it fine. The problem is
producing it and sizing it without the local cell geometry.

## Decision

Adopt the **Kitty graphics protocol** as a second backend, chosen by capability,
with these sub-decisions:

### 1. Kitty cell-based placement is the lever

Kitty's display command carries pixel data **plus an explicit cell footprint**
(`c=`columns, `r=`rows): the *local* terminal scales the image into that many
cells. The remote host therefore never needs the local cell pixel size — it just
asks for `c=W,r=H`. This dissolves problem (1) for layout.

### 2. Raster-first architecture, two encoders

The canonical internal image is an RGBA **raster**; **sixel and Kitty are both
encoders** of it. gmore already decodes sixel→raster, so this unifies the two
programs rather than forking them. (But see decision 4: for the Kitty path we do
not actually need a decoded raster in the common case.)

### 3. Capability resolution: env override → best-effort probe → Kitty default → text

- `MDCAT_GRAPHICS=kitty|sixel|none` (optionally with `MDCAT_CELL_W/H`,
  `MDCAT_AREA_W/H`) is **authoritative** — zero round-trips, works headless and
  forwards over SSH via `SendEnv`/`SetEnv`.
- Otherwise a **best-effort runtime probe** (Kitty `a=q` query, then DA1 for
  sixel). The probe round-trip *does* reach the local terminal over SSH because the
  bytes traverse the pty — but it is latency-fragile, so **silence is not "no
  support."**
- On silence, **default to Kitty** (optimistic): a user running mdcat/gmore for
  graphics over SSH almost certainly has a Kitty-capable terminal, and Kitty's
  cell-based placement degrades more gracefully than a wrong sixel guess.
- Fall back to text/alt only when graphics are positively known absent.

This replaces the current `TERM_PROGRAM` denylist in `terminalSupportsGraphics()`,
which is wrong over SSH (`TERM_PROGRAM` is not forwarded).

### 4. Keep `timg`; pass its PNG through verbatim — gmore needs no PNG decoder

To limit scope we keep using `timg`, invoked as `timg -pk` (Kitty output).

**Verified facts** (probed 2026-06-27): `timg -pk` emits a Kitty APC
`ESC _ G a=T,…,f=100,m=0 ; <base64> ESC \` whose payload is a **PNG (`f=100`)**,
not raw RGB — and the PNG's IDAT is **DEFLATE-compressed** (BTYPE=2). `--compress`
controls only the outer transport zlib, not the inner PNG. `-g` is ignored for
Kitty when stdout is not a tty (timg emits native pixels; the terminal does the
cell fit). So the earlier assumption that timg emits *uncompressed* PNG is false.

Rather than inflate DEFLATE, mdcat/gmore **pass timg's PNG through verbatim** and
let the terminal decode it. The Kitty protocol's
display-a-transmitted-image-cropped primitive makes this sufficient even for a
partially-scrolled image in gmore:

- Transmit each image's PNG **once** (`a=t`, image id `i=`).
- Paint the visible band with a **placement** (`a=p`) using a *source-pixel crop*
  (`x,y,w,h`) scaled into a *cell footprint* (`c,r`). With images bounded to ≤ the
  screen, exactly one edge truncates at a time:
  - **bottom-cut** → crop the top slice (`y=0, h=Pv·visRows/totRows`) into
    `r=visRows`;
  - **top-cut** → crop the bottom slice (`y=Pv·hidRows/totRows`) into `r=visRows`.
- Delete/repaint via `a=d,d=i,i=<id>` — no per-band re-encode.

gmore needs only the image's pixel dimensions (Pw,Pv) for the crop math, and gets
them by parsing the **PNG IHDR** — a 16-byte plaintext field right after the
signature, *before* the compressed IDAT. **No inflate, no full PNG decode.**

**mdcat must inject `c=`/`r=` to control display size.** Probing showed `timg -pk`
ignores `-g` for the PNG: it always emits the image at *native resolution* and sets
*no* `c=`/`r=`. A bare timg Kitty APC therefore displays at native pixel size; to lay
the image out in a known cell box (the whole point of the remote story) mdcat must
compute and inject `c=`/`r=` from its own `cellMetrics` footprint math (intrinsic
pixels from the PNG IHDR). (A bare native-size image still renders *correctly* — see
the retraction below; the footprint governs SIZE, not correctness.)

**Retraction.** An earlier draft claimed a footprint-less PNG renders as distorted
"grey mush" while one with `c=`/`r=` is correct. That was a **probe artifact, not
terminal behavior**: the probe spliced `c=`/`r=` into timg's binary APC with `awk`,
which is line-oriented and dropped the newline timg emits between the APC terminator
and a trailing `ESC[?25h`, mis-placing the image. With a byte-exact splice
(`perl -0777`) both the footprinted and the bare-native images render correctly.
**Lesson for the implementation: never edit the Kitty/PNG byte stream with
line-oriented tools — operate on raw bytes.**

## Consequences

- **mdcat:** add a Kitty backend that wraps/forwards `timg -pk` output, supplying
  `c=`/`r=` from its own footprint math (since timg won't size headless). Sixel
  path unchanged.
- **gmore:** the grid's image layer shifts from a decoded `Image.px` raster to
  image **metadata** `{id, row, col, Pw, Pv, footCols, footRows}` for the Kitty
  path. This is a *simplifying* change and the crop-by-id repaint may fix the open
  up-scroll repaint bug (ADR-era sixel re-encode in `replaySixel`/`encodeSixel`).
- The sixel path stays for local terminals that lack Kitty (e.g. xterm-vt340) and
  as the `MDCAT_GRAPHICS=sixel` choice.
- A wrong optimistic Kitty guess on a non-supporting terminal leaves stray bytes;
  the escape hatch is `MDCAT_GRAPHICS`. Terminal-state repair (e.g. re-showing the
  cursor after `timg`'s `ESC[?25l`) is a Phase-1 hygiene task.

## Validation gate (probes must pass before implementation)

`tools/probe-kitty-graphics.sh` and `tools/probe-remote-cellinfo.sh`, run in
VSCode/iTerm2/Orbstack both locally and over SSH, must confirm:

1. Kitty `c=`/`r=` sizing scales an image to a cell box independent of its pixels
   (the remote-layout premise). If false, fall back to env-var cell metrics.
2. `CSI 14t`/`16t` still answer over SSH (the cell-size fallback is real).
3. Whether an unsupported terminal leaves visible garbage on a Kitty image (sets
   how loud the optimistic default may be).

If (1) fails the core premise collapses and this ADR must be revised before
coding.

**Result (VSCode, 2026-06-27):** (1) CONFIRMED — a timg PNG with `c=10,r=4`
spliced in (byte-exact) scaled to ~10×4 cells; a bare native-size timg PNG also
rendered correctly. (An intermediate run showed a distorted block, traced to an
`awk` splice corrupting the byte stream — see the retraction in decision 4, not a
terminal limitation.) Status promoted to Accepted. SSH-side runs (2) still pending
and do not block the local architecture decision.
