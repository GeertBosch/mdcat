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

### 3. Capability resolution: env override → env allowlist (no probe) → best-effort probe → Kitty default → text

- `MDCAT_GRAPHICS=kitty|sixel|none` (optionally with `MDCAT_CELL_W/H`,
  `MDCAT_AREA_W/H`) is **authoritative** — zero round-trips, works headless and
  forwards over SSH via `SendEnv`/`SetEnv`.
- **Env allowlist short-circuits the probe *when present*.** When the environment
  identifies a Kitty-capable terminal — `KITTY_WINDOW_ID` set, or `TERM_PROGRAM` in
  {`ghostty`, `iTerm.app`, `vscode`} (extensible) — use Kitty **without probing at
  all**. This avoids the round-trip and the **reply-leak** failure mode (see below).
  All three were confirmed Kitty-capable by probe. **But env is unreliable over
  SSH:** it survived an Orbstack hop (shared loopback) yet was **entirely blank**
  over VSCode Remote-SSH (`TERM_PROGRAM`, `COLORTERM`, `KITTY_WINDOW_ID` all empty;
  `TERM=xterm-256color`). So the allowlist is an *optimization for the local/lucky
  case*, **never a requirement** — the probe and optimistic default must carry the
  no-env case. VSCode Remote-SSH (a Kitty-capable terminal presenting as bare xterm)
  is exactly where the **optimistic Kitty default** earns its place.
- Only for an **unrecognized** environment do we fall to a **best-effort runtime
  probe** (Kitty `a=q` query, then DA1 for sixel) with a **short** timeout. The
  round-trip reaches the local terminal over SSH (bytes traverse the pty), but
  **silence is not "no support."**
- On silence, **default to Kitty** (optimistic): a user running mdcat/gmore for
  graphics over SSH almost certainly has a Kitty-capable terminal, and Kitty's
  cell-based placement degrades more gracefully than a wrong sixel guess.
- Fall back to text/alt only when graphics are positively known absent.

This replaces the current `TERM_PROGRAM` denylist in `terminalSupportsGraphics()`.
Note `TERM_PROGRAM` is **not** forwarded over SSH by default, so the allowlist helps
only when it survives (e.g. same-host Orbstack, or `SendEnv`); when it doesn't, the
probe/optimistic-default path covers the case.

**Why the allowlist matters — the reply-leak.** Probing on `ghostty`/Orbstack over
SSH produced a `[]` (no reply) result *and leaked the reply bytes onto the screen*.
Root cause was **not** latency (it was localhost `::1`, sub-millisecond) — it was a
probe-script shell-portability bug (`read -n1 -t` is a bash extension; the remote
`/bin/sh` is `dash`, which blocks for a whole line on a newline-less reply). mdcat
in C++ uses `read()`+`VTIME` (like `queryCellSize16t`) and is immune. But the
episode shows that **not probing when we already know the terminal is the safest
path**, hence the allowlist short-circuit. The probe timeout stays short — latency
is not the failure mode.

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

This was **validated end-to-end** by `tools/probe-kitty-clip.sh` (2026-06-27):
transmitting once and placing top-half / bottom-half source crops rendered exactly
the head-only and base-only bands. **Set `q=2` on every graphics command** (both
transmit and placement) — Kitty otherwise replies `ESC_G…;OK ESC\` to each, which
echoes as stray escapes; timg's transmit already sets `q=2`.

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
(byte-exact splice, **unique image id**) rendered the image correctly scaled to a
~10×4 cell box; a bare native-size timg PNG also rendered correctly. Getting here
took ruling out three causes of an earlier grey-block: a false "terminals
mis-render footprint-less PNGs" theory, a real-but-separate `awk` splice that
dropped a boundary newline (fixed with `perl -0777`), and the **actual** cause —
**timg reuses the same `i=` across renders of a file, so a second `a=T` re-lays the
first placement and corrupts it.** Implication for the implementation:
mdcat/gmore must give each image a **unique id** (or transmit-once `a=t` +
placements `a=p`), and must edit the Kitty byte stream byte-exactly (never with
line-oriented tools).

**SSH + cross-terminal results (2026-06-27):** validated on VSCode, iTerm2, and
ghostty/Orbstack **over SSH** — Kitty support, `c=`/`r=` sizing, and partial-image
source-crop (`tools/probe-kitty-clip.sh`: top/bottom-half crops rendered the head /
base bands) all confirmed; gmore needs no PNG decoder. Set **`q=2` on every**
transmit and placement command (suppresses the `;OK` reply that otherwise leaks).

**Cell metrics over SSH (gate item 2 — CONFIRMED, and stronger than expected):**
`TIOCGWINSZ` pixel fields are 0 remotely, but the `CSI 16t/14t/18t` round-trips
reach the local terminal through the pty and answer with **exact, self-consistent**
metrics (Orbstack: cell 15×30 px; VSCode Remote-SSH: 7×16 px, with 14t÷18t agreeing
to ~7.08×16.25). So the CSI query is the **primary** remote cell-metrics source, not
a mere fallback. **Implementation:** extend `cellMetrics()` to also issue `CSI 14t`
(area px) and `CSI 18t` (area cells) so the precise *area ratio* is available over
SSH, not just the integer `16t` cell — matching the exact px→cell conversion the
local path already uses.

**Env reliability over SSH:** env survived an Orbstack hop but was **entirely blank**
over VSCode Remote-SSH (see decision 3). The env allowlist is therefore an
optimization, never load-bearing; the optimistic Kitty default + CSI metrics carry
the no-env remote case.

Status: **Accepted**, fully validated (local + SSH, three Kitty terminals).
