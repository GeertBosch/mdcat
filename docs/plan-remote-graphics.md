# Implementation plan — remote graphics (Kitty protocol)

Derived from ADR 0002 (Accepted) and the probe campaign. Goal: full mdcat/gmore
image support over SSH via the Kitty graphics protocol, with sixel kept as the
local non-Kitty fallback. Every phase ends in a working, committed, README-demoed
state.

## Guiding constraints (all probe-verified — do not relitigate)

- **PNG bypasses timg** on the Kitty path: read the `.png`, wrap in a Kitty APC.
  timg (`-pk`) is used **only** for JPEG/GIF/SVG → Kitty PNG. Sixel path unchanged.
- **Unique image id per image** (timg reuses `i=`; a colliding `a=T` re-lays the
  prior placement). Use transmit-once `a=t` + placement `a=p` in gmore.
- **`q=2` on every** transmit and placement command (else the `;OK` reply leaks).
- **`c=`/`r=` is mandatory** to size the image (the terminal scales; we never rely
  on timg `-g`, which it ignores for Kitty).
- **Byte-exact** Kitty/PNG stream handling — never line-oriented edits.
- **Chunk** base64 PNG at 4096 (`m=1` … `m=0`).
- **Cell metrics over SSH** come from `CSI 16t` + `14t` + `18t` (TIOCGWINSZ pixels
  are 0 remotely); the query must enter raw/no-echo **before** emitting.
- **Capability:** `MDCAT_GRAPHICS` override → env allowlist (when present) →
  best-effort probe → **optimistic Kitty default** → text. Env is unreliable over
  SSH (blank under VSCode Remote-SSH), so the default carries the no-env case.

## Phase 0 — Backend selection + cell metrics (no visible change)

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

## Phase 1 — Kitty encoder for mdcat `<img>` + mermaid

1. `std::string kittyImage(const std::string& pngBytes, int cols, int rows, uint32_t id)`:
   base64-encode, chunk at 4096, emit `a=T,f=100,q=2,i=<id>,c=<cols>,r=<rows>` with
   `m=1`…`m=0`. Allocate `id` from a per-run counter (unique).
2. In `renderImageBlock`, branch on backend:
   - **Kitty + source is PNG:** read the file bytes directly (no timg), compute
     `cols`/`rows` from intrinsic size (PNG IHDR via existing `pngWidth`, add
     `pngHeight`) and `cellMetrics`, honoring `width`/`height`/`forceWidthBound` as
     today. Emit via `kittyImage`.
   - **Kitty + non-PNG (jpg/gif/svg):** `timg -pk` → it yields a Kitty PNG APC;
     rewrite its `i=` to our unique id and inject `c=`/`r=` (byte-exact), or decode
     its base64 and re-wrap via `kittyImage`. (Prefer re-wrap for one code path.)
   - **Sixel:** unchanged (`timg -ps`).
3. mermaid: `renderMermaid` already produces a temp `.png`; on the Kitty backend,
   wrap it with `kittyImage` instead of routing through the sixel `renderImageBlock`.
4. Footprint: on the Kitty path we *set* `c`/`r`, so the reserved cell box is known
   directly (no sixel read-back).
5. README demo + commit.

## Phase 2 — gmore Kitty ingest + scroll-clip

1. Parse Kitty APCs from the mdcat stream into the image layer: record
   `{id, row, col, Pw, Pv (from PNG IHDR), footCols, footRows, pngBytes}`. No raster
   decode.
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
