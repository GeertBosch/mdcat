# Implementation plan — remote graphics (Kitty protocol)

Derived from ADR 0002 (Accepted) and the probe campaign. Goal: full mdcat/gmore
image support over SSH via the Kitty graphics protocol, with sixel kept as the
local non-Kitty fallback. Every phase ends in a working, committed, README-demoed
state.

## Guiding constraints (all probe-verified — do not relitigate)

- **Keep timg for all formats including PNG.** mdcat runs `timg -pk -g<cols>x<rows>`,
  which downscales with proper interpolation and emits a well-formed, chunked Kitty
  APC (`a=T,q=2,f=100` + `q=2,m=` continuations). mdcat only **rewrites the `i=` on
  the first chunk** to a unique id; no PNG reading/chunking/base64 in mdcat. (Passing
  a native-size PNG and letting the terminal scale via `c=`/`r=` was rejected: VSCode
  scales without anti-aliasing → aliased output. If we ever drop timg we must do our
  own interpolated downscale first.) Sixel path unchanged (`timg -ps`).
- **Unique image id per image** (timg reuses `i=`; a colliding `a=T` re-lays the
  prior placement). Use transmit-once `a=t` + placement `a=p` in gmore.
- **`q=2` on every** command — transmit, placement, **and every chunked
  continuation** (`m=` chunks also reply `;OK` without it). timg already does this
  for its own output; gmore's hand-built `a=p` placements must set it explicitly.
- **Aspect ratio:** timg's `-g<cols>x<rows>` preserves aspect when it downscales, so
  the mdcat path inherits correct AR for free. gmore's crop placements still compute
  `c`/`r` from the (already-downscaled) PNG's `imageW/cellW`, `imageH/cellH` via the
  `cellMetrics` area ratio — cells are not square (~7×16 px), so never hardcode `c:r`.
- **`c=`/`r=` is mandatory** to size the image (the terminal scales; we never rely
  on timg `-g`, which it ignores for Kitty).
- **Byte-exact** Kitty/PNG stream handling — never line-oriented edits.
- **Chunk** base64 PNG at 4096 (`m=1` … `m=0`).
- **Cell metrics over SSH** come from `CSI 16t` + `14t` + `18t` (TIOCGWINSZ pixels
  are 0 remotely); the query must enter raw/no-echo **before** emitting.
- **Capability:** `MDCAT_GRAPHICS` override → env allowlist (when present) →
  best-effort probe → **optimistic Kitty default** → text. Env is unreliable over
  SSH (blank under VSCode Remote-SSH), so the default carries the no-env case.

## Phase 0 — Backend selection + cell metrics (no visible change) ✅ DONE

1. `enum GraphicsBackend { None, Sixel, Kitty }` and `graphicsBackend()`:
   - `MDCAT_GRAPHICS=kitty|sixel|none` wins.
   - else env allowlist → Kitty: `KITTY_WINDOW_ID` set, or `TERM_PROGRAM` ∈
     {`ghostty`,`iTerm.app`,`vscode`} (also keep the `Apple_Terminal`→none case).
   - else best-effort Kitty `a=q` probe over `/dev/tty` (raw/no-echo first, short
     timeout, drain), then DA1 for sixel.
   - else **default Kitty** when stdout is a tty; `None` when not.
   - Replaces the `TERM_PROGRAM` denylist in `terminalSupportsGraphics()`.
2. Extend `cellMetrics()`:
   - keep `TIOCGWINSZ` (best, local);
   - add `CSI 14t` (area px) + `CSI 18t` (area cells) alongside `16t`, so the exact
     **area ratio** is available over SSH, not just the integer cell;
   - honor `MDCAT_CELL_W/H`, `MDCAT_AREA_W/H` overrides first.
3. Commit. No output change yet; verify via `MDCAT_DEBUG_CELL` and a debug print of
   the chosen backend.

## Phase 1 — Kitty output for mdcat `<img>` + mermaid ✅ DONE

(Implementation note: also generalized the image-placement gate from
`isSixelImage` to `isImageBlock` so Kitty images flow through the paragraph,
table-cell, and mermaid emit paths; `replaySixel`/`emitImageParagraph` already
work for both protocols since neither moves the cursor predictably. gmore-side
Kitty ingest + scroll-clip remains Phase 2.)

1. `runTimgKitty(path, geom)`: like the existing `runTimg` but `-pk` instead of
   `-ps`. Returns timg's chunked Kitty APC bytes.
2. `kittyRewriteId(bytes, id)`: byte-exact rewrite of the `i=` value on the first
   `ESC_G` controls segment to a unique per-run id (`s/i=\d+/i=<id>/` on first match
   only). No other edits — timg already sets `q=2`, `f=100`, chunking.
3. In `renderImageBlock`, branch on backend:
   - **Kitty:** compute the `-g<cols>x<rows>` box exactly as the sixel path does
     today (intrinsic size via `pngWidth`/add `pngHeight`, `cellMetrics`,
     `width`/`height`/`forceWidthBound`); `runTimgKitty` → `kittyRewriteId` → emit.
     The footprint (reserved cells) is the `cols`/`rows` we requested. Works for
     PNG/JPEG/GIF/SVG uniformly (timg decodes + interpolated-downscales all).
   - **Sixel:** unchanged (`runTimg -ps`).
4. mermaid: `renderMermaid` already produces a temp `.png`; route it through the same
   `renderImageBlock` Kitty branch (it's just another image file).
5. README demo + commit.

## Phase 2 — gmore Kitty ingest + scroll-clip

1. Parse Kitty APCs from the mdcat stream into the image layer: record
   `{id, row, col, Pw, Pv (from PNG IHDR), footCols, footRows, pngBytes}`. No raster
   decode. (Pw,Pv is timg's already-downscaled size, e.g. 225×221 — exactly the right
   source dimensions for the crop, since placements crop *that* transmitted PNG.)
   Reassemble chunked APCs (`m=1`…`m=0`) before parsing the IHDR.
2. On first paint of an image: transmit once (`a=t,f=100,q=2,i=<id>` + chunked PNG).
3. Paint visible band with a placement (`a=p,q=2,i=<id>` + source crop `x,y,w,h` +
   `c,r`), per the clip math:
   - fully visible → `x=0,y=0,w=Pw,h=Pv`, `c=footCols,r=footRows`;
   - bottom-cut → `y=0,h=Pv·visRows/totRows`, `r=visRows`;
   - top-cut → `y=Pv·hidRows/totRows,h=remaining`, `r=visRows`.
4. Repaint/scroll: delete placements with `a=d,d=i,i=<id>` and re-place — replaces
   the sixel re-encode path; revisit the open up-scroll bug.
5. Keep the sixel ingest/replay path for sixel streams. Commit.

## Phase 3 — Hygiene, docs, fallback polish

1. Terminal-state repair: ensure cursor-show (`ESC[?25h`) after rendering; never
   leave modes changed.
2. `none`/text fallback wired through both programs.
3. README "Remote / SSH graphics" section: `MDCAT_GRAPHICS`, `SendEnv MDCAT_*`,
   what works headless. Update ADR 0002 status notes if anything shifts.
4. Tests: add a Kitty-output golden (controls + chunking shape) to `make check`;
   keep sixel goldens.

## Open / deferred (explicitly out of scope for now)

- Compressed-PNG edge cases beyond what timg/terminals handle (ADR notes f=100 PNG
  is always DEFLATE; we pass through, terminal inflates).
- Animated GIF (timg → single Kitty PNG frame for now).
- Removing the sixel path (kept as local fallback per decision).
