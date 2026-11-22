// Terminal probing: width, graphics backend, cell metrics, background colour and theme.
#pragma once

#include <algorithm>
#include <string>

namespace mdcat {

int fullTerminalWidth();

// The width content should be laid out to: the terminal width less the columns taken by any
// surrounding container-block decoration (gIndent). At least one column so layout never collapses.
int terminalWidth();

// The graphics protocol used to paint inline images. Kitty carries pixel data plus an explicit cell
// footprint (so the terminal does the scaling and a remote host needn't know the local cell size);
// Sixel is pixel-absolute and is the local fallback for terminals that lack Kitty. None means draw
// no images at all (not a terminal, or a known text-only terminal) — an <img> falls back to its alt
// text.
enum class GraphicsBackend { None, Sixel, Kitty };

// Resolve the graphics backend once, cached for the run. Resolution order (most explicit first):
//   1. `--img <kitty|sixel|none>` (gForcedBackend): authoritative, set on the command line.
//   2. $MDCAT_GRAPHICS=kitty|sixel|none — authoritative; works headless and forwards over SSH.
//   3. bare --img (gForceGraphics): force image output even when piped, defaulting to Kitty; kept
//      for the `mdcat --img doc.md | gmore` pipeline.
//   4. Not a terminal -> None (nothing to draw to).
//   5. Env allowlist (no probe): a Kitty-capable terminal we recognize -> Kitty. KITTY_WINDOW_ID
//   set,
//      or TERM_PROGRAM in {ghostty, iTerm.app, vscode}. Apple_Terminal is known sixel-incapable ->
//      None.
//   6. Probe (Kitty a=q + Primary DA in one round-trip): Kitty reply -> Kitty; DA sixel attr ->
//   Sixel;
//      DA reply but neither -> None (the terminal answered and speaks neither, e.g. Apple Terminal
//      over SSH where TERM_PROGRAM is stripped — this is what stops mdcat leaking a Kitty APC to
//      it).
//   7. Optimistic default (TOTAL SILENCE only): Kitty. A user who ran mdcat for graphics and got no
//      probe reply at all (esp. over SSH, where env often doesn't survive — VSCode Remote-SSH
//      presents as a bare xterm) most likely has a Kitty-capable terminal that just didn't answer
//      in time.
GraphicsBackend graphicsBackend();

// Whether any inline-image backend is available (Kitty or Sixel). When false an <img> falls back to
// its alt text or filename. Kept as a thin predicate so existing call sites read clearly.
bool terminalSupportsGraphics();

// Whether the mermaid CLI (mmdc) is on $PATH, so a ```mermaid code block can be rendered as a
// diagram. Probed once via `command -v` and cached. When absent, mermaid blocks fall back to being
// shown as ordinary fenced code. The probe is skipped entirely on terminals that cannot display
// graphics, since the rendered PNG could not be painted there anyway.
bool mmdcAvailable();

// Metrics for converting an image's pixel footprint (read from the sixel it produces) into a number
// of text rows/columns to reserve. A single cell is often a fractional number of pixels (e.g. ~5.83
// px wide), so rounding it to an integer before dividing mis-counts: rounding up under-counts the
// columns the terminal actually uses (image overflows its reserved space), rounding down
// over-counts (leaves a gap). We therefore keep the text-area pixel size and cell count as a ratio
// and do the pixel->cell conversion exactly, matching how the terminal lays a sixel out.
// `cellW`/`cellH` are an integer fallback used only when the area ratio is unavailable.
struct CellMetrics {
    int areaW = 0, areaH = 0;   // text-area size in pixels (0 if unknown)
    int cols = 0, rows = 0;     // text-area size in cells (0 if unknown)
    int cellW = 8, cellH = 16;  // integer px/cell fallback

    // ceil-divide pixels to the cell count the terminal will use, via the precise area ratio when
    // known (px * cells / areaPx, rounded up) and the integer cell size otherwise. Used for the
    // FOOTPRINT: turning a painted sixel's pixel size into the columns/rows it actually occupies,
    // so the layout matches reality (on a HiDPI terminal the real cell is large, e.g. 14x34).
    int pxToCols(int px) const {
        if (cols > 0 && areaW > 0) return std::max(1, (px * cols + areaW - 1) / areaW);
        return std::max(1, (px + cellW - 1) / cellW);
    }
    int pxToRows(int px) const {
        if (rows > 0 && areaH > 0) return std::max(1, (px * rows + areaH - 1) / areaH);
        return std::max(1, (px + cellH - 1) / cellH);
    }

    // The real cell height/width in pixels (from the area ratio when known, else the integer cell).
    int realCellH() const { return rows > 0 && areaH > 0 ? std::max(1, areaH / rows) : cellH; }
    int realCellW() const { return cols > 0 && areaW > 0 ? std::max(1, areaW / cols) : cellW; }

    // A FIXED "nominal" cell used only to turn a REQUESTED (or intrinsic) pixel size into the -g
    // geometry, the same on every terminal. Using a fixed ratio (rather than the real cell) makes a
    // given width/height render at the same size everywhere: on a HiDPI terminal where one cell
    // spans many device pixels, the real cell would map a requested height to too few rows (a tiny
    // image), and on a low-DPI terminal it would render larger than on HiDPI — both undesirable.
    // 8x20 keeps images compact and sharp. The FOOTPRINT still uses the real cell, so columns line
    // up exactly.
    static constexpr int kNominalW = 8;
    static constexpr int kNominalH = 20;
    int pxToColsNominal(int px) const { return std::max(1, (px + kNominalW - 1) / kNominalW); }
    int pxToRowsNominal(int px) const { return std::max(1, (px + kNominalH - 1) / kNominalH); }

    // The cell size timg assumes for `-g<cols>x<rows>` when it has NO terminal to query (its stdin
    // is detached from the tty, as mdcat runs it — see runTimg). Measured from timg 1.6.3+: a -g
    // cell is exactly 9 px wide and ~18 px tall. Because sixel is pixel-absolute, an image timg
    // paints at gCols*9 px then occupies (gCols*9 / realCellW) of the TERMINAL's cells — wider than
    // gCols whenever the real cell is narrower than 9 px (e.g. VSCode's ~6 px), so the sixel
    // overflows its reserved columns. To make timg paint a given number of REAL cells we scale the
    // -g count by realCell/timgCell.
    static constexpr int kTimgCellW = 9;
    static constexpr int kTimgCellH = 18;
    // Convert a desired width in real terminal cells into the -g column count that makes timg paint
    // that many real cells' worth of pixels (cols * realCellW), rounded to nearest. Identity when
    // realCellW==9.
    int colsToTimgCols(int cols) const {
        return std::max(1, (cols * realCellW() + kTimgCellW / 2) / kTimgCellW);
    }
    int rowsToTimgRows(int rows) const {
        return std::max(1, (rows * realCellH() + kTimgCellH / 2) / kTimgCellH);
    }
};

// Query the terminal's background colour with OSC 11 ("ESC ] 11 ; ? BEL") and return it as a timg
// `-b` argument ("#rrggbb"), or empty if the terminal doesn't answer. timg pads a sixel's height up
// to the next 6px band and fills the padding (and any alpha) with its -b colour; the default is
// BLACK, which paints a visible dark line under a light-background image (e.g. a mermaid diagram).
// Filling with the real terminal background instead makes the pad invisible. Cached once per run.
//
// The reply is "ESC ] 11 ; rgb:RRRR/GGGG/BBBB <ST|BEL>" with 1-4 hex digits per channel; we take
// the high byte of each. Done over /dev/tty in raw mode with a short timeout, like queryWindowOp,
// so it reaches the local terminal even when stdout is piped and never hangs or echoes.
std::string queryBackgroundColor();

// Whether the terminal has a DARK background, so code blocks and syntax highlighting should use the
// dark palette (light-gray text on dark-gray, lightened hues) instead of the light one. Decided
// from the OSC 11 background colour's luminance (Rec. 601: a bg darker than mid-gray is "dark").
// When the terminal doesn't answer OSC 11 (headless/piped, or a terminal that ignores it) we
// default to the LIGHT theme, matching mdcat's historical appearance. $MDCAT_THEME=dark|light|auto
// forces the choice. Cached once per run (the underlying query is memoised; this just classifies
// it).
bool darkBackground();

// Select the code-block colour palette (kCodeOn / kLightGray here, plus the highlighter's palette)
// for the terminal theme. Light theme keeps the historical look (dark-gray text on light-gray);
// dark theme uses light-gray text on dark-gray. Called once on the main thread (initTheme) before
// rendering, so every kCodeOn/kLightGray read sees the chosen palette and the OSC 11 query
// (memoised) fires once.
void initTheme();

// Cached cell metrics for the run. Preference order:
//   1. $MDCAT_CELL_W/H and $MDCAT_AREA_W/H overrides — explicit, work headless and forward over
//   SSH.
//   2. The kernel's text-area pixels + cell counts (TIOCGWINSZ ws_xpixel/ws_ypixel with
//   ws_col/ws_row)
//      — the precise area ratio for exact pixel<->cell conversion. Zero over SSH (not forwarded).
//   3. The CSI terminal queries, which reach the LOCAL terminal even over SSH: CSI 18 t (area in
//   cells)
//      + CSI 14 t (area in pixels) together give the same precise area ratio remotely; CSI 16 t
//      gives the integer cell size. We take whatever subset answers.
//   4. Typical defaults.
// Only consulted when an image is actually rendered, so non-image documents never pay for the
// queries.
CellMetrics cellMetrics();

// Whether the terminal is iTerm2 ($TERM_PROGRAM or $LC_TERMINAL).
bool isITerm();

// The X= pixel offset to send with a diagram's image. iTerm draws a Kitty image at the cell's left
// edge without the window's side margin, 5 points by default, so it sits that far left of the text
// grid; X= moves it back (measured with tools/probe-kitty-label-fill.py, round 3: X=10 is flush on
// a 2x display). Points are device pixels over the display scale, guessed as 2 for a cell at least
// 12 px wide. MDCAT_IMAGE_X sets the offset in pixels for another margin setting, or 0 for none.
int imageXOffset(int cellW);

}  // namespace mdcat
