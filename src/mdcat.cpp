// mdcat - render a GitHub Flavored Markdown subset to an ANSI terminal.
//
// This is a C++17 port of the earlier markfun.sh. Rather than rewriting text with regular
// expressions, it parses the input in two phases that mirror the GFM specification
// (https://github.github.com/gfm):
//
//   1. Block parsing splits the document into a sequence of blocks: ATX headings, thematic
//      breaks, fenced and indented code blocks, GFM tables, and paragraphs (the leaf blocks),
//      plus the container blocks — block quotes and lists — whose stripped contents are rendered
//      recursively (a quote gets a left rule; a list item gets a bullet/number marker and a
//      hanging indent). Blank lines separate blocks; the lines of a paragraph are gathered
//      together so they can be reflowed.
//   2. Inline parsing turns the raw text of headings, paragraphs and table cells into styled
//      runs: emphasis (*x* / _x_), strong emphasis (**x**), code spans (`x`) and links
//      ([text](url)). Code-fence contents are emitted verbatim, not inline-parsed.
//
// Styling is applied with ANSI escape sequences; link targets use OSC 8 hyperlinks. Widths used
// for table layout and paragraph wrapping are measured in display columns, ignoring escapes.
//
// Only the elements the original script dealt with are implemented, made more solid and more
// conformant. As an exception to the "no inline HTML" rule, a paragraph or table cell that
// consists solely of an <img ...> tag pointing at a local PNG is rendered as an actual image by
// shelling out to timg(1) on terminals that support sixel graphics; elsewhere (e.g. Apple's
// Terminal.app) it falls back to the tag's alt text or filename. See renderImageBlock. Supported
// formats: PNG, JPG, GIF, SVG. As a second exception, an inline <br> tag (in any of the <br>,
// <br/>, <br /> spellings) is rendered as a hard line break wherever it appears — see
// scanHardBreak. All other inline HTML is passed through literally. LaTeX math between $...$
// (inline) or $$...$$ (block) is transliterated to Unicode on a best-effort basis — Greek letters,
// common operator and relation symbols, super/subscripts, and \mathrm/\mathbf wrappers; see
// renderMath and scanMath. A ```mermaid fenced code block is rendered as a diagram via the mermaid
// CLI (mmdc) when it and a graphics terminal are available; otherwise it falls back to ordinary
// code. See renderMermaidBlock.
//
// Build:  c++ -std=c++17 -O2 -o mdcat mdcat.cpp
// Usage:  mdcat [--width N] [--img[=kitty|sixel|none]] [--] [file ...]   (reads standard input when
//         given no file arguments)
//         --width N forces the render width, overriding $COLUMNS and the terminal size.
//         --img forces image output even when piped; an optional protocol pins the graphics
//         backend.

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gmore_run.h"
#include "highlight.h"

namespace {

// ---------------------------------------------------------------------------
// ANSI / OSC helpers
// ---------------------------------------------------------------------------

constexpr char kEsc = '\033';

const std::string kBoldOn = "\033[1m";
const std::string kBoldOff = "\033[22m";
const std::string kItalicOn = "\033[3m";
const std::string kItalicOff = "\033[23m";
// Code-block colours depend on the terminal theme (see initTheme, called from main once the OSC 11
// background is known). Light theme: dark-gray text on light-gray. Dark theme: light-gray text on
// dark-gray. Both are set before any rendering, so reads during rendering see the chosen palette.
std::string kCodeOn =
    "\033[48;5;254;38;5;236m";  // light-gray bg, dark-gray fg (light-theme default)
const std::string kCodeOff = "\033[39;49m";
const std::string kReset = "\033[0m";
std::string kLightGray = "\033[38;5;250m";  // table row separators; lightened on a dark theme
const std::string kQuoteBar = "\033[38;5;244m▎\033[0m ";  // the left rule drawn before quoted lines

// An explicit width from the --width/-w command-line flag, or <= 0 if none was given. Set by main()
// before any rendering, and so before terminalWidth() is first called and caches its result.
int gWidthOverride = 0;

// Force inline image output even when stdout is not a terminal (--img). Lets mdcat be used as an
// image source when piped, e.g. `mdcat --img doc.md | gmore` for visual pager testing. Set by
// main() before rendering, so before terminalSupportsGraphics() caches its result. Cell-size
// detection still works over /dev/tty, and the terminal width is read from $COLUMNS / stderr's
// TIOCGWINSZ.
bool gForceGraphics = false;

// Backend explicitly chosen on the command line via `--img <kitty|sixel|none>`. When set, this is
// authoritative (same force as MDCAT_GRAPHICS), so `mdcat --img sixel doc.md | gmore` pins the
// protocol regardless of terminal or env. Unset (-1) means a bare `--img` with no protocol
// argument: the historical "force output, default to Kitty when piped" behaviour. Set by main().
int gForcedBackend = -1;  // -1 = unset; else a GraphicsBackend value

// Directory of the file currently being rendered, used to resolve relative image paths.
// Empty string means the current working directory. Set by main() before each render() call.
std::string gFileDir;

// Resolve an OSC 8 link target. Absolute URLs (those with a scheme like http://, or a
// fragment/mailto/etc.) are emitted unchanged so they stay portable; a relative path is rewritten
// to an absolute file:// URL anchored on the rendered file's directory, so terminals can open it on
// click. A bare in-document fragment (#anchor) is left alone — there's nothing local to point at.
std::string resolveLinkTarget(const std::string& url) {
    if (url.empty() || url[0] == '#') return url;
    // A URL scheme is letters/digits/+/-/. followed by ':' before any '/' — leave those alone.
    for (size_t i = 0; i < url.size(); ++i) {
        char c = url[i];
        if (c == ':') return url;  // has a scheme (http:, mailto:, file:, ...)
        if (!(std::isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.')) break;
    }
    std::string path = url;
    if (path[0] != '/') {
        std::string base = gFileDir;
        if (base.empty() || base[0] != '/') {
            char cwd[4096];
            if (getcwd(cwd, sizeof(cwd))) {
                std::string c(cwd);
                base = base.empty() ? c : c + "/" + base;
            }
        }
        if (!base.empty()) path = base + "/" + path;
    }
    return "file://" + path;
}

// Columns currently consumed by container-block decorations (the block-quote left rule, a list
// item's marker indent). Every nested container raises it by the width of its prefix while its
// content is being rendered, so the width that paragraphs reflow to and that tables and rules fill
// shrinks to match the space the prefix leaves. Restored on the way out. See emitBlockQuote /
// emitList / terminalWidth.
int gIndent = 0;

// Current bullet-list nesting depth, used to pick the bullet glyph (● ○ ▪︎ by depth, like GitHub's
// disc/circle/square). Raised while a bullet list's items render and restored after. Ordered lists
// don't change it — their marker is the number, not a depth-varying glyph. See emitList.
int gListDepth = 0;

// Nesting depth across *all* lists (bullet and ordered). Used so only the outermost list block gets
// the two-space left lead that sets a list off from surrounding text; nested lists already step in
// via their parent item's marker. Raised around any list's items and restored after. See emitList.
int gListNesting = 0;

// Set just before rendering the content of a *tight* list item. A tight list draws no blank line
// between its items, and likewise its item bodies should not put a blank line between the item's
// lead paragraph and an immediately-following nested list (GFM renders a tight item's paragraph
// without <p> spacing). render() reads this once at entry to suppress its own top-level block
// separators, then clears it so blocks nested deeper (and recursive renders) separate normally. See
// emitList.
bool gTightItem = false;

// The full terminal width, before any container indentation is taken out. Determined once and
// cached (see the comment in terminalWidth). Callers want terminalWidth(), which subtracts the
// indent.
int fullTerminalWidth() {
    // Determined once and cached for the rest of the run: the width is fixed for our purposes and
    // there is no reason to repeat the syscall for every block and every reflowed line.
    //
    // Precedence, most explicit first:
    //   1. --width / -w on the command line (gWidthOverride): an unconditional override.
    //   2. $COLUMNS, when set to a positive value: lets the caller force a width even when a
    //   terminal
    //      is attached (e.g. `COLUMNS=44 mdcat ... | less`). Note bash does not export COLUMNS by
    //      default, so it must be exported or passed inline to reach us.
    //   3. The kernel window size of whichever standard stream is a terminal.
    //   4. A fixed default when none of the above applies (e.g. fully piped with no COLUMNS).
    static const int width = [] {
        if (gWidthOverride > 0) return gWidthOverride;
        if (const char* cols = std::getenv("COLUMNS")) {
            int w = std::atoi(cols);
            if (w > 0) return w;
        }
        for (int fd : {STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO}) {
            struct winsize ws;
            if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
                return static_cast<int>(ws.ws_col);
        }
        return 100;  // sensible default when not attached to a terminal
    }();
    return width;
}

// The width content should be laid out to: the terminal width less the columns taken by any
// surrounding container-block decoration (gIndent). At least one column so layout never collapses.
int terminalWidth() {
    return std::max(1, fullTerminalWidth() - gIndent);
}

// The graphics protocol used to paint inline images. Kitty carries pixel data plus an explicit cell
// footprint (so the terminal does the scaling and a remote host needn't know the local cell size);
// Sixel is pixel-absolute and is the local fallback for terminals that lack Kitty. None means draw
// no images at all (not a terminal, or a known text-only terminal) — an <img> falls back to its alt
// text.
enum class GraphicsBackend { None, Sixel, Kitty };

// Probe sentinel: total silence (no reply at all) is distinct from a definite "supports neither".
// A terminal that answers Primary DA but advertises neither Kitty nor sixel is Unsupported; a
// terminal that answers nothing is Unknown, and the caller may fall back to an optimistic default.
enum class ProbeResult { Unknown, Unsupported, Sixel, Kitty };

// Best-effort graphics capability probe over /dev/tty. Sends, in ONE raw-mode round-trip:
//   1. a Kitty query graphic (a=q) -> terminal replies ESC _ G ... ; OK ESC \ if it speaks Kitty;
//   2. Primary DA (CSI c)          -> EVERY real terminal replies ESC [ ? ... c, and a
//   sixel-capable
//      one includes attribute ";4;". DA is the anchor: it tells us the terminal is THERE and lets
//      us distinguish "speaks neither" (Apple Terminal: DA reply, no OK, no ;4;) from "no one
//      answered".
// Modeled on queryCellSize16t: raw mode with a short timeout, over the controlling tty so it works
// even when stdout is piped, and — crucially — echo is disabled BEFORE the queries are written so
// any reply (or an APC a non-Kitty terminal echoes through) is consumed in no-echo mode, not
// painted on screen. We read until BOTH the Kitty ST and the DA 'c' terminator arrive, or the
// timeout fires.
ProbeResult probeGraphics() {
    int fd = open("/dev/tty", O_RDWR | O_NOCTTY);
    if (fd < 0) return ProbeResult::Unknown;
    struct termios saved{};
    if (tcgetattr(fd, &saved) != 0) {
        close(fd);
        return ProbeResult::Unknown;
    }
    struct termios raw = saved;
    raw.c_lflag &=
        ~static_cast<tcflag_t>(ICANON | ECHO);  // no canon, NO ECHO (set before the query)
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 2;  // 0.2s between bytes before giving up
    tcsetattr(fd, TCSANOW, &raw);
    // a=q: validate + reply, do not draw ("AAAA" is one 1x1 black pixel). Then Primary DA (CSI c).
    static const char query[] = "\033_Gi=1,a=q,f=24,s=1,v=1;AAAA\033\\\033[c";
    ProbeResult result = ProbeResult::Unknown;
    if (write(fd, query, sizeof query - 1) == static_cast<ssize_t>(sizeof query - 1)) {
        std::string reply;
        bool sawDA = false;
        char c;
        while (reply.size() < 128) {
            ssize_t n = read(fd, &c, 1);
            if (n <= 0) break;  // timeout: no more bytes coming
            reply += c;
            // DA terminates with a literal 'c' after an ESC [ ? ... sequence; once we've seen it
            // the exchange is complete (Kitty's ;OK, if any, precedes DA since we wrote it first).
            if (c == 'c' && reply.find("\033[?") != std::string::npos) {
                sawDA = true;
                break;
            }
        }
        if (reply.find(";OK") != std::string::npos)
            result = ProbeResult::Kitty;  // Kitty answered -> definite
        else if (sawDA && reply.find(";4;") != std::string::npos)
            result = ProbeResult::Sixel;  // DA advertises sixel (attribute 4)
        else if (sawDA)
            result = ProbeResult::Unsupported;  // answered, speaks neither (Apple Terminal)
        // else: total silence -> Unknown (slow/lost reply, or env-stripped Kitty-capable remote)
    }
    tcsetattr(fd, TCSANOW, &saved);
    close(fd);
    return result;
}

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
GraphicsBackend graphicsBackend() {
    static const GraphicsBackend backend = [] {
        if (gForcedBackend >= 0)
            return static_cast<GraphicsBackend>(gForcedBackend);  // --img <protocol>
        if (const char* g = std::getenv("MDCAT_GRAPHICS")) {
            std::string s(g);
            if (s == "kitty") return GraphicsBackend::Kitty;
            if (s == "sixel") return GraphicsBackend::Sixel;
            if (s == "none") return GraphicsBackend::None;
        }
        if (gForceGraphics)
            return GraphicsBackend::Kitty;  // bare --img: default to Kitty when piped
        if (!isatty(STDOUT_FILENO)) return GraphicsBackend::None;
        const char* prog = std::getenv("TERM_PROGRAM");
        std::string tp = prog ? prog : "";
        if (tp == "Apple_Terminal") return GraphicsBackend::None;  // no sixel, no Kitty
        if (std::getenv("KITTY_WINDOW_ID") || tp == "ghostty" || tp == "iTerm.app" ||
            tp == "vscode")
            return GraphicsBackend::Kitty;
        switch (probeGraphics()) {
        case ProbeResult::Kitty: return GraphicsBackend::Kitty;
        case ProbeResult::Sixel: return GraphicsBackend::Sixel;
        case ProbeResult::Unsupported: return GraphicsBackend::None;  // answered, speaks neither
        case ProbeResult::Unknown: return GraphicsBackend::Kitty;     // silence -> optimistic
        }
        return GraphicsBackend::Kitty;  // unreachable; satisfies non-void return
    }();
    if (std::getenv("MDCAT_DEBUG_CELL"))
        std::fprintf(stderr,
                     "mdcat: graphics backend: %s\n",
                     backend == GraphicsBackend::Kitty       ? "kitty"
                         : backend == GraphicsBackend::Sixel ? "sixel"
                                                             : "none");
    return backend;
}

// Whether any inline-image backend is available (Kitty or Sixel). When false an <img> falls back to
// its alt text or filename. Kept as a thin predicate so existing call sites read clearly.
bool terminalSupportsGraphics() {
    return graphicsBackend() != GraphicsBackend::None;
}

// Whether the mermaid CLI (mmdc) is on $PATH, so a ```mermaid code block can be rendered as a
// diagram. Probed once via `command -v` and cached. When absent, mermaid blocks fall back to being
// shown as ordinary fenced code. The probe is skipped entirely on terminals that cannot display
// graphics, since the rendered PNG could not be painted there anyway.
bool mmdcAvailable() {
    static const bool available = [] {
        if (!terminalSupportsGraphics()) return false;
        return std::system("command -v mmdc >/dev/null 2>&1") == 0;
    }();
    return available;
}

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

// Send a CSI window-op report request ("ESC [ <op> t") over /dev/tty and return the terminal's
// reply. Used for the cell-size / text-area queries (16 t, 14 t, 18 t), which all reply "ESC [ <p1>
// ; A ; B t". Done over /dev/tty (not stdout) so it works even when our output is piped to a pager,
// and in raw mode with echo off and a short timeout so the reply is not echoed, line-buffered, or
// able to hang us. CRITICALLY these queries reach the LOCAL terminal even over SSH (the bytes
// traverse the pty), which is what lets mdcat learn the local cell size remotely, where TIOCGWINSZ
// pixel fields are zero. Returns {0,0} if there is no controlling terminal or it doesn't answer.
// `op` is e.g. "16" / "14" / "18"; the two parsed numbers (in reply order: height-ish then
// width-ish) are returned as {a, b}.
struct TwoInts {
    int a;
    int b;
};
TwoInts queryWindowOp(const char* op) {
    int fd = open("/dev/tty", O_RDWR | O_NOCTTY);
    if (fd < 0) return {0, 0};
    struct termios saved{};
    if (tcgetattr(fd, &saved) != 0) {
        close(fd);
        return {0, 0};
    }
    struct termios raw = saved;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 2;  // 0.2s between bytes before giving up
    tcsetattr(fd, TCSANOW, &raw);
    std::string query = std::string("\033[") + op + "t";
    TwoInts out{0, 0};
    if (write(fd, query.c_str(), query.size()) == static_cast<ssize_t>(query.size())) {
        std::string reply;
        char c;
        // Read until the report's terminating 't' (or the inter-byte timeout). The reply is short.
        while (reply.size() < 32) {
            ssize_t n = read(fd, &c, 1);
            if (n <= 0) break;
            reply += c;
            if (c == 't') break;
        }
        // Parse "ESC [ <p1> ; <a> ; <b> t". sscanf skips the ESC/[ and p1 for us.
        int p1 = 0, a = 0, b = 0;
        if (std::sscanf(reply.c_str(), "\033[%d;%d;%dt", &p1, &a, &b) == 3 && a > 0 && b > 0)
            out = {a, b};
    }
    tcsetattr(fd, TCSANOW, &saved);
    close(fd);
    return out;
}

// Query the terminal's background colour with OSC 11 ("ESC ] 11 ; ? BEL") and return it as a timg
// `-b` argument ("#rrggbb"), or empty if the terminal doesn't answer. timg pads a sixel's height up
// to the next 6px band and fills the padding (and any alpha) with its -b colour; the default is
// BLACK, which paints a visible dark line under a light-background image (e.g. a mermaid diagram).
// Filling with the real terminal background instead makes the pad invisible. Cached once per run.
//
// The reply is "ESC ] 11 ; rgb:RRRR/GGGG/BBBB <ST|BEL>" with 1-4 hex digits per channel; we take
// the high byte of each. Done over /dev/tty in raw mode with a short timeout, like queryWindowOp,
// so it reaches the local terminal even when stdout is piped and never hangs or echoes.
std::string queryBackgroundColor() {
    static const std::string color = []() -> std::string {
        if (const char* env = std::getenv("MDCAT_BG"))  // explicit override / testability
            return env[0] ? std::string(env) : std::string();
        int fd = open("/dev/tty", O_RDWR | O_NOCTTY);
        if (fd < 0) return std::string();
        struct termios saved{};
        if (tcgetattr(fd, &saved) != 0) {
            close(fd);
            return std::string();
        }
        struct termios raw = saved;
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 2;  // 0.2s between bytes
        tcsetattr(fd, TCSANOW, &raw);
        std::string out;
        const char query[] = "\033]11;?\033\\";
        if (write(fd, query, sizeof query - 1) == static_cast<ssize_t>(sizeof query - 1)) {
            std::string reply;
            char c;
            // Read until ST (ESC \) or BEL, or the inter-byte timeout. The reply is short.
            while (reply.size() < 64) {
                ssize_t n = read(fd, &c, 1);
                if (n <= 0) break;
                reply += c;
                if (c == '\a') break;
                if (c == '\\' && reply.size() >= 2 && reply[reply.size() - 2] == '\033') break;
            }
            unsigned r = 0, g = 0, b = 0;
            // "rgb:RRRR/GGGG/BBBB" (1-4 hex digits each); %x reads however many hex digits are
            // present.
            size_t pos = reply.find("rgb:");
            if (pos != std::string::npos &&
                std::sscanf(reply.c_str() + pos, "rgb:%x/%x/%x", &r, &g, &b) == 3) {
                // Normalise each channel's high byte: scale a 1-4 digit value to 8 bits. A 4-digit
                // value is 16-bit, so >>8; map shorter widths by their digit count.
                auto hi8 = [](unsigned v) {
                    while (v > 0xFF) v >>= 4;  // collapse 16-/12-bit channels to 8 bits
                    return v;
                };
                char buf[8];
                std::snprintf(buf, sizeof buf, "#%02x%02x%02x", hi8(r), hi8(g), hi8(b));
                out = buf;
            }
        }
        tcsetattr(fd, TCSANOW, &saved);
        close(fd);
        return out;
    }();
    return color;
}

// Whether the terminal has a DARK background, so code blocks and syntax highlighting should use the
// dark palette (light-gray text on dark-gray, lightened hues) instead of the light one. Decided
// from the OSC 11 background colour's luminance (Rec. 601: a bg darker than mid-gray is "dark").
// When the terminal doesn't answer OSC 11 (headless/piped, or a terminal that ignores it) we
// default to the LIGHT theme, matching mdcat's historical appearance. $MDCAT_THEME=dark|light|auto
// forces the choice. Cached once per run (the underlying query is memoised; this just classifies
// it).
bool darkBackground() {
    static const bool dark = [] {
        if (const char* t = std::getenv("MDCAT_THEME")) {
            if (std::string(t) == "dark") return true;
            if (std::string(t) == "light") return false;
            // "auto" (or anything else) falls through to detection.
        }
        std::string bg = queryBackgroundColor();  // "#rrggbb" or empty
        if (bg.size() != 7 || bg[0] != '#')
            return false;  // no answer -> light (historical default)
        auto hex = [&](int i) { return std::stoi(bg.substr(i, 2), nullptr, 16); };
        int r = hex(1), g = hex(3), b = hex(5);
        // Rec. 601 luma; < 128 (mid-gray) means a dark background.
        return (299 * r + 587 * g + 114 * b) / 1000 < 128;
    }();
    return dark;
}

// Select the code-block colour palette (kCodeOn / kLightGray here, plus the highlighter's palette)
// for the terminal theme. Light theme keeps the historical look (dark-gray text on light-gray);
// dark theme uses light-gray text on dark-gray. Called once on the main thread (initTheme) before
// rendering, so every kCodeOn/kLightGray read sees the chosen palette and the OSC 11 query
// (memoised) fires once.
void initTheme() {
    if (darkBackground()) {
        kCodeOn = "\033[48;5;236;38;5;252m";  // dark-gray bg, light-gray fg
        kLightGray = "\033[38;5;240m";        // a dimmer separator that reads on a dark background
        setHighlightTheme(true);
    } else {
        setHighlightTheme(false);  // kCodeOn/kLightGray keep their light-theme defaults
    }
}

// Read a positive integer from an environment variable, or 0 if unset/non-positive.
int envInt(const char* name) {
    if (const char* v = std::getenv(name)) {
        int n = std::atoi(v);
        if (n > 0) return n;
    }
    return 0;
}

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
CellMetrics cellMetrics() {
    static const CellMetrics m = [] {
        CellMetrics c;
        // 1. Explicit env overrides (any subset).
        c.cellW = envInt("MDCAT_CELL_W") ? envInt("MDCAT_CELL_W") : c.cellW;
        c.cellH = envInt("MDCAT_CELL_H") ? envInt("MDCAT_CELL_H") : c.cellH;
        if (envInt("MDCAT_AREA_W") && envInt("MDCAT_AREA_H")) {
            c.areaW = envInt("MDCAT_AREA_W");
            c.areaH = envInt("MDCAT_AREA_H");
        }
        if (c.areaW > 0 || envInt("MDCAT_CELL_W")) {
            // An override was given; trust it and skip the queries.
            if (std::getenv("MDCAT_DEBUG_CELL"))
                std::fprintf(stderr,
                             "mdcat: cell metrics (env): area=%dx%d cell=%dx%d\n",
                             c.areaW,
                             c.areaH,
                             c.cellW,
                             c.cellH);
            return c;
        }
        // 2. Kernel window size with pixel fields (best when local).
        for (int fd : {STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO}) {
            struct winsize ws;
            if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_xpixel > 0 && ws.ws_ypixel > 0 &&
                ws.ws_col > 0 && ws.ws_row > 0) {
                c.areaW = ws.ws_xpixel;
                c.areaH = ws.ws_ypixel;
                c.cols = ws.ws_col;
                c.rows = ws.ws_row;
                c.cellW = std::max(1, ws.ws_xpixel / ws.ws_col);
                c.cellH = std::max(1, ws.ws_ypixel / ws.ws_row);
                return c;
            }
        }
        // 3. Terminal queries (reach the local terminal over SSH). CSI 14 t (area px) + CSI 18 t
        // (area
        //    cells) give the precise area ratio; CSI 16 t gives the integer cell size as a
        //    backstop.
        TwoInts area = queryWindowOp("14");   // ESC [ 4 ; H ; W t
        TwoInts cells = queryWindowOp("18");  // ESC [ 8 ; rows ; cols t
        if (area.a > 0 && area.b > 0 && cells.a > 0 && cells.b > 0) {
            c.areaH = area.a;
            c.areaW = area.b;
            c.rows = cells.a;
            c.cols = cells.b;
            c.cellW = std::max(1, c.areaW / c.cols);
            c.cellH = std::max(1, c.areaH / c.rows);
            return c;
        }
        TwoInts cell = queryWindowOp("16");  // ESC [ 6 ; H ; W t
        if (cell.a > 0 && cell.b > 0) {
            c.cellH = cell.a;
            c.cellW = cell.b;
        }  // area ratio stays unknown
        return c;  // else the {8,16} struct defaults
    }();
    if (std::getenv("MDCAT_DEBUG_CELL"))
        std::fprintf(stderr,
                     "mdcat: cell metrics: area=%dx%d px, cells=%dx%d, cell~%dx%d px\n",
                     m.areaW,
                     m.areaH,
                     m.cols,
                     m.rows,
                     m.cellW,
                     m.cellH);
    return m;
}

// ---------------------------------------------------------------------------
// UTF-8 / display-width utilities
// ---------------------------------------------------------------------------

// Number of bytes in the UTF-8 sequence whose lead byte is c (1 for invalid lead bytes).
int utf8SequenceLength(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x06) return 2;
    if ((c >> 4) == 0x0E) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

// Decode the UTF-8 code point beginning at s[i] into cp, returning the byte length consumed (>=1).
// An invalid/truncated sequence yields the single lead byte as cp with length 1 (best-effort).
int decodeUtf8(const std::string& s, size_t i, uint32_t& cp) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    int len = utf8SequenceLength(c);
    if (len == 1 || i + static_cast<size_t>(len) > s.size()) {
        cp = c;
        return 1;
    }
    static const unsigned char kLeadMask[5] = {0, 0x7F, 0x1F, 0x0F, 0x07};
    cp = c & kLeadMask[len];
    for (int k = 1; k < len; ++k) {
        unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) {
            cp = c;
            return 1;
        }  // not a continuation byte: invalid
        cp = (cp << 6) | (cc & 0x3F);
    }
    return len;
}

// True if `cp` is between lo and hi inclusive (used by the width tables below).
static inline bool inRange(uint32_t cp, uint32_t lo, uint32_t hi) {
    return cp >= lo && cp <= hi;
}

// Terminal column width of a single Unicode code point: 0 for zero-width (combining marks, joiners,
// variation selectors, the BOM), 2 for East-Asian-Wide / Fullwidth and the emoji that render as a
// double-width cell, and 1 otherwise. This is a pragmatic subset of UAX #11 / the Unicode emoji
// data — the ranges that actually occur in the content mdcat renders — not a full property
// database. See gmore_core.h, which mirrors this exactly so the pager and the renderer agree on
// alignment.
int codePointWidth(uint32_t cp) {
    if (cp == 0) return 0;
    // Zero-width: combining diacritics, joiners (ZWJ/ZWNJ), variation selectors, BOM/ZWNBSP.
    if (inRange(cp, 0x0300, 0x036F) ||                   // combining diacritical marks
        inRange(cp, 0x1AB0, 0x1AFF) ||                   // combining diacritical marks extended
        inRange(cp, 0x1DC0, 0x1DFF) ||                   // combining diacritical marks supplement
        inRange(cp, 0x20D0, 0x20FF) ||                   // combining marks for symbols
        inRange(cp, 0xFE20, 0xFE2F) ||                   // combining half marks
        cp == 0x200B || cp == 0x200C || cp == 0x200D ||  // ZWSP, ZWNJ, ZWJ
        cp == 0xFEFF ||                                  // ZWNBSP / BOM
        inRange(cp, 0xFE00, 0xFE0F) ||  // variation selectors 1-16 (incl. VS16 emoji presentation)
        inRange(cp, 0xE0100, 0xE01EF))  // variation selectors supplement
        return 0;
    // East-Asian Wide / Fullwidth and the wide symbol/emoji blocks that occupy two cells.
    if (inRange(cp, 0x1100, 0x115F) ||    // Hangul Jamo
        cp == 0x2329 || cp == 0x232A ||   // angle brackets
        inRange(cp, 0x2E80, 0x303E) ||    // CJK radicals, Kangxi, CJK symbols/punctuation
        inRange(cp, 0x3041, 0x33FF) ||    // Hiragana, Katakana, CJK symbols, enclosed CJK
        inRange(cp, 0x3400, 0x4DBF) ||    // CJK Extension A
        inRange(cp, 0x4E00, 0x9FFF) ||    // CJK Unified Ideographs
        inRange(cp, 0xA000, 0xA4CF) ||    // Yi
        inRange(cp, 0xAC00, 0xD7A3) ||    // Hangul Syllables
        inRange(cp, 0xF900, 0xFAFF) ||    // CJK compatibility ideographs
        inRange(cp, 0xFE10, 0xFE19) ||    // vertical forms
        inRange(cp, 0xFE30, 0xFE6F) ||    // CJK compatibility forms, small form variants
        inRange(cp, 0xFF00, 0xFF60) ||    // fullwidth forms
        inRange(cp, 0xFFE0, 0xFFE6) ||    // fullwidth signs
        inRange(cp, 0x1F300, 0x1F64F) ||  // Misc symbols & pictographs, emoticons
        inRange(cp, 0x1F680, 0x1F6FF) ||  // transport & map symbols
        inRange(cp, 0x1F900, 0x1F9FF) ||  // supplemental symbols & pictographs
        inRange(cp, 0x1FA70, 0x1FAFF) ||  // symbols & pictographs extended-A
        inRange(cp, 0x20000, 0x3FFFD))    // CJK Extension B+ and plane 3
        return 2;
    return 1;
}

// Display width of a string in terminal columns, skipping ANSI escape (CSI) sequences and OSC 8
// hyperlink sequences so that styled text measures by what is actually shown. Each code point is
// measured by codePointWidth, so fullwidth emoji / CJK count as two columns and combining marks,
// joiners and variation selectors as zero — keeping tables and reflow aligned with what the
// terminal actually draws.
int displayWidth(const std::string& s) {
    int width = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == kEsc && i + 1 < s.size() && s[i + 1] == '[') {
            // CSI sequence: ESC [ ... final-byte in @..~
            i += 2;
            while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) ++i;
            if (i < s.size()) ++i;  // consume the final byte
            continue;
        }
        if (c == kEsc && i + 1 < s.size() && s[i + 1] == ']') {
            // OSC sequence: ESC ] ... terminated by BEL or ST (ESC backslash)
            i += 2;
            while (i < s.size()) {
                if (static_cast<unsigned char>(s[i]) == 0x07) {
                    ++i;
                    break;
                }
                if (s[i] == kEsc && i + 1 < s.size() && s[i + 1] == '\\') {
                    i += 2;
                    break;
                }
                ++i;
            }
            continue;
        }
        uint32_t cp;
        size_t adv = static_cast<size_t>(decodeUtf8(s, i, cp));
        // A pair of regional-indicator symbols (U+1F1E6..1F1FF) is one flag glyph, two cells wide —
        // not 2+2. Consume the second indicator here so the pair counts once.
        if (inRange(cp, 0x1F1E6, 0x1F1FF) && i + adv < s.size()) {
            uint32_t cp2;
            size_t adv2 = static_cast<size_t>(decodeUtf8(s, i + adv, cp2));
            if (inRange(cp2, 0x1F1E6, 0x1F1FF)) {
                width += 2;
                i += adv + adv2;
                continue;
            }
        }
        i += adv;
        width += codePointWidth(cp);
    }
    return width;
}

// Append n copies of the box-drawing horizontal line character to out.
std::string horizontalLine(int n, const std::string& bar = "─") {
    std::string out;
    for (int i = 0; i < n; ++i) out += bar;
    return out;
}

std::string padTo(const std::string& s, int width) {
    int w = displayWidth(s);
    std::string out = s;
    for (int i = w; i < width; ++i) out += ' ';
    return out;
}

// Per-column text alignment derived from a GFM delimiter row (:--, --:, :-:).
enum class Align { Left, Right, Center };

// Pad `s` to `width` display columns, placing the content per `a`. Spaces are added before and/or
// after; padding never appears inside the styled content, so ANSI escapes are unaffected. A cell
// wider than `width` is returned unchanged (layout sizes columns to fit, so this is the rare
// re-render-rounding case, where overflowing is preferable to truncating).
std::string padAligned(const std::string& s, int width, Align a) {
    int w = displayWidth(s);
    if (w >= width) return s;
    int pad = width - w;
    switch (a) {
    case Align::Right: return std::string(pad, ' ') + s;
    case Align::Center: {
        int left = pad / 2;
        return std::string(left, ' ') + s + std::string(pad - left, ' ');
    }
    case Align::Left:
    default: return s + std::string(pad, ' ');
    }
}

// Trim ASCII spaces and tabs from both ends.
std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

// ---------------------------------------------------------------------------
// LaTeX math -> Unicode
// ---------------------------------------------------------------------------
//
// GitHub renders LaTeX between $...$ (inline) and $$...$$ (block). We don't ship a TeX engine;
// instead renderMath() does a best-effort transliteration of the common, simple expressions that
// map cleanly onto Unicode: Greek letters, a handful of operator/relation symbols, super/subscripts
// of digits and a few characters, \mathrm/\mathbf wrappers, and \mathbb blackboard-bold letters.
// Bare Latin letters render in math italic by default (as in LaTeX math mode); \mathrm/\text keep
// them upright and \mathbf makes them bold.
// Anything it can't represent is left as-is — and a `\cmd{...}` group that can't be fully rendered
// is kept verbatim INCLUDING its braces (so a LaTeX reader still sees `\mathbb{R}`, not
// `\mathbbR`), so the output is never worse than the raw source. The function is total: never
// throws.

// Greek letters and named symbols, keyed by the LaTeX command without its backslash.
const std::map<std::string, std::string>& mathSymbols() {
    static const std::map<std::string, std::string> m = {
        // Lowercase Greek
        {"alpha", "α"},
        {"beta", "β"},
        {"gamma", "γ"},
        {"delta", "δ"},
        {"epsilon", "ε"},
        {"varepsilon", "ε"},
        {"zeta", "ζ"},
        {"eta", "η"},
        {"theta", "θ"},
        {"vartheta", "ϑ"},
        {"iota", "ι"},
        {"kappa", "κ"},
        {"lambda", "λ"},
        {"mu", "μ"},
        {"nu", "ν"},
        {"xi", "ξ"},
        {"omicron", "ο"},
        {"pi", "π"},
        {"varpi", "ϖ"},
        {"rho", "ρ"},
        {"varrho", "ϱ"},
        {"sigma", "σ"},
        {"varsigma", "ς"},
        {"tau", "τ"},
        {"upsilon", "υ"},
        {"phi", "φ"},
        {"varphi", "ϕ"},
        {"chi", "χ"},
        {"psi", "ψ"},
        {"omega", "ω"},
        // Uppercase Greek
        {"Alpha", "Α"},
        {"Beta", "Β"},
        {"Gamma", "Γ"},
        {"Delta", "Δ"},
        {"Epsilon", "Ε"},
        {"Zeta", "Ζ"},
        {"Eta", "Η"},
        {"Theta", "Θ"},
        {"Iota", "Ι"},
        {"Kappa", "Κ"},
        {"Lambda", "Λ"},
        {"Mu", "Μ"},
        {"Nu", "Ν"},
        {"Xi", "Ξ"},
        {"Omicron", "Ο"},
        {"Pi", "Π"},
        {"Rho", "Ρ"},
        {"Sigma", "Σ"},
        {"Tau", "Τ"},
        {"Upsilon", "Υ"},
        {"Phi", "Φ"},
        {"Chi", "Χ"},
        {"Psi", "Ψ"},
        {"Omega", "Ω"},
        // Operators / relations / misc
        {"exists", "∃"},
        {"in", "∈"},
        {"int", "∫"},
        {"sum", "∑"},
        {"prod", "∏"},
        {"partial", "∂"},
        {"infty", "∞"},
        {"perp", "⊥"},
        {"parallel", "∥"},
        {"therefore", "∴"},
        {"because", "∵"},
        {"subset", "⊂"},
        {"supset", "⊃"},
        {"subseteq", "⊆"},
        {"supseteq", "⊇"},
        {"to", "→"},
        {"rightarrow", "→"},
        {"longrightarrow", "⟶"},
        {"leftarrow", "←"},
        {"Rightarrow", "⇒"},
        {"Leftarrow", "⇐"},
        {"times", "×"},
        {"div", "÷"},
        {"pm", "±"},
        {"mp", "∓"},
        {"simeq", "≃"},
        {"approx", "≈"},
        {"cong", "≅"},
        {"equiv", "≡"},
        {"neq", "≠"},
        {"leq", "≤"},
        {"geq", "≥"},
        {"ll", "≪"},
        {"gg", "≫"},
        {"cdot", "⋅"},
        {"cdots", "⋯"},
        {"ldots", "…"},
        {"dots", "…"},
        {"nabla", "∇"},
        {"forall", "∀"},
        {"notin", "∉"},
        {"emptyset", "∅"},
        {"cup", "∪"},
        {"cap", "∩"},
        {"wedge", "∧"},
        {"vee", "∨"},
        {"neg", "¬"},
        {"oplus", "⊕"},
        {"otimes", "⊗"},
        {"sqrt", "√"},
        {"angle", "∠"},
        {"prime", "′"},
        {"circ", "∘"},
        {"star", "⋆"},
        {"langle", "⟨"},
        {"rangle", "⟩"},
        {"propto", "∝"},
        {"mapsto", "↦"},
    };
    return m;
}

// Super/subscript glyphs for the characters that have dedicated Unicode forms. Characters with no
// form are left unconverted (the whole script group is then rendered literally with ^ or _).
const std::map<char, std::string>& superscripts() {
    static const std::map<char, std::string> m = {
        {'0', "⁰"},
        {'1', "¹"},
        {'2', "²"},
        {'3', "³"},
        {'4', "⁴"},
        {'5', "⁵"},
        {'6', "⁶"},
        {'7', "⁷"},
        {'8', "⁸"},
        {'9', "⁹"},
        {'+', "⁺"},
        {'-', "⁻"},
        {'=', "⁼"},
        {'(', "⁽"},
        {')', "⁾"},
        {'n', "ⁿ"},
        {'i', "ⁱ"},
        {'.', "·"},
    };
    return m;
}
const std::map<char, std::string>& subscripts() {
    static const std::map<char, std::string> m = {
        {'0', "₀"},
        {'1', "₁"},
        {'2', "₂"},
        {'3', "₃"},
        {'4', "₄"},
        {'5', "₅"},
        {'6', "₆"},
        {'7', "₇"},
        {'8', "₈"},
        {'9', "₉"},
        {'+', "₊"},
        {'-', "₋"},
        {'=', "₌"},
        {'(', "₍"},
        {')', "₎"},
    };
    return m;
}

// Blackboard-bold (\mathbb) and calligraphic (\mathcal) letters that have dedicated Unicode forms.
// Returns the mapped string, or empty if the letter has no form (so the caller can keep \mathbb{X}
// literal rather than dropping the styling).
std::string mathbbLetter(char c) {
    switch (c) {
    case 'A': return "𝔸";
    case 'B': return "𝔹";
    case 'C': return "ℂ";
    case 'D': return "𝔻";
    case 'E': return "𝔼";
    case 'F': return "𝔽";
    case 'G': return "𝔾";
    case 'H': return "ℍ";
    case 'I': return "𝕀";
    case 'J': return "𝕁";
    case 'K': return "𝕂";
    case 'L': return "𝕃";
    case 'M': return "𝕄";
    case 'N': return "ℕ";
    case 'O': return "𝕆";
    case 'P': return "ℙ";
    case 'Q': return "ℚ";
    case 'R': return "ℝ";
    case 'S': return "𝕊";
    case 'T': return "𝕋";
    case 'U': return "𝕌";
    case 'V': return "𝕍";
    case 'W': return "𝕎";
    case 'X': return "𝕏";
    case 'Y': return "𝕐";
    case 'Z': return "ℤ";
    }
    return "";
}

// Math styling applied to bare Latin letters. In LaTeX math mode the default is italic; \mathrm /
// \text / \mathsf / \operatorname keep letters upright; \mathbf is bold; \mathit is italic.
enum class MathStyle { Italic, Upright, Bold };

// Encode a Unicode code point as UTF-8 and append it to `out`.
void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Map an ASCII letter to its styled Mathematical-Alphanumeric code point, or return the letter
// unchanged for Upright (which is plain ASCII). The Mathematical Italic and Bold blocks are
// contiguous A-Z then a-z, except that italic lowercase 'h' is unassigned (its glyph is the Planck
// constant U+210E). Non-letters pass through unchanged.
std::string mathStyledLetter(char c, MathStyle style) {
    bool upper = (c >= 'A' && c <= 'Z');
    bool lower = (c >= 'a' && c <= 'z');
    if (!upper && !lower) return std::string(1, c);
    unsigned idx = upper ? (c - 'A') : 26 + (c - 'a');
    std::string out;
    switch (style) {
    case MathStyle::Upright: return std::string(1, c);
    case MathStyle::Italic:
        if (c == 'h') {
            appendUtf8(out, 0x210E);
            return out;
        }  // Planck constant ℎ
        appendUtf8(out, 0x1D434 + idx);  // 𝐴..𝑧 (italic)
        return out;
    case MathStyle::Bold:
        appendUtf8(out, 0x1D400 + idx);  // 𝐀..𝐳 (bold)
        return out;
    }
    return std::string(1, c);
}

// Try to convert a super/subscript group at s[i] (the '^' or '_'). On success, appends the Unicode
// to out, advances i past the consumed text, and returns true. On failure returns false and leaves
// i/out unchanged so the caller can emit the marker literally.
bool convertScript(const std::string& s, size_t& i, std::string& out, bool sup) {
    const auto& table = sup ? superscripts() : subscripts();
    std::string body;  // the raw characters being scripted
    size_t j = i + 1;
    if (j < s.size() && s[j] == '{') {
        size_t k = j + 1;
        while (k < s.size() && s[k] != '}') body += s[k++];
        if (k >= s.size()) return false;  // unbalanced brace: give up
        j = k + 1;
    } else if (j < s.size()) {
        body = s[j];  // single-character script: x^2, a_i
        j += 1;
    } else {
        return false;
    }
    std::string conv;
    for (char c : body) {
        auto it = table.find(c);
        if (it == table.end()) return false;  // a char with no glyph: render the group literally
        conv += it->second;
    }
    out += conv;
    i = j;
    return true;
}

// Best-effort transliteration of a LaTeX math fragment to Unicode. `tex` is the content between the
// delimiters (without the $'s). `style` is the font applied to bare Latin letters (italic by
// default, as in LaTeX math mode; \mathrm and friends recurse with a different style). Always
// returns a renderable string.
std::string renderMath(const std::string& tex, MathStyle style = MathStyle::Italic) {
    std::string out;
    size_t n = tex.size();
    for (size_t i = 0; i < n;) {
        char c = tex[i];
        if (c == '\\') {
            // Read the command name (letters), or a single-character escape like \{ or \,.
            size_t j = i + 1;
            while (j < n && std::isalpha(static_cast<unsigned char>(tex[j]))) ++j;
            if (j == i + 1) {
                // Non-letter after backslash: \, \; \! are spacing (drop); \{ \} \$ are literals.
                char e = (j < n) ? tex[j] : '\0';
                if (e == ',' || e == ';' || e == ':' || e == '!' || e == ' ') {
                    i = j + 1;
                    continue;
                }
                if (e == '{' || e == '}' || e == '$' || e == '%' || e == '&' || e == '#') {
                    out += e;
                    i = j + 1;
                    continue;
                }
                out += c;
                ++i;
                continue;  // unknown: keep the backslash literally
            }
            std::string cmd = tex.substr(i + 1, j - (i + 1));
            if (cmd == "quad" || cmd == "qquad") {
                out += ' ';
                i = j;
                continue;
            }
            if (cmd == "left" || cmd == "right") {
                i = j;
                continue;
            }  // delimiter sizing: drop

            // If the command takes a braced argument, read the whole balanced group now. `arg` is
            // the argument contents; `groupEnd` is the position just past the closing '}'. If there
            // is no balanced group, argEnd stays == j (no argument) and we keep the bare command
            // logic.
            std::string arg;
            size_t groupEnd = j;
            bool hasArg = false;
            if (j < n && tex[j] == '{') {
                size_t k = j + 1;
                int depth = 1;
                while (k < n && depth > 0) {
                    if (tex[k] == '{')
                        ++depth;
                    else if (tex[k] == '}') {
                        if (--depth == 0) break;
                    }
                    if (depth > 0) arg += tex[k];
                    ++k;
                }
                if (k < n) {
                    hasArg = true;
                    groupEnd = k + 1;
                }  // balanced; else leave hasArg false
            }

            // \mathbb / \mathcal: each letter must have a Unicode form, or the whole group is kept.
            if (hasArg && (cmd == "mathbb" || cmd == "mathcal")) {
                std::string conv;
                bool ok = !arg.empty();
                for (char ch : arg) {
                    std::string g = mathbbLetter(ch);
                    if (g.empty()) {
                        ok = false;
                        break;
                    }
                    conv += g;
                }
                if (ok) {
                    out += conv;
                    i = groupEnd;
                    continue;
                }
                out += tex.substr(i, groupEnd - i);  // keep \mathbb{...} verbatim, braces and all
                i = groupEnd;
                continue;
            }

            // \mathrm / \mathbf / ...: styling wrappers. Recurse on the argument with the wrapper's
            // font, but only accept the result if it is fully representable (no leftover backslash
            // from an unmapped command); otherwise keep the entire \cmd{...} so a LaTeX reader
            // still sees the intent.
            if (hasArg &&
                (cmd == "mathrm" || cmd == "mathbf" || cmd == "mathit" || cmd == "mathsf" ||
                 cmd == "text" || cmd == "operatorname")) {
                MathStyle inner = cmd == "mathbf" ? MathStyle::Bold
                    : cmd == "mathit"             ? MathStyle::Italic
                                      : MathStyle::Upright;  // mathrm, text, mathsf, operatorname
                std::string conv = renderMath(arg, inner);
                if (conv.find('\\') == std::string::npos) {
                    out += conv;
                    i = groupEnd;
                    continue;
                }
                out += tex.substr(i, groupEnd - i);  // keep \cmd{...} verbatim
                i = groupEnd;
                continue;
            }

            auto it = mathSymbols().find(cmd);
            if (it != mathSymbols().end()) {
                out += it->second;
                i = j;
                continue;
            }

            // Unknown command: leave it as written, INCLUDING any braced argument, so the source
            // stays readable (e.g. \mathbb{R} -> "\mathbb{R}", not "\mathbbR").
            if (hasArg) {
                out += tex.substr(i, groupEnd - i);
                i = groupEnd;
                continue;
            }
            out += "\\" + cmd;
            i = j;
            continue;
        }
        if (c == '^' || c == '_') {
            if (convertScript(tex, i, out, c == '^')) continue;
            out += c;
            ++i;
            continue;
        }
        if (c == '{' || c == '}') {
            ++i;
            continue;
        }  // bare grouping braces have no visual effect
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            out += mathStyledLetter(c, style);  // Latin letters take the current math font
            ++i;
            continue;
        }
        int len = utf8SequenceLength(static_cast<unsigned char>(c));
        out += tex.substr(i, len);
        i += len;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Inline parsing
// ---------------------------------------------------------------------------

// Render markdown inline content from raw text into ANSI-styled output.
//
// The renderer runs in two linear passes so that its cost is O(n) in the input length, with no
// recursion or whole-buffer copying:
//
//   Pass 1 (tokenize): scan left to right producing a flat list of tokens. Backslash escapes,
//   code spans and links are resolved immediately (they don't nest emphasis the way emphasis
//   nests them, and their bodies are short). Every run of '*' or '_' becomes a Delim token whose
//   open/close capability is decided from the characters flanking it.
//
//   Pass 2 (resolve emphasis): walk the tokens with a stack of open delimiters, matching each
//   closer to the nearest compatible opener (the standard GFM algorithm). Matched pairs are
//   marked as strong or emphasis; unmatched delimiters stay literal. A final pass emits text.
//
// Emphasis uses a practical subset of the GFM flanking rules: a delimiter run can open if it is
// not followed by whitespace and can close if it is not preceded by whitespace; '_' additionally
// may not open/close inside a word, so snake_case is left intact. '**' is strong, '*'/'_' is
// emphasis; a run of length >= 2 can serve as either by consuming two or one of its characters.
class InlineRenderer {
public:
    explicit InlineRenderer(const std::string& text) : s_(text) {}

    std::string render() {
        tokenize();
        resolveEmphasis();
        return emit();
    }

private:
    const std::string& s_;

    enum class Kind { Text, Code, Link, Image, Delim };
    struct Token {
        Kind kind;
        std::string text;  // Text: literal; Code: content; Link: rendered link text
        std::string url;   // Link: target URL
        // Delim fields:
        char delim = 0;  // '*' or '_'
        int count = 0;   // remaining usable delimiter characters
        bool canOpen = false;
        bool canClose = false;
        // Emphasis resolution result, applied at emit time:
        int openStrong = 0, openEm = 0;    // styles opened just before this token's text
        int closeStrong = 0, closeEm = 0;  // styles closed just after
    };
    std::vector<Token> toks_;

    static bool isAsciiPunct(char c) {
        return std::string("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~").find(c) != std::string::npos;
    }
    static bool isWordChar(unsigned char c) { return std::isalnum(c) || c == '_'; }
    static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n'; }

    char charAt(size_t i) const { return i < s_.size() ? s_[i] : '\n'; }
    char prevAt(size_t i) const { return i == 0 ? '\n' : s_[i - 1]; }

    void addText(const std::string& t) {
        if (!toks_.empty() && toks_.back().kind == Kind::Text)
            toks_.back().text += t;
        else {
            Token tk;
            tk.kind = Kind::Text;
            tk.text = t;
            toks_.push_back(std::move(tk));
        }
    }

    // --- Pass 1 -------------------------------------------------------------
    void tokenize() {
        size_t n = s_.size();
        for (size_t i = 0; i < n;) {
            char c = s_[i];
            if (c == '\\' && i + 1 < n && isAsciiPunct(s_[i + 1])) {
                addText(std::string(1, s_[i + 1]));
                i += 2;
                continue;
            }
            if (c == '`') {
                if (scanCode(i)) continue;
            }
            if (c == '$') {
                if (scanMath(i)) continue;
            }
            if (c == '<') {
                if (scanHardBreak(i)) continue;
            }
            if (c == '!' && i + 1 < n && s_[i + 1] == '[') {
                if (scanImage(i)) continue;
            }
            if (c == '[') {
                if (scanLink(i)) continue;
            }
            if (c == '*' || c == '_') {
                scanDelim(i);
                continue;
            }
            int len = utf8SequenceLength(static_cast<unsigned char>(c));
            addText(s_.substr(i, len));
            i += len;
        }
    }

    // `code`: backtick run closed by an equal-length run; content emitted verbatim. Advances i.
    bool scanCode(size_t& i) {
        size_t open = 0;
        while (i + open < s_.size() && s_[i + open] == '`') ++open;
        size_t j = i + open;
        while (j < s_.size()) {
            if (s_[j] == '`') {
                size_t run = 0;
                while (j + run < s_.size() && s_[j + run] == '`') ++run;
                if (run == open) {
                    Token tk;
                    tk.kind = Kind::Code;
                    tk.text = s_.substr(i + open, j - (i + open));
                    toks_.push_back(std::move(tk));
                    i = j + run;
                    return true;
                }
                j += run;
            } else
                ++j;
        }
        return false;  // unmatched: fall through and treat the backticks as text
    }

    // $...$ (inline) or $$...$$ (block) LaTeX math. The content is transliterated to Unicode by
    // renderMath and emitted as plain text. We follow GitHub's heuristics to avoid mistaking prose
    // dollar signs (prices) for math: the opener must be followed by a non-space, the closer must
    // be preceded by a non-space, and a single-$ closer may not be immediately followed by a digit.
    // The '$' is at position i. On failure, falls through and the '$' is treated as literal text.
    bool scanMath(size_t& i) {
        size_t open = (i + 1 < s_.size() && s_[i + 1] == '$') ? 2 : 1;  // $ or $$
        size_t start = i + open;
        if (start >= s_.size() || isSpace(s_[start])) return false;  // no space right after opener
        size_t j = start;
        while (j < s_.size()) {
            if (s_[j] == '\\' && j + 1 < s_.size()) {
                j += 2;
                continue;
            }  // skip escaped char
            if (s_[j] == '$') {
                size_t run = 0;
                while (j + run < s_.size() && s_[j + run] == '$') ++run;
                if (run >= open) {
                    if (j == start) return false;          // empty: "$$" is not math
                    if (isSpace(s_[j - 1])) return false;  // no space right before closer
                    size_t after = j + open;
                    // For inline ($) math, a digit right after the closer means it's likely a
                    // price.
                    if (open == 1 && after < s_.size() &&
                        std::isdigit(static_cast<unsigned char>(s_[after])))
                        return false;
                    addText(renderMath(s_.substr(start, j - start)));
                    i = j + open;
                    return true;
                }
                j += run;
            } else
                ++j;
        }
        return false;  // unterminated: treat the opening '$' as literal text
    }

    // <br>, <br/>, <br />: an HTML hard line break (GFM treats it as a literal line break wherever
    // it appears in inline content). Emitted as a '\n' Text token; downstream reflow and table-cell
    // layout split on '\n'. Tag/attribute matching is case-insensitive but otherwise strict: a
    // malformed run like "<brx" or "<br foo" falls through and is rendered as literal text,
    // matching how mdcat passes all other inline HTML through unchanged. The '<' is at position i.
    bool scanHardBreak(size_t& i) {
        size_t j = i + 1;  // just past '<'
        auto lower = [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        };
        if (j + 1 >= s_.size() || lower(s_[j]) != 'b' || lower(s_[j + 1]) != 'r') return false;
        j += 2;
        while (j < s_.size() && (s_[j] == ' ' || s_[j] == '\t')) ++j;  // optional whitespace
        if (j < s_.size() && s_[j] == '/') ++j;                        // optional self-close slash
        if (j >= s_.size() || s_[j] != '>') return false;              // must end at '>'
        addText("\n");
        i = j + 1;
        return true;
    }

    // ![alt](url): markdown image. Captures alt text and url; the '!' is at position i.
    bool scanImage(size_t& i) {
        size_t j = i + 1;  // j points at '['
        size_t close = j + 1;
        int depth = 1;
        while (close < s_.size()) {
            if (s_[close] == '\\' && close + 1 < s_.size()) {
                close += 2;
                continue;
            }
            if (s_[close] == '[')
                ++depth;
            else if (s_[close] == ']') {
                if (--depth == 0) break;
            }
            ++close;
        }
        if (close >= s_.size() || close + 1 >= s_.size() || s_[close + 1] != '(') return false;
        size_t up = close + 2;
        std::string url;
        int pd = 1;
        while (up < s_.size()) {
            char c = s_[up];
            if (c == '\\' && up + 1 < s_.size()) {
                url += s_[up + 1];
                up += 2;
                continue;
            }
            if (c == '(')
                ++pd;
            else if (c == ')') {
                if (--pd == 0) break;
            }
            url += c;
            ++up;
        }
        if (up >= s_.size() || s_[up] != ')') return false;
        Token tk;
        tk.kind = Kind::Image;
        tk.text = s_.substr(j + 1, close - (j + 1));  // raw alt text
        tk.url = trim(url);
        toks_.push_back(std::move(tk));
        i = up + 1;
        return true;
    }

    // [text](url): link text is inline-rendered by a *separate* renderer over just that slice,
    // which is cheap because link text is short and contains no further links to recurse on here.
    bool scanLink(size_t& i) {
        // Find the matching ']' for the opening '[' (no nested brackets expected in our inputs).
        size_t close = i + 1;
        int depth = 1;
        while (close < s_.size()) {
            if (s_[close] == '\\' && close + 1 < s_.size()) {
                close += 2;
                continue;
            }
            if (s_[close] == '[')
                ++depth;
            else if (s_[close] == ']') {
                if (--depth == 0) break;
            }
            ++close;
        }
        if (close >= s_.size() || close + 1 >= s_.size() || s_[close + 1] != '(') return false;
        size_t up = close + 2;
        std::string url;
        int pd = 1;
        while (up < s_.size()) {
            char c = s_[up];
            if (c == '\\' && up + 1 < s_.size()) {
                url += s_[up + 1];
                up += 2;
                continue;
            }
            if (c == '(')
                ++pd;
            else if (c == ')') {
                if (--pd == 0) break;
            }
            url += c;
            ++up;
        }
        if (up >= s_.size() || s_[up] != ')') return false;
        Token tk;
        tk.kind = Kind::Link;
        tk.text = InlineRenderer(s_.substr(i + 1, close - (i + 1))).render();
        tk.url = trim(url);
        toks_.push_back(std::move(tk));
        i = up + 1;
        return true;
    }

    // A maximal run of identical '*' or '_'. Decide open/close capability from flanking chars.
    void scanDelim(size_t& i) {
        char d = s_[i];
        size_t run = 0;
        while (i + run < s_.size() && s_[i + run] == d) ++run;
        char before = prevAt(i);
        char after = charAt(i + run);
        bool spaceBefore = isSpace(before), spaceAfter = isSpace(after);
        Token tk;
        tk.kind = Kind::Delim;
        tk.delim = d;
        tk.count = static_cast<int>(run);
        tk.canOpen = !spaceAfter;
        tk.canClose = !spaceBefore;
        if (d == '_') {
            // Intraword underscores neither open nor close (keeps snake_case intact).
            if (isWordChar(static_cast<unsigned char>(before))) tk.canOpen = false;
            if (isWordChar(static_cast<unsigned char>(after))) tk.canClose = false;
        }
        toks_.push_back(std::move(tk));
        i += run;
    }

    // --- Pass 2 -------------------------------------------------------------
    void resolveEmphasis() {
        std::vector<size_t> stack;  // indices of Delim tokens that can still open
        for (size_t idx = 0; idx < toks_.size(); ++idx) {
            Token& t = toks_[idx];
            if (t.kind != Kind::Delim) continue;
            if (t.canClose) {
                // Try to match against the nearest compatible opener on the stack.
                while (t.count > 0 && !stack.empty()) {
                    bool matched = false;
                    for (size_t k = stack.size(); k-- > 0;) {
                        Token& o = toks_[stack[k]];
                        if (o.delim == t.delim && o.canOpen && o.count > 0) {
                            int use = (t.count >= 2 && o.count >= 2) ? 2 : 1;
                            if (use == 2) {
                                o.openStrong++;
                                t.closeStrong++;
                            } else {
                                o.openEm++;
                                t.closeEm++;
                            }
                            o.count -= use;
                            t.count -= use;
                            if (o.count == 0)
                                stack.resize(k);  // opener spent; drop it and any above
                            matched = true;
                            break;
                        }
                    }
                    if (!matched) break;
                }
            }
            if (t.canOpen && t.count > 0) stack.push_back(idx);
        }
    }

    // --- Emit ---------------------------------------------------------------
    std::string emit() {
        std::string out;
        for (const Token& t : toks_) {
            switch (t.kind) {
            case Kind::Text: out += t.text; break;
            case Kind::Code: out += kCodeOn + t.text + kCodeOff; break;
            case Kind::Link:
                out += "\033]8;;" + resolveLinkTarget(t.url) + "\033\\" + t.text + "\033]8;;\033\\";
                break;
            case Kind::Image:
                // Inline image: show alt text (we can't fetch remote URLs inline).
                out += t.text;
                break;
            case Kind::Delim: {
                // Closers emit their off-codes first, then any opens, then leftover literals.
                for (int k = 0; k < t.closeEm; ++k) out += kItalicOff;
                for (int k = 0; k < t.closeStrong; ++k) out += kBoldOff;
                for (int k = 0; k < t.openStrong; ++k) out += kBoldOn;
                for (int k = 0; k < t.openEm; ++k) out += kItalicOn;
                // Any delimiter characters not consumed by a pair stay as literal text.
                out.append(static_cast<size_t>(t.count), t.delim);
                break;
            }
            }
        }
        return out;
    }
};

std::string renderInline(const std::string& text) {
    return InlineRenderer(text).render();
}

// ---------------------------------------------------------------------------
// Image rendering: <img ...> -> timg
// ---------------------------------------------------------------------------
//
// As an exception to the "inline HTML is literal" rule, a paragraph or table cell whose entire
// content is a single <img ...> tag referring to a local supported image file is rendered as a
// real image. We let timg(1) paint it with sixel graphics. For PNG we read the intrinsic pixel
// size from its fixed header; for other formats (JPG, GIF, SVG) we verify the file exists and
// rely on the width/height attributes (or let timg fill the available width). If anything goes
// wrong (unsupported format, file unreadable, timg missing or failing) we fall back to the tag's
// alt text, or the literal tag.

// Supported image extensions (lowercase). timg handles all of these.
bool isSupportedImageExt(const std::string& path) {
    static const char* exts[] = {".png", ".jpg", ".jpeg", ".gif", ".svg", nullptr};
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (int i = 0; exts[i]; ++i)
        if (ext == exts[i]) return true;
    return false;
}

// Verify that path is a supported image format and the file exists.
bool readImageSize(const std::string& path) {
    if (!isSupportedImageExt(path)) return false;
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

// Pixel width of a PNG file, read from its IHDR chunk (a big-endian uint32 at byte offset 16, right
// after the 8-byte signature, the 4-byte chunk length and the 4-byte "IHDR" type). Returns 0 if the
// file is not a readable PNG. Used to learn a rendered diagram's natural size for scaling.
int pngWidth(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return 0;
    unsigned char h[24];
    f.read(reinterpret_cast<char*>(h), sizeof h);
    if (f.gcount() < static_cast<std::streamsize>(sizeof h)) return 0;
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8; ++i)
        if (h[i] != sig[i]) return 0;
    return (h[16] << 24) | (h[17] << 16) | (h[18] << 8) | h[19];
}

// Whitespace test usable on signed char without the locale surprises of std::isspace.
inline bool isSpaceCh(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Scan a markdown image ![alt](src) starting at s[i]. On success, sets alt/src and advances i
// past the closing ')'; returns true. i must point at '!'.
static bool scanMdImageAt(const std::string& s, size_t& i, std::string& alt, std::string& src) {
    size_t n = s.size();
    if (i + 4 >= n || s[i] != '!' || s[i + 1] != '[') return false;
    size_t close = i + 2;
    int depth = 1;
    while (close < n) {
        if (s[close] == '\\' && close + 1 < n) {
            close += 2;
            continue;
        }
        if (s[close] == '[')
            ++depth;
        else if (s[close] == ']') {
            if (--depth == 0) break;
        }
        ++close;
    }
    if (close >= n || close + 1 >= n || s[close + 1] != '(') return false;
    alt = s.substr(i + 2, close - (i + 2));
    size_t up = close + 2;
    std::string url;
    int pd = 1;
    while (up < n) {
        char c = s[up];
        if (c == '\\' && up + 1 < n) {
            url += s[up + 1];
            up += 2;
            continue;
        }
        if (c == '(')
            ++pd;
        else if (c == ')') {
            if (--pd == 0) break;
        }
        url += c;
        ++up;
    }
    if (up >= n || s[up] != ')') return false;
    src = trim(url);
    i = up + 1;
    return true;
}

// Parse markdown image syntax into an attrs map with "alt", "src" (and optionally "href") keys.
// Handles plain images ![alt](src) and linked images [![alt](src)](href).
// Returns false if `text` is not exactly one such pattern with nothing before or after it.
bool parseMdImage(const std::string& text, std::map<std::string, std::string>& attrs) {
    const std::string& s = text;
    size_t n = s.size();

    // [![alt](src)](href) — linked image
    if (n > 5 && s[0] == '[' && s[1] == '!') {
        size_t i = 1;  // points at '!'
        std::string alt, src;
        if (scanMdImageAt(s, i, alt, src) && i < n && s[i] == ']' && i + 1 < n && s[i + 1] == '(') {
            ++i;  // skip ']'
            size_t up = i + 1;
            std::string href;
            int pd = 1;
            while (up < n) {
                char c = s[up];
                if (c == '\\' && up + 1 < n) {
                    href += s[up + 1];
                    up += 2;
                    continue;
                }
                if (c == '(')
                    ++pd;
                else if (c == ')') {
                    if (--pd == 0) break;
                }
                href += c;
                ++up;
            }
            if (up < n && s[up] == ')' && up + 1 == n) {
                attrs["alt"] = alt;
                attrs["src"] = src;
                attrs["href"] = trim(href);
                return true;
            }
        }
    }

    // ![alt](src) — plain image
    if (n > 4 && s[0] == '!') {
        size_t i = 0;
        std::string alt, src;
        if (scanMdImageAt(s, i, alt, src) && i == n) {
            attrs["alt"] = alt;
            attrs["src"] = src;
            return true;
        }
    }

    return false;
}

// Parse a single HTML start tag of the form <img key="value" key2="value2" ...> into a map of
// attribute names (lowercased) to values. Only the simple quoted-value form described by the task
// is recognised. Returns false if `text` (already trimmed) is not exactly one <img ...> tag with
// nothing before or after it. Values may be single- or double-quoted.
bool parseImgTag(const std::string& text, std::map<std::string, std::string>& attrs) {
    const std::string& s = text;
    size_t i = 0, n = s.size();
    if (n < 5 || s[0] != '<') return false;
    // Tag name must be "img" (case-insensitive), followed by whitespace or '>'.
    if (!(std::tolower(s[1]) == 'i' && std::tolower(s[2]) == 'm' && std::tolower(s[3]) == 'g'))
        return false;
    if (!(isSpaceCh(s[4]) || s[4] == '>' || s[4] == '/')) return false;
    i = 4;
    while (i < n) {
        while (i < n && isSpaceCh(s[i])) ++i;
        if (i < n && s[i] == '/') ++i;  // tolerate a self-closing slash
        while (i < n && isSpaceCh(s[i])) ++i;
        if (i >= n) return false;            // no closing '>'
        if (s[i] == '>') return i == n - 1;  // the tag must end the string
        // attribute name
        size_t ks = i;
        while (i < n &&
               (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '_'))
            ++i;
        if (i == ks) return false;  // expected a name
        std::string key = s.substr(ks, i - ks);
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        while (i < n && isSpaceCh(s[i])) ++i;
        std::string value;
        if (i < n && s[i] == '=') {
            ++i;
            while (i < n && isSpaceCh(s[i])) ++i;
            if (i < n && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i++];
                size_t vs = i;
                while (i < n && s[i] != q) ++i;
                if (i >= n) return false;  // unterminated quote
                value = s.substr(vs, i - vs);
                ++i;  // consume closing quote
            } else {  // unquoted value up to whitespace or '>'
                size_t vs = i;
                while (i < n && !isSpaceCh(s[i]) && s[i] != '>') ++i;
                value = s.substr(vs, i - vs);
            }
        }
        attrs[key] = value;
    }
    return false;  // ran off the end without a closing '>'
}

// Complete a half-open timg geometry ("Wx" or "xH") into a full "WxH" box by filling the missing
// dimension with a deliberately loose bound that can never become the binding constraint, so the
// requested dimension governs and the image keeps its own aspect ratio. We do this ourselves rather
// than letting timg fill the blank from the terminal: timg >= 1.6.3 reads the terminal size via the
// controlling tty's window-size ioctl, and when that is unavailable (stdout on a pipe with no
// usable controlling tty) it refuses a partial -g with "Failed to read size from terminal" and
// emits no output. Supplying a full box keeps mdcat working regardless of timg version or how it is
// invoked. The loose bound is the terminal extent in that axis with a generous floor; mdcat reads
// the actual painted size back from the sixel afterward, so over-specifying costs nothing.
std::string completeGeom(const std::string& geom) {
    // A loose bound large enough never to clip: the terminal extent floored at 1000 cells so a
    // small or undetectable terminal can't accidentally bind the result.
    int loose = std::max(1000, fullTerminalWidth());
    size_t x = geom.find('x');
    // No box at all: timg still needs a size, and on a pipe with no usable controlling tty it
    // cannot get one from the terminal. Bound the width to the available columns with a loose
    // height; timg fits the image inside, so a smaller image keeps its intrinsic size and a larger
    // one is scaled down to the column budget — the same effect timg's own terminal default would
    // give.
    if (geom.empty() || x == std::string::npos)
        return std::to_string(fullTerminalWidth()) + "x" + std::to_string(loose);
    bool haveW = x > 0;
    bool haveH = x + 1 < geom.size();
    if (haveW && haveH) return geom;                             // already a full "WxH": leave it
    if (haveW) return geom + std::to_string(loose);              // "Wx" -> "WxLOOSE"
    if (haveH) return std::to_string(loose) + geom;              // "xH" -> "LOOSExH"
    return std::to_string(loose) + "x" + std::to_string(loose);  // bare "x": both loose
}

// Run timg to render `path` at the given -g geometry with sixel graphics, returning its stdout.
// `geom` is a timg geometry string in character cells: "WxH" (a full box), "Wx" (width only, height
// derived from the aspect ratio) or "xH" (height only, width derived). Partial geometry lets a
// single requested dimension govern, with the other following the true aspect ratio, instead of our
// own rounding double-constraining the box; completeGeom turns it into a full box before invoking
// timg so a piped timg never has to query the terminal. Returns an empty string if timg cannot be
// launched or exits non-zero.
//
// The image data is captured (rather than letting timg draw straight to the terminal) for two
// reasons: with its stdout on a pipe timg emits plain sixel without doing its own cursor/scroll
// positioning, so we can later replay the bytes at whatever cursor position we choose; and we can
// read the painted pixel size out of the sixel to compute an exact cell footprint.
std::string runTimg(const std::string& path, const std::string& geomIn) {
    std::string geom = completeGeom(geomIn);
    // The -g box arrives in REAL terminal cells, but timg (run headless, stdin detached) sizes it
    // with its own fixed 9x18 px cell, painting pixels that occupy more of the terminal's narrower
    // cells than requested. Rescale each axis from real cells to timg cells so the painted pixels
    // match the columns mdcat reserves. completeGeom always returns a full "WxH", so both fields
    // are present.
    if (size_t x = geom.find('x'); x != std::string::npos) {
        CellMetrics cm = cellMetrics();
        int w = std::atoi(geom.substr(0, x).c_str());
        int h = std::atoi(geom.substr(x + 1).c_str());
        if (w > 0 && h > 0)
            geom =
                std::to_string(cm.colsToTimgCols(w)) + "x" + std::to_string(cm.rowsToTimgRows(h));
    }
    std::ostringstream cmd;
    // -ps : sixel pixelation;  -g <geom> : fit inside the given character-cell box.  The path is
    // passed single-quoted with embedded single quotes escaped, so odd filenames stay safe.
    std::string quoted = "'";
    for (char c : path) {
        if (c == '\'')
            quoted += "'\\''";
        else
            quoted += c;
    }
    quoted += "'";
    // stdin from /dev/null: timg only writes its sixel/Kitty bytes to stdout (the pipe). With stdin
    // left on the controlling tty, timg puts the terminal into raw mode (no ECHO/ICANON) to query
    // it, then restores it on exit — but when several timg processes run concurrently (ADR 0003),
    // their save/restore races and the last one restores a stale "echo off" state, leaving the
    // user's terminal dead. Detaching stdin stops timg touching terminal modes at all; we already
    // pass an explicit -g box and protocol, so it has nothing to auto-detect. -b <bg>: fill the
    // height padding (timg rounds up to a 6px sixel band) and any alpha with the terminal's
    // background colour instead of timg's default black, which would otherwise paint a dark line
    // under a light image. Omitted when the terminal didn't answer OSC 11 (timg keeps its default).
    std::string bg = queryBackgroundColor();
    std::string bgOpt = bg.empty() ? std::string() : " -b '" + bg + "'";
    if (!geom.empty())
        cmd << "timg -ps" << bgOpt << " -g" << geom << " " << quoted << " </dev/null 2>/dev/null";
    else
        cmd << "timg -ps" << bgOpt << " " << quoted << " </dev/null 2>/dev/null";
    FILE* p = popen(cmd.str().c_str(), "r");
    if (!p) return std::string();
    std::string out;
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, got);
    int rc = pclose(p);
    if (rc != 0) return std::string();
    return out;
}

// Run timg to render `path` into the given -g cell box with the KITTY graphics protocol (-pk),
// returning its stdout: a Kitty APC (ESC _ G a=T,...,q=2,f=100,m=...; <base64 PNG> ESC \), chunked
// for large images. Unlike the sixel path, timg here DOWNSCALES the image to the -g box with proper
// interpolation (so the terminal does no quality-losing scaling — VSCode's terminal in particular
// has no anti-aliasing), and emits a well-formed APC: q=2 on every chunk, correct m= chunking. timg
// requires a -g box when headless (piped), so completeGeom fills any missing dimension. Returns
// empty on failure. The caller rewrites the image id to a unique value (see kittyRewriteId).
std::string runTimgKitty(const std::string& path, const std::string& geomIn) {
    std::string geom = completeGeom(geomIn);
    if (geom.empty()) geom = "100x100";  // timg -pk needs a box headless; a loose default
    std::string quoted = "'";
    for (char c : path) {
        if (c == '\'')
            quoted += "'\\''";
        else
            quoted += c;
    }
    quoted += "'";
    std::ostringstream cmd;
    // stdin from /dev/null so concurrent timg processes never race on terminal raw-mode
    // save/restore and leave the tty with echo disabled — see runTimg's note.
    cmd << "timg -pk -g" << geom << " " << quoted << " </dev/null 2>/dev/null";
    FILE* p = popen(cmd.str().c_str(), "r");
    if (!p) return std::string();
    std::string out;
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, got);
    int rc = pclose(p);
    if (rc != 0) return std::string();
    // timg wraps its Kitty output with its own cursor management — a leading "ESC[?25l" (hide
    // cursor) and a trailing newline + "ESC[?25h" (show cursor). We strip these so the captured
    // bytes are the bare Kitty APC sequence: mdcat positions and reserves space for the image
    // itself (emitImageParagraph with DECSC/DECRC), and leaving timg's stray newline in would
    // advance the cursor mid-band and corrupt the placement of tall, multi-chunk images.
    auto strip = [&](const std::string& lead) {
        if (out.compare(0, lead.size(), lead) == 0) out.erase(0, lead.size());
    };
    strip("\033[?25l");
    auto stripTail = [&](const std::string& tail) {
        if (out.size() >= tail.size() &&
            out.compare(out.size() - tail.size(), tail.size(), tail) == 0)
            out.erase(out.size() - tail.size());
    };
    stripTail("\033[?25h");
    stripTail("\n");
    return out;
}

// A per-run monotonic Kitty image id. timg assigns the SAME id to every render of a given file, so
// two images in one document would collide — a later transmit replaces the earlier image and
// re-lays its placement. Handing out a fresh id per image keeps them independent. Atomic so
// parallel image workers (ADR 0003) can mint ids concurrently without colliding or racing on the
// counter.
uint32_t nextKittyId() {
    static std::atomic<uint32_t> id{1000};
    return ++id;
}

// Sentinel kittyId for renderImageBlock: "leave timg's default id; the id is stamped later, in
// document order, on the writer thread" (ADR 0003 deferred slots). Distinct from 0 (mint now).
constexpr uint32_t kKittyIdDefer = 0xFFFFFFFFu;

// Rewrite the i= value on the FIRST Kitty controls segment of `apc` to `id` (byte-exact, leaving
// the base64 payload and all chunk continuations untouched). timg's output is otherwise already
// correct (q=2 on every chunk, f=100, proper m= chunking), so this is the only edit mdcat needs to
// make.
std::string kittyRewriteId(const std::string& apc, uint32_t id) {
    size_t g = apc.find("\033_G");
    if (g == std::string::npos) return apc;
    size_t keyStart = g + 3;
    size_t semi = apc.find(';', keyStart);  // controls run up to the first ';'
    if (semi == std::string::npos) return apc;
    size_t ip = apc.find("i=", keyStart);
    if (ip == std::string::npos || ip > semi) return apc;
    size_t numStart = ip + 2;
    size_t numEnd = numStart;
    while (numEnd < semi && std::isdigit((unsigned char)apc[numEnd])) ++numEnd;
    return apc.substr(0, numStart) + std::to_string(id) + apc.substr(numEnd);
}

// Renumber the i= id of every Kitty image in `bytes`, in order, with successive ids from
// nextKittyId(). Each image's FIRST chunk carries the controls run with i=; continuation chunks
// (m=) carry no i= and are left alone, so exactly one fresh id is minted per image. Used by the
// writer thread (ADR 0003) to stamp ids in document (slot) order, regardless of the order parallel
// workers produced the bytes — keeping output byte-identical to the serial renderer, which minted
// ids in document order on one thread. `bytes` may hold zero, one, or many images (a single
// deferred <img>, or a whole table row).
std::string kittyRenumberAll(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size());
    size_t pos = 0;
    for (;;) {
        size_t g = bytes.find("\033_G", pos);
        if (g == std::string::npos) {
            out.append(bytes, pos, std::string::npos);
            break;
        }
        size_t semi = bytes.find(';', g + 3);
        size_t ip = bytes.find("i=", g + 3);
        out.append(bytes, pos, g - pos);  // bytes before this APC
        if (semi == std::string::npos || ip == std::string::npos || ip > semi) {
            // No i= in this controls run: a continuation chunk (or not an image). Copy "\033_G"
            // only and continue scanning just past it.
            out.append("\033_G");
            pos = g + 3;
            continue;
        }
        // An image's first chunk: rewrite its i= to the next document-order id and copy through the
        // ';'.
        out.append(kittyRewriteId(bytes.substr(g, semi - g + 1), nextKittyId()));
        pos = semi + 1;
    }
    return out;
}

// Inject (or replace) the cell-footprint controls c=<cols>,r=<rows> on the FIRST Kitty controls
// segment of `apc`, byte-exact. timg emits no c=/r=, so the terminal would otherwise derive the
// footprint by dividing the PNG's pixel size by ITS OWN cell size — which differs from the ~9px
// cell timg assumed when it sized the image (it has no terminal to query on a pipe). That mismatch
// makes a width-bound table image paint wider than the column mdcat reserved, overlapping the next
// cell. With c=/r= the terminal scales the image into EXACTLY that cell box, so the painted
// footprint always equals mdcat's reservation. (iTerm requires BOTH c and r and honours them
// exactly — verified via tools/probe-kitty-aspect.sh — so we always set both; aspect is preserved
// because cols and rows are computed from the same painted pixels and the same real cell ratio.)
std::string kittyRewriteFootprint(const std::string& apc, int cols, int rows) {
    if (cols <= 0 || rows <= 0) return apc;
    size_t g = apc.find("\033_G");
    if (g == std::string::npos) return apc;
    size_t semi = apc.find(';', g + 3);  // controls run up to the first ';'
    if (semi == std::string::npos) return apc;
    // Strip any existing c=/r= keys from the controls run so we never duplicate them.
    std::string ctrls = apc.substr(g + 3, semi - (g + 3));
    std::string kept;
    size_t i = 0;
    while (i < ctrls.size()) {
        size_t comma = ctrls.find(',', i);
        std::string kv =
            ctrls.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
        if (kv.compare(0, 2, "c=") != 0 && kv.compare(0, 2, "r=") != 0) {
            if (!kept.empty()) kept += ',';
            kept += kv;
        }
        if (comma == std::string::npos) break;
        i = comma + 1;
    }
    std::string footprint = ",c=" + std::to_string(cols) + ",r=" + std::to_string(rows);
    return apc.substr(0, g + 3) + kept + footprint + apc.substr(semi);
}

// Read a PNG's pixel width/height from the IHDR (big-endian uint32s at byte offsets 16 and 20,
// right after the 8-byte signature + 4-byte length + "IHDR"). `png` is the raw PNG bytes. Returns
// false if the buffer is too short or not a PNG.
bool pngSize(const std::string& png, int& w, int& h) {
    if (png.size() < 24) return false;
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8; ++i)
        if (static_cast<unsigned char>(png[i]) != sig[i]) return false;
    auto u32 = [&](size_t o) {
        return (static_cast<unsigned char>(png[o]) << 24) |
            (static_cast<unsigned char>(png[o + 1]) << 16) |
            (static_cast<unsigned char>(png[o + 2]) << 8) | static_cast<unsigned char>(png[o + 3]);
    };
    w = u32(16);
    h = u32(20);
    return w > 0 && h > 0;
}

// Decode the base64 PNG payload out of a Kitty APC (concatenating all chunks) and read its pixel
// size, so the caller can compute the exact cell footprint timg's downscaled image will occupy.
// Returns false if no PNG could be recovered. Only a small prefix is needed (the IHDR), but timg's
// first chunk already contains it, so we decode just the first chunk's payload.
bool kittyImageSize(const std::string& apc, int& pw, int& ph) {
    size_t g = apc.find("\033_G");
    if (g == std::string::npos) return false;
    size_t semi = apc.find(';', g);
    if (semi == std::string::npos) return false;
    size_t end = apc.find("\033\\", semi);
    if (end == std::string::npos) return false;
    std::string b64 = apc.substr(semi + 1, end - semi - 1);
    // Minimal base64 decode (standard alphabet, no whitespace in Kitty payloads).
    static const std::string tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (char c : b64) {
        if (c == '=') break;
        size_t pos = tbl.find(c);
        if (pos == std::string::npos) continue;
        val = (val << 6) | static_cast<int>(pos);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
        if (out.size() >= 24) break;  // enough for the IHDR
    }
    return pngSize(out, pw, ph);
}

// Read the painted pixel size out of a sixel data stream's raster attributes. A sixel begins
// ESC P <params> q "Pan;Pad;Ph;Pv ... where Ph/Pv are the graphic's pixel width/height. We scan for
// the '"' that introduces them and parse the last two of the four numbers. Returns false if absent
// (older sixel without raster attributes), in which case callers fall back to the requested size.
bool sixelPixelSize(const std::string& sixel, int& pw, int& ph) {
    size_t q = sixel.find("\033P");
    if (q == std::string::npos) return false;
    size_t quote = sixel.find('"', q);
    if (quote == std::string::npos) return false;
    int pan = 0, pad = 0, w = 0, h = 0;
    if (std::sscanf(sixel.c_str() + quote + 1, "%d;%d;%d;%d", &pan, &pad, &w, &h) == 4 && w > 0 &&
        h > 0) {
        pw = w;
        ph = h;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Image worker thread pool (ADR 0003)
// ---------------------------------------------------------------------------
//
// A fixed pool of N = hardware-concurrency workers that run image-conversion tasks (timg / mmdc
// subprocesses) off the render thread. Each task is a std::function returning void and captures its
// own inputs/outputs, so the pool stays agnostic about ImageJob shape; callers submit() a packaged
// task and wait on the returned future. For now (step 2) call sites submit and immediately .get(),
// so behaviour and output are unchanged — later steps overlap the work.
class ThreadPool {
public:
    explicit ThreadPool(unsigned n) {
        for (unsigned i = 0; i < n; ++i) workers_.emplace_back([this] { workerLoop(); });
    }
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }

    // Enqueue `fn` and return a future for its result. The task runs on some worker thread.
    template <class F>
    std::future<typename std::result_of<F()>::type> submit(F&& fn) {
        using R = typename std::result_of<F()>::type;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(mu_);
            queue_.emplace([task] { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

private:
    void workerLoop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                job = std::move(queue_.front());
                queue_.pop();
            }
            job();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> queue_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
};

// The process-wide image pool, created on first use. Sized to hardware concurrency (at least one
// worker). Lazy + leaked-on-exit via a function-local static so terminal probes (graphicsBackend,
// cellMetrics) have run on the main thread before any worker touches them.
ThreadPool& imagePool() {
    static ThreadPool pool(std::max(1u, std::thread::hardware_concurrency()));
    return pool;
}

// If `text` (already trimmed) is a single <img> tag for a local PNG, render it and return true,
// writing the raw sixel bytes to `out` and the image's exact cell footprint to `cellWidth` /
// `cellHeight` (columns / rows). Otherwise return false and leave the outputs untouched.
// `availWidth` caps the width in columns. On any failure to produce an image, the alt text (or
// literal tag) is returned as a one-line text block (cellHeight == 1) so callers can treat the
// result uniformly.
//
// `out` is the plain sixel as timg produced it — it does NOT position itself. The caller is
// responsible for placing the cursor before replaying it: timg leaves the cursor at column 1 on the
// row just below the image, so a sixel painted at the top of a reserved block lands within it. The
// footprint is computed from the painted pixel size read back out of the sixel (which reflects
// timg's aspect-preserving fit), divided by the real cell size — not from the requested -g box.
// `kittyId` controls the Kitty image id stamped on a successful Kitty render: zero mints one now
// from nextKittyId() (the table and synchronous paths, which run in encounter order); a nonzero
// value is used verbatim; the sentinel kKittyIdDefer leaves timg's default id untouched, so a
// deferred slot (ADR 0003) can stamp a document-order id on the writer thread after parallel —
// possibly out-of-order — conversion completes, keeping output byte-identical to the serial
// renderer.
bool renderImageBlock(const std::string& text,
                      int availWidth,
                      const std::string& fileDir,
                      std::string& out,
                      int& cellWidth,
                      int& cellHeight,
                      bool forceWidthBound = false,
                      uint32_t kittyId = 0) {
    std::map<std::string, std::string> attrs;
    if (!parseImgTag(text, attrs) && !parseMdImage(text, attrs)) return false;

    auto srcIt = attrs.find("src");

    // Fall back to a one-line text block when an image can't (or shouldn't) be drawn: the alt text
    // if present, else the source filename, else the literal tag. Used both for error cases and for
    // terminals that cannot display graphics at all.
    auto fallback = [&](const std::string& reason) {
        (void)reason;
        auto altIt = attrs.find("alt");
        std::string label;
        if (altIt != attrs.end() && !altIt->second.empty())
            label = altIt->second;
        else if (srcIt != attrs.end() && !srcIt->second.empty())
            label = srcIt->second;
        else
            label = text;
        auto hrefIt = attrs.find("href");
        if (hrefIt != attrs.end() && !hrefIt->second.empty())
            out = "\033]8;;" + resolveLinkTarget(hrefIt->second) + "\033\\" + renderInline(label) +
                "\033]8;;\033\\";
        else
            out = renderInline(label);
        cellWidth = displayWidth(out);
        cellHeight = 1;
        return true;
    };

    // On a terminal that can't render sixel graphics, show the alt text or filename instead.
    if (!terminalSupportsGraphics()) return fallback("no graphics support");

    if (srcIt == attrs.end() || srcIt->second.empty()) return fallback("no src");
    // Resolve relative paths against the directory of the file being rendered. fileDir is passed in
    // (a snapshot of gFileDir) rather than read from the global, so image workers stay global-free.
    std::string src = srcIt->second;
    if (!fileDir.empty() && src[0] != '/') src = fileDir + "/" + src;

    if (!readImageSize(src)) return fallback("not a supported image");

    // Target pixel size: an explicit width/height overrides the intrinsic size. With only one of
    // the two given, the other is derived to preserve the aspect ratio; with both, the ratio is
    // free. When the intrinsic size is unknown (non-PNG formats return 0×0), only attribute
    // dimensions are available; if neither is given we fall back to a width-bound geometry below.
    auto attrInt = [&](const char* key, int& dst) {
        auto it = attrs.find(key);
        if (it == attrs.end()) return false;
        int v = std::atoi(it->second.c_str());
        if (v <= 0) return false;
        dst = v;
        return true;
    };
    int tw = 0, th = 0;
    int aw = 0, ah = 0;
    bool haveW = attrInt("width", aw), haveH = attrInt("height", ah);
    if (haveW) tw = aw;
    if (haveH) th = ah;

    // Decide the -g geometry to pass to timg. Rules, in priority order:
    //   - forceWidthBound: caller (table layout) demands an exact column width.
    //   - Image is wider than available columns: clamp to the column budget.
    //   - Explicit height only: height-bound, width follows aspect ratio.
    //   - Explicit width or both dimensions: full box.
    //   - No constraint at all (no attrs, intrinsic size fits or is unknown): omit -g entirely
    //     and let timg choose its own size; the footprint is read back from the sixel header.
    CellMetrics cell = cellMetrics();
    int wCells = cell.pxToColsNominal(tw);
    int hCells = cell.pxToRowsNominal(th);
    std::string geom;
    if (forceWidthBound && availWidth > 0)
        geom = std::to_string(availWidth) + "x";  // caller demands an exact column width
    else if (availWidth > 0 && wCells > availWidth)
        geom = std::to_string(availWidth) + "x";  // width-bound: intrinsic size too wide
    else if (haveH && !haveW)
        geom = "x" + std::to_string(hCells);  // height-bound: width follows aspect
    else if (haveW || haveH)
        geom = std::to_string(wCells) + "x" + std::to_string(hCells);  // explicit box
    // else: geom stays empty — no -g, timg picks its own size

    std::string img;
    if (graphicsBackend() == GraphicsBackend::Kitty) {
        // Kitty path: timg downscales into the -g box and emits a chunked Kitty APC; we only
        // rewrite its image id to a unique value. The footprint is the painted pixel size (read
        // from the embedded PNG's IHDR) converted with the precise area ratio, falling back to
        // requested cells.
        img = runTimgKitty(src, geom);
        if (img.empty()) return fallback("timg failed");
        if (kittyId != kKittyIdDefer) img = kittyRewriteId(img, kittyId ? kittyId : nextKittyId());
        int paintedW = 0, paintedH = 0;
        if (kittyImageSize(img, paintedW, paintedH)) {
            // Width is the binding axis (it is what a table column reserves, and the axis the
            // original overflow was on): the column count is the painted width in cells, but never
            // more than the caller's cap — timg sizes the -g box with a fixed ~9px cell, so the
            // painted pixels can map to a column or two more than reserved (the 38-vs-33 case in
            // table layout). Capping here, then pinning c=/r= below, makes the terminal scale into
            // exactly that box.
            cellWidth = cell.pxToCols(paintedW);
            if (availWidth > 0 && cellWidth > availWidth) cellWidth = availWidth;
            // Derive rows from the columns and the painted image's true aspect, rounded to NEAREST
            // (not ceil): ceil inflates one axis and, when the other axis can't follow, visibly
            // distorts a small image (a 3-column square would become c=3,r=2). Round-nearest keeps
            // c:r as close to the source aspect as the cell grid allows. realCellW/H give the
            // terminal's actual cell shape, so the c:r ratio matches the pixels the terminal will
            // blit into.
            double aspect = paintedW > 0 ? static_cast<double>(paintedH) / paintedW : 1.0;
            double rExact =
                static_cast<double>(cellWidth) * cell.realCellW() * aspect / cell.realCellH();
            cellHeight = std::max(1, static_cast<int>(rExact + 0.5));
        } else {
            cellWidth = wCells;
            cellHeight = hCells;
        }
        // Pin the terminal's cell footprint to the one mdcat reserves, so the image never overflows
        // its column or row band regardless of the local terminal's real cell size. See
        // kittyRewriteFootprint.
        img = kittyRewriteFootprint(img, cellWidth, cellHeight);
    } else {
        // Sixel path (unchanged): timg paints sixel; the footprint comes from the sixel raster
        // header.
        img = runTimg(src, geom);
        if (img.empty()) return fallback("timg failed");
        int paintedW = 0, paintedH = 0;
        if (sixelPixelSize(img, paintedW, paintedH)) {
            cellWidth = cell.pxToCols(paintedW);
            cellHeight = cell.pxToRows(paintedH);
        } else {
            cellWidth = wCells;
            cellHeight = hCells;
        }
    }
    auto hrefIt = attrs.find("href");
    if (hrefIt != attrs.end() && !hrefIt->second.empty())
        out = "\033]8;;" + resolveLinkTarget(hrefIt->second) + "\033\\" + img + "\033]8;;\033\\";
    else
        out = img;
    return true;
}

// Render the source of a ```mermaid fenced code block as a diagram, writing the raw sixel bytes to
// `out` and the painted cell footprint to `cellWidth`/`cellHeight`. Returns true on success; on any
// failure (mmdc missing or erroring, timg failing) returns false so the caller can fall back to
// showing the block as ordinary fenced code.
//
// mmdc(1) (the mermaid CLI) renders the diagram to a temporary PNG, which is then handed to the
// existing <img> pipeline (renderImageBlock) as an absolute path — reusing its geometry/footprint
// and sixel-capture logic exactly. We go through a temp file rather than mmdc's `-o -` stdout mode
// so that renderImageBlock can read the PNG's intrinsic size and width-bound it to the terminal.
// mmdc's -w (page width) is set to the available width in device pixels. Since -w is only a maximum
// (a diagram narrower than the page renders at its natural, often tiny, size on a HiDPI terminal),
// we render once to measure the natural width, then re-render at a -s (scale) chosen to enlarge a
// small diagram toward the column budget — this is what actually fixes "too small" on iTerm2.
bool renderMermaidBlock(const std::vector<std::string>& lines,
                        int availWidth,
                        std::string& out,
                        int& cellWidth,
                        int& cellHeight,
                        uint32_t kittyId = 0) {
    if (!mmdcAvailable()) return false;

    // Unique temp paths for the mermaid source and the rendered PNG. mkstemp creates the files; we
    // only need their names (mmdc writes the PNG itself), so the descriptors are closed
    // immediately.
    char srcPath[] = "/tmp/mdcat-mermaid-XXXXXX";
    char pngPath[] = "/tmp/mdcat-mermaid-XXXXXX.png";
    int sfd = mkstemp(srcPath);
    if (sfd < 0) return false;
    int pfd = mkstemps(pngPath, 4);  // keep the ".png" suffix so it is a valid image extension
    if (pfd < 0) {
        close(sfd);
        unlink(srcPath);
        return false;
    }
    close(pfd);

    // Write the diagram source to the temp file.
    {
        std::string body;
        for (const auto& l : lines) {
            body += l;
            body += '\n';
        }
        ssize_t total = 0, len = static_cast<ssize_t>(body.size());
        while (total < len) {
            ssize_t w = write(sfd, body.data() + total, static_cast<size_t>(len - total));
            if (w <= 0) break;
            total += w;
        }
        close(sfd);
    }

    auto cleanup = [&] {
        unlink(srcPath);
        unlink(pngPath);
    };

    auto shquote = [](const char* p) {
        std::string q = "'";
        for (const char* c = p; *c; ++c) {
            if (*c == '\'')
                q += "'\\''";
            else
                q += *c;
        }
        q += "'";
        return q;
    };

    // If PUPPETEER_EXECUTABLE_PATH is not already set, probe standard macOS Chrome locations so
    // mmdc works even when puppeteer's own chrome-headless-shell binary was not downloaded.
    std::string chromeEnv;
    if (!std::getenv("PUPPETEER_EXECUTABLE_PATH")) {
        static const char* kChromePaths[] = {
            "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
            "/Applications/Chromium.app/Contents/MacOS/Chromium",
        };
        for (const char* p : kChromePaths) {
            if (access(p, X_OK) == 0) {
                chromeEnv = "PUPPETEER_EXECUTABLE_PATH=" + shquote(p) + " ";
                break;
            }
        }
    }

    auto runMmdc = [&](const std::string& opts) {
        // -q suppresses mmdc's progress chatter so it never pollutes the rendered output; stderr is
        // discarded as a further guard.
        std::string cmd = chromeEnv + "mmdc -q" + opts + " -i " + shquote(srcPath) + " -e png -o " +
            shquote(pngPath) + " </dev/null >/dev/null 2>&1";
        return std::system(cmd.c_str()) == 0;
    };

    // Available width in device pixels — the column budget times the real cell width. Used both as
    // mmdc's page width (-w) and as the target the diagram is scaled up toward.
    int availPx = availWidth > 0 ? availWidth * cellMetrics().realCellW() : 0;
    std::string widthOpt = availPx > 0 ? " -w " + std::to_string(availPx) : std::string();

    // Pass the terminal background to mmdc so the diagram background matches the terminal theme.
    // Omitted when the terminal didn't answer OSC 11 (mmdc keeps its default white background).
    std::string bg = queryBackgroundColor();
    std::string bgOpt = bg.empty() ? std::string() : " -b '" + bg + "'";
    if (darkBackground()) bgOpt += " --theme dark";

    // First render at scale 1 to learn the diagram's natural pixel width. mmdc's -w is only a
    // maximum page width: a diagram whose layout is narrower than -w renders at its natural size,
    // which on a HiDPI terminal is a tiny sixel. So if the natural width is well under the
    // available width, pick a -s (Puppeteer scale) that enlarges it to roughly fill the column
    // budget and re-render. Scale is capped at 2 so a small diagram is not blown up to fill most of
    // the screen.
    if (!runMmdc(widthOpt + bgOpt)) {
        cleanup();
        return false;
    }
    int naturalW = pngWidth(pngPath);
    if (availPx > 0 && naturalW > 0) {
        int scale = availPx / naturalW;
        if (scale > 2) scale = 2;
        if (scale >= 2) {
            if (!runMmdc(widthOpt + bgOpt + " -s " + std::to_string(scale))) {
                cleanup();
                return false;
            }
        }
    }

    // Hand the rendered PNG to the <img> pipeline as an absolute path (so gFileDir resolution is a
    // no-op), reusing all of its geometry and sixel-capture logic.
    std::string tag = "<img src=\"" + std::string(pngPath) + "\">";
    // pngPath is absolute, so fileDir resolution is a no-op; pass empty. Forward kittyId so a
    // deferred mermaid slot gets a document-order Kitty id (ADR 0003).
    bool ok = renderImageBlock(tag,
                               availWidth,
                               /*fileDir=*/std::string(),
                               out,
                               cellWidth,
                               cellHeight,
                               /*forceWidthBound=*/false,
                               kittyId);
    cleanup();
    return ok && !out.empty() && cellHeight > 0;
}

// Whether `s` is sixel image data rather than a text fallback: a sixel contains a DCS introducer
// (ESC P), which rendered inline text never does (it uses only ESC[ for SGR and ESC] for OSC 8).
bool isSixelImage(const std::string& s) {
    return s.find("\033P") != std::string::npos;
}

// Whether `s` is Kitty graphics image data: a Kitty APC begins ESC _ G.
bool isKittyImage(const std::string& s) {
    return s.find("\033_G") != std::string::npos;
}

// Whether `s` is an inline image (either protocol) rather than a one-line text fallback.
bool isImageBlock(const std::string& s) {
    return isSixelImage(s) || isKittyImage(s);
}

// Replay a captured image (sixel or Kitty APC) at the cursor's current position, then leave the
// cursor exactly where it started. We bracket the bytes with DECSC/DECRC (ESC 7 / ESC 8): save the
// position, paint, restore. This is terminal-independent — where an image leaves the cursor differs
// between terminals (VSCode advances to the row below; iTerm does not), but the saved position is
// restored either way — so callers can move deterministically afterward without depending on that
// behaviour. Works for both protocols: neither moves the cursor predictably, so save/restore is the
// right tool.
void replaySixel(const std::string& image, std::ostream& out) {
    out << "\0337" << image << "\0338";
}

// Emit a captured sixel `image` (exactly `rows` tall) as a standalone block at the left margin,
// leaving the cursor at column 1 on the line just below it. Scroll-safe and needs no cursor report:
// print `rows` newlines to reserve the band (this scrolls if we are near the screen bottom, making
// room first), move the cursor back up `rows` lines (relative, so unaffected by any scroll), replay
// the sixel (which restores the cursor to the band top), then step down `rows` to the band bottom —
// ready for the next block, just like a text paragraph's trailing newline.
// Build the framed bytes for an image paragraph (the same sequence emitImageParagraph writes) as a
// string, so a deferred slot (ADR 0003) can hold the finished image without an ostream on hand.
std::string imageParagraphBytes(const std::string& image, int rows) {
    std::string s;
    s += std::string(static_cast<size_t>(rows), '\n');  // reserve the band (may scroll)
    s += "\033[" + std::to_string(rows) + 'A';          // back to the band's top row, column 1
    s += "\0337";
    s += image;
    s += "\0338";                                      // paint; cursor restored to band top
    s += "\033[" + std::to_string(rows) + 'B' + '\r';  // down to the band bottom, column 1
    return s;
}

void emitImageParagraph(const std::string& image, int rows, std::ostream& out) {
    out << imageParagraphBytes(image, rows);
}

// The non-tty output sink, defined near main(). If `out` is backed by one, deferred image slots can
// be reserved on it (ADR 0003); otherwise (tty buffer, or a recursive ostringstream) images render
// synchronously inline. Forward-declared (with thin free-function wrappers, defined after the
// class) so the deferred call sites here can detect the sink and reserve a slot without the
// complete type.
class SlotSink;
SlotSink* asSlotSink(std::ostream& out);
void slotDeferImage(SlotSink* sink, std::future<std::string> fut);

// ---------------------------------------------------------------------------
// Block model
// ---------------------------------------------------------------------------

// Length in bytes of the next display "unit" at offset i: a whole ANSI CSI escape, a whole OSC 8
// hyperlink escape, or a single grapheme — a base UTF-8 code point plus any trailing zero-width
// followers (combining marks, variation selectors) and ZWJ-joined code points (e.g. a flag or a
// family emoji). Keeping the grapheme intact means reflow never splits a base from its marks or
// breaks a ZWJ emoji sequence across a line, and the unit's display width stays correct.
size_t unitLength(const std::string& s, size_t i) {
    char c = s[i];
    if (c == kEsc && i + 1 < s.size() && s[i + 1] == '[') {  // CSI: ESC [ ... final @..~
        size_t j = i + 2;
        while (j < s.size() && !(s[j] >= '@' && s[j] <= '~')) ++j;
        if (j < s.size()) ++j;
        return j - i;
    }
    if (c == kEsc && i + 1 < s.size() && s[i + 1] == ']') {  // OSC: ESC ] ... BEL or ST
        size_t j = i + 2;
        while (j < s.size()) {
            if (static_cast<unsigned char>(s[j]) == 0x07) {
                ++j;
                break;
            }
            if (s[j] == kEsc && j + 1 < s.size() && s[j + 1] == '\\') {
                j += 2;
                break;
            }
            ++j;
        }
        return j - i;
    }
    // Base code point, then absorb following zero-width code points and ZWJ-joined sequences so the
    // whole grapheme cluster travels together.
    uint32_t cp;
    size_t j = i + static_cast<size_t>(decodeUtf8(s, i, cp));
    // A regional-indicator pair is one flag grapheme: take both indicators together.
    if (inRange(cp, 0x1F1E6, 0x1F1FF) && j < s.size()) {
        uint32_t cp2;
        size_t adv2 = static_cast<size_t>(decodeUtf8(s, j, cp2));
        if (inRange(cp2, 0x1F1E6, 0x1F1FF)) {
            cp = cp2;
            j += adv2;
        }
    }
    while (j < s.size()) {
        uint32_t next;
        size_t adv = static_cast<size_t>(decodeUtf8(s, j, next));
        bool zwj = (cp == 0x200D);               // previous unit ended in a zero-width joiner
        if (codePointWidth(next) == 0 || zwj) {  // a zero-width follower, or the char after a ZWJ
            j += adv;
            cp = next;
            continue;
        }
        break;
    }
    return j - i;
}

// Keep ANSI styling from bleeding across the line breaks that reflow (or table layout) introduces.
// A code span carries a background colour (kCodeOn .. kCodeOff); if a span wraps, the line would
// end with the background still on and paint gray to the right edge of the terminal. Walk the text
// line by line, tracking whether a code span is open at each '\n': close it before the break and
// reopen it after, so each line is self-contained and no background extends past its text.
std::string closeStylesAtLineBreaks(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool inCode = false;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\n') {
            if (inCode) out += kCodeOff;  // close the span at end of line
            out += '\n';
            if (inCode) out += kCodeOn;  // reopen it at the start of the next line
            ++i;
            continue;
        }
        size_t len = unitLength(s, i);
        if (s.compare(i, kCodeOn.size(), kCodeOn) == 0)
            inCode = true;
        else if (s.compare(i, kCodeOff.size(), kCodeOff) == 0)
            inCode = false;
        out.append(s, i, len);
        i += len;
    }
    return out;
}

// Reflow a rendered (ANSI-styled) string so that no output line exceeds `width` display columns,
// breaking on spaces between words. A word longer than `width` on its own is hard-cut at exactly
// `width` columns and continued on the next line. ANSI escapes don't count toward the width and
// are never split. Line breaks in the result are '\n'; the result has no trailing newline.
std::string reflow(const std::string& s, int width) {
    if (width < 1) width = 1;

    // Split into whitespace-separated words; escapes travel with the word they are attached to. A
    // '\n' (from a <br> hard break) is kept as a standalone "\n" word that forces a line break
    // below.
    std::vector<std::string> words;
    std::string cur;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == ' ' || s[i] == '\n') {
            if (!cur.empty()) {
                words.push_back(cur);
                cur.clear();
            }
            if (s[i] == '\n') words.push_back("\n");
            ++i;
        } else {
            size_t len = unitLength(s, i);
            cur.append(s, i, len);
            i += len;
        }
    }
    if (!cur.empty()) words.push_back(cur);

    std::string out;
    int lineWidth = 0;      // display columns already on the current line
    bool lineEmpty = true;  // nothing placed on the current line yet
    for (std::string w : words) {
        if (w == "\n") {  // forced hard break from a <br>
            out += '\n';
            lineWidth = 0;
            lineEmpty = true;
            continue;
        }
        // A word that cannot fit on a line by itself is hard-cut into width-sized pieces.
        while (displayWidth(w) > width) {
            if (!lineEmpty) {
                out += '\n';
                lineWidth = 0;
                lineEmpty = true;
            }
            std::string piece;
            int pieceWidth = 0;
            size_t i = 0;
            for (; i < w.size();) {
                size_t len = unitLength(w, i);
                int cw = displayWidth(w.substr(i, len));  // 0 for escape, 1 normal, 2 wide
                if (pieceWidth + cw > width) break;
                piece.append(w, i, len);
                pieceWidth += cw;
                i += len;
            }
            // A single unit wider than the whole line (a width-2 grapheme when width==1) fits in no
            // piece. Place it anyway — overflowing one column beats looping forever (w never
            // shrinks).
            if (i == 0) {
                size_t len = unitLength(w, 0);
                piece.append(w, 0, len);
                i = len;
            }
            out += piece;
            out += '\n';
            w = w.substr(i);
        }
        int ww = displayWidth(w);
        if (!lineEmpty && lineWidth + 1 + ww > width) {  // start a new line for this word
            out += '\n';
            lineWidth = 0;
            lineEmpty = true;
        }
        if (!lineEmpty) {
            out += ' ';
            ++lineWidth;
        }
        out += w;
        lineWidth += ww;
        lineEmpty = false;
    }
    return closeStylesAtLineBreaks(out);
}

std::vector<std::string> splitOnNewlines(const std::string& s);  // defined below

void emitParagraph(const std::vector<std::string>& lines, std::ostream& out) {
    // Join the paragraph's source lines (a soft line break becomes a space), render inline markup,
    // then reflow the styled result to the terminal width by display columns.
    std::string joined;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) joined += ' ';
        joined += trim(lines[i]);
    }
    // A paragraph that is just an <img> tag for a local PNG renders as an image instead of text.
    // renderImageBlock returns either a multi-row sixel (ih > 1, or ih == 1 with sixel bytes) or a
    // one-line text fallback (ih == 1, plain text — e.g. on a non-graphics terminal). The sixel is
    // placed via the reserve-and-paint helper; the text fallback is emitted as a normal line.
    std::string imgText = trim(joined);
    int availW = terminalWidth();
    std::string fileDir = gFileDir;
    // Quick non-rendering test: is this paragraph an image tag at all? parseImgTag/parseMdImage are
    // pure and cheap, so checking here avoids submitting a job for ordinary paragraphs. (A
    // graphics- capable check is implicit — renderImageBlock falls back to text when graphics are
    // unavailable.)
    {
        std::map<std::string, std::string> probe;
        bool isImgTag = parseImgTag(imgText, probe) || parseMdImage(imgText, probe);
        if (isImgTag) {
            // Deferred slot (non-tty): submit the conversion, reserve an ordered slot for its
            // framed bytes, and return immediately so later blocks/images proceed in parallel. The
            // worker produces the exact bytes this site would have written: framed image, or text
            // fallback.
            if (SlotSink* sink = asSlotSink(out)) {
                // Defer the Kitty id: the worker leaves timg's default id and the writer stamps a
                // document-order id when it drains this slot, so out-of-order completion stays
                // serial- identical (see kKittyIdDefer and SlotSink::deferImage's stampKittyId).
                std::future<std::string> fut = imagePool().submit([imgText, availW, fileDir] {
                    std::string image;
                    int iw = 0, ih = 0;
                    if (!renderImageBlock(imgText,
                                          availW,
                                          fileDir,
                                          image,
                                          iw,
                                          ih,
                                          /*forceWidthBound=*/false,
                                          kKittyIdDefer) ||
                        image.empty())
                        return std::string();
                    if (isImageBlock(image)) return imageParagraphBytes(image, ih);
                    return image + "\n";  // one-line text fallback
                });
                slotDeferImage(sink, std::move(fut));
                return;
            }
            // Synchronous path (tty buffer or recursive ostringstream): render now and write
            // inline.
            std::string image;
            int iw = 0, ih = 0;
            if (imagePool()
                    .submit(
                        [&] { return renderImageBlock(imgText, availW, fileDir, image, iw, ih); })
                    .get()) {
                if (image.empty()) return;
                if (isImageBlock(image))
                    emitImageParagraph(image, ih, out);
                else
                    out << image << '\n';  // one-line text fallback
                return;
            }
        }
    }
    // A paragraph that is exactly one block-math expression ($$...$$) is centered, the way GitHub
    // (and TeX display math) sets it off on its own centered line. Each rendered line is centered
    // independently within the available width.
    std::string trimmed = trim(joined);
    bool blockMath = trimmed.size() >= 4 && trimmed.compare(0, 2, "$$") == 0 &&
        trimmed.compare(trimmed.size() - 2, 2, "$$") == 0 &&
        trimmed.find("$$", 2) == trimmed.size() - 2;

    std::string reflowed = reflow(renderInline(joined), terminalWidth());
    if (reflowed.empty()) return;
    if (blockMath) {
        for (const std::string& line : splitOnNewlines(reflowed)) {
            int w = displayWidth(line);
            int pad = std::max(0, (terminalWidth() - w) / 2);
            out << std::string(static_cast<size_t>(pad), ' ') << line << '\n';
        }
        return;
    }
    out << reflowed << '\n';
}

void emitHeading(int level, const std::string& text, std::ostream& out) {
    // Bold heading text, followed by an underline whose character depends on the level, matching
    // the original script's "━─┈┄" for levels 1..4 (deeper levels reuse the level-4 character).
    static const std::vector<std::string> kUnderlines = {"━", "─", "┈", "┄"};
    std::string styled = renderInline(text);
    out << kBoldOn << styled << kBoldOff << '\n';
    const std::string& bar = kUnderlines[std::min(level, 4) - 1];
    out << horizontalLine(terminalWidth(), bar) << '\n';
}

void emitThematicBreak(std::ostream& out) {
    out << horizontalLine(terminalWidth()) << '\n';
}

void emitCodeBlock(const std::vector<std::string>& lines,
                   const std::string& lang,
                   std::ostream& out) {
    // Fenced code: print with the light-gray background used for inline code, padded to the
    // width of the widest line so the block reads as a solid panel.  Syntax highlighting is
    // applied when a recognised language tag is present.
    //
    // kCodeOff only resets fg/bg; cancel bold+italic explicitly so highlighted attributes
    // cannot leak past the line boundary into subsequent terminal output.
    const std::string kAttrOff = "\033[22;23m";  // cancel bold (22) and italic (23)
    const auto highlighted = highlightCode(lines, lang);
    int maxw = 0;
    for (const auto& l : lines) maxw = std::max(maxw, displayWidth(l));
    // Clamp the panel width so a row (2-space margins + content) never exceeds the terminal width.
    // Code lines are emitted verbatim (never reflowed), so a line longer than this still overflows
    // and wraps — that is unavoidable — but short lines must not be over-padded to the widest
    // line's width, which would push them past the edge and make the pager wrap them into phantom
    // rows with a broken background (gmore renders into a fixed-width grid; the terminal soft-wraps
    // under cat).
    maxw = std::min(maxw, std::max(1, terminalWidth() - 4));
    for (const auto& l : highlighted) {
        out << kCodeOn << "  " << padTo(l, maxw) << "  " << kAttrOff << kCodeOff << '\n';
    }
}

// Is `line` a GFM table delimiter row, e.g. |---|:--:| ? It must contain only pipes, colons,
// hyphens and spaces, include at least one hyphen, and at least one pipe.
bool isTableDelimiterRow(const std::string& line) {
    bool hasDash = false, hasPipe = false;
    for (char c : line) {
        if (c == '-')
            hasDash = true;
        else if (c == '|')
            hasPipe = true;
        else if (c != ':' && c != ' ' && c != '\t')
            return false;
    }
    return hasDash && hasPipe;
}

// Split a table row on unescaped '|', dropping one optional leading and trailing pipe, and trim
// each resulting cell. Pipes escaped as \| become literal pipes within a cell.
std::vector<std::string> splitTableRow(const std::string& raw) {
    std::string row = trim(raw);
    size_t b = 0, e = row.size();
    if (b < e && row[b] == '|') ++b;
    if (e > b && row[e - 1] == '|') --e;
    std::vector<std::string> cells;
    std::string cur;
    for (size_t i = b; i < e; ++i) {
        if (row[i] == '\\' && i + 1 < e && row[i + 1] == '|') {
            cur += '|';
            ++i;
            continue;
        }
        if (row[i] == '|') {
            cells.push_back(trim(cur));
            cur.clear();
            continue;
        }
        cur += row[i];
    }
    cells.push_back(trim(cur));
    return cells;
}

// Parse the per-column alignment from a delimiter row. Each cell is one of `---` (left, the
// default), `:--` (left), `--:` (right), or `:-:` (center); a leading colon means "left edge
// pinned", a trailing colon "right edge pinned". Cells are split the same way as data rows.
std::vector<Align> parseTableAlignment(const std::string& delimiter) {
    std::vector<Align> aligns;
    for (const std::string& cell : splitTableRow(delimiter)) {
        std::string c = trim(cell);
        bool left = !c.empty() && c.front() == ':';
        bool right = !c.empty() && c.back() == ':';
        if (left && right)
            aligns.push_back(Align::Center);
        else if (right)
            aligns.push_back(Align::Right);
        else
            aligns.push_back(Align::Left);
    }
    return aligns;
}

// Split a string on '\n' into its constituent lines (no trailing empty element added for a final
// newline; an empty string yields a single empty line).
std::vector<std::string> splitOnNewlines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '\n') {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

void emitTable(const std::vector<std::string>& rawRows, std::ostream& out) {
    // rawRows[0] is the header, rawRows[1] the delimiter (consumed by the caller's detection and
    // not printed), and the rest are body rows. Cells are inline-rendered.
    std::vector<std::vector<std::string>> rows;  // rendered cells, header + body
    rows.push_back(splitTableRow(rawRows[0]));
    for (size_t i = 2; i < rawRows.size(); ++i) rows.push_back(splitTableRow(rawRows[i]));

    size_t ncols = 0;
    for (auto& r : rows) ncols = std::max(ncols, r.size());
    for (auto& r : rows) r.resize(ncols);  // pad short rows with empty cells
    if (ncols == 0) return;

    // Per-column alignment from the delimiter row, padded to ncols with the Left default for any
    // columns the delimiter did not specify.
    std::vector<Align> aligns = parseTableAlignment(rawRows[1]);
    aligns.resize(ncols, Align::Left);

    const int W = terminalWidth();
    int budget = W - 2 * static_cast<int>(ncols - 1);
    if (budget < static_cast<int>(ncols)) budget = static_cast<int>(ncols);  // at least 1 col each

    // Render every cell, distinguishing image cells (a lone <img> for a local PNG that produced an
    // actual sixel) from text cells. An image cell holds raw sixel bytes that must never reach
    // displayWidth/reflow, so we record its column width and row height separately and carry the
    // bytes through layout untouched. A non-image result (ordinary text, or an <img> fallback to
    // its alt text/filename when graphics are unavailable) is treated as normal reflowable text.
    std::vector<std::vector<bool>> isImage(rows.size(), std::vector<bool>(ncols, false));
    std::vector<std::vector<int>> imgRows(rows.size(), std::vector<int>(ncols, 0));  // image height
    std::vector<std::vector<int>> imgCellW(rows.size(), std::vector<int>(ncols, 0));  // image width
    std::vector<std::vector<std::string>> imgText(rows.size(), std::vector<std::string>(ncols));
    std::vector<int> imgWidth(ncols, 0);  // forced width of an image column, if any
    // Batch-join (ADR 0003): submit every image-tag cell's conversion to the pool up front, then
    // wait on the whole batch before laying out the table — the column-width algorithm needs every
    // image's footprint, so the table can't be emitted until its own cells are measured, but the
    // conversions run in parallel with each other (and overlap any earlier blocks' deferred images
    // already queued). Per-cell results live in stable storage so each job writes its own slot
    // without racing.
    struct CellImg {
        std::string block;
        int iw = 0, ih = 0;
        bool ok = false;
    };
    std::vector<std::vector<CellImg>> cimg(rows.size(), std::vector<CellImg>(ncols));
    // future is move-only, so the row vectors can't be copy-filled; resize each instead.
    std::vector<std::vector<std::future<void>>> cfut(rows.size());
    for (auto& row : cfut) row.resize(ncols);
    std::string fileDir = gFileDir;
    for (size_t i = 0; i < rows.size(); ++i) {
        for (size_t j = 0; j < ncols; ++j) {
            std::string cellText = trim(rows[i][j]);
            std::map<std::string, std::string> probe;
            if (!parseImgTag(cellText, probe) && !parseMdImage(cellText, probe)) continue;
            imgText[i][j] = cellText;
            CellImg* r = &cimg[i][j];
            // Render at the full content budget so an explicit height is honoured at the image's
            // natural width; a column too narrow to hold it is handled by a re-render after layout.
            cfut[i][j] = imagePool().submit([cellText, budget, fileDir, r] {
                r->ok = renderImageBlock(cellText,
                                         budget,
                                         fileDir,
                                         r->block,
                                         r->iw,
                                         r->ih,
                                         /*forceWidthBound=*/false,
                                         kKittyIdDefer);
            });
        }
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        for (size_t j = 0; j < ncols; ++j) {
            if (cfut[i][j].valid()) cfut[i][j].get();  // join this cell's conversion
            const CellImg& r = cimg[i][j];
            if (r.ok && isImageBlock(r.block)) {
                // The Kitty id is left as timg's default (kKittyIdDefer); the writer renumbers
                // every image in document order when it drains this table's slot.
                rows[i][j] = r.block;
                isImage[i][j] = true;
                imgRows[i][j] = std::max(1, r.ih);
                imgCellW[i][j] = r.iw;
                imgWidth[j] = std::max(imgWidth[j], r.iw);
            } else if (r.ok && !r.block.empty() && r.ih == 1) {
                rows[i][j] = r.block;  // <img> fallback text (alt/filename): reflow as text
            } else {
                imgText[i][j].clear();  // not an image after all; drop the kept tag
                rows[i][j] = renderInline(rows[i][j]);
            }
        }
    }

    // Natural (unwrapped) width of each column = its widest rendered cell. Image cells contribute
    // their recorded width rather than displayWidth (which can't measure a sixel block). A text
    // cell may already hold its own line breaks (from a <br>), so measure its widest line, not the
    // whole string (which would wrongly sum the lines and count the '\n').
    std::vector<int> natural(ncols, 0);
    for (size_t i = 0; i < rows.size(); ++i)
        for (size_t j = 0; j < ncols; ++j) {
            if (isImage[i][j]) {
                natural[j] = std::max(natural[j], imgWidth[j]);
                continue;
            }
            for (auto& ln : splitOnNewlines(rows[i][j]))
                natural[j] = std::max(natural[j], displayWidth(ln));
        }

    // Decide a target width per column with max-min fair sharing of the content budget (which
    // already excludes the two-space separators). A column whose natural width is at most its fair
    // share keeps that natural width; the columns that want more split the leftover budget evenly.
    // We iterate because fixing the columns that fit raises the share available to the rest — so
    // e.g. one narrow column does not let the wide ones starve each other (the earlier
    // left-to-right greedy did).
    std::vector<int> target(ncols, 0);
    std::vector<bool> fixed(ncols, false);
    int remBudget = budget, remCols = static_cast<int>(ncols);
    for (bool changed = true; changed && remCols > 0;) {
        changed = false;
        int fair = remBudget / remCols;
        for (size_t j = 0; j < ncols; ++j) {
            if (fixed[j] || natural[j] > fair) continue;
            target[j] = natural[j];  // fits within its share: keep natural width
            fixed[j] = true;
            remBudget -= natural[j];
            --remCols;
            changed = true;
        }
    }
    if (remCols > 0) {  // over-share columns split the rest evenly
        int base = remBudget / remCols, extra = remBudget % remCols, k = 0;
        for (size_t j = 0; j < ncols; ++j)
            if (!fixed[j]) target[j] = base + (k++ < extra ? 1 : 0);  // remainder to earliest
    }

    // Apply the targets. A text column is reflowed to its target and may end narrower still; an
    // image column takes the smaller of its natural width and the target (it does not wrap), and is
    // re-rendered below if that is narrower than the image it already produced.
    std::vector<int> widths(ncols, 0);
    for (size_t j = 0; j < ncols; ++j) {
        if (imgWidth[j] > 0) {
            widths[j] = std::min(natural[j], target[j]);
        } else {
            int actual = 0;
            for (auto& r : rows) {
                r[j] = reflow(r[j], target[j]);
                for (auto& ln : splitOnNewlines(r[j])) actual = std::max(actual, displayWidth(ln));
            }
            widths[j] = actual < 1 ? (natural[j] == 0 ? 0 : 1) : actual;
        }
    }

    // An image whose painted width exceeds its allocated column (the table could not give it the
    // room its requested height implied) is re-rendered WIDTH-BOUND to exactly that column width,
    // so it never overflows into the next column or past the table. forceWidthBound is required
    // because the image's true (real-cell) footprint can exceed the column even when the
    // nominal-cell estimate does not, so renderImageBlock's own heuristic would otherwise keep it
    // height-bound and too wide. This is the only case that runs timg twice, and only when columns
    // are tight.
    std::vector<int> allocWidth = widths;  // the budgeted width; tightening must not exceed it
    // The forced width-bound re-renders are submitted as a follow-up batch and joined the same way
    // (ADR 0003), again with kKittyIdDefer + row-major id stamping for serial-identical output.
    std::vector<std::vector<CellImg>> rimg(rows.size(), std::vector<CellImg>(ncols));
    std::vector<std::vector<std::future<void>>> rfut(rows.size());
    for (auto& row : rfut) row.resize(ncols);
    for (size_t i = 0; i < rows.size(); ++i) {
        for (size_t j = 0; j < ncols; ++j) {
            if (!isImage[i][j] || allocWidth[j] >= imgCellW[i][j]) continue;
            std::string cellText = imgText[i][j];
            int allocW = allocWidth[j];
            CellImg* r = &rimg[i][j];
            rfut[i][j] = imagePool().submit([cellText, allocW, fileDir, r] {
                r->ok = renderImageBlock(cellText,
                                         allocW,
                                         fileDir,
                                         r->block,
                                         r->iw,
                                         r->ih,
                                         /*forceWidthBound=*/true,
                                         kKittyIdDefer);
            });
        }
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        for (size_t j = 0; j < ncols; ++j) {
            if (!rfut[i][j].valid()) continue;
            rfut[i][j].get();
            CellImg& r = rimg[i][j];
            if (r.ok && isImageBlock(r.block)) {
                rows[i][j] = r.block;  // id left deferred; writer renumbers in document order
                imgRows[i][j] = std::max(1, r.ih);
                imgCellW[i][j] = r.iw;
            }
        }
    }

    // Tighten each image column to the width its image actually paints (aspect-fit and rounding can
    // make it narrower than the allocated cap), so a column hugs its image and the gap between
    // columns stays the intended two spaces rather than padding out to the cap. Never go below the
    // column's widest TEXT cell (a header wider than the image would overflow and misalign later
    // columns) and never ABOVE the budgeted width (which would overflow the table — a re-render can
    // round a cell or two wide).
    for (size_t j = 0; j < ncols; ++j) {
        if (imgWidth[j] <= 0) continue;
        int w = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (isImage[i][j])
                w = std::max(w, imgCellW[i][j]);
            else
                for (auto& ln : splitOnNewlines(rows[i][j])) w = std::max(w, displayWidth(ln));
        }
        if (w > 0) widths[j] = std::min(w, allocWidth[j]);
    }

    // Re-pin each Kitty image's cell footprint (c=/r=) to the FINAL column width. The c=/r= baked
    // in during rendering reflects that render's availWidth cap (the full budget on the first
    // pass), not the width this column ends up with after fair-sharing and tightening — so without
    // this an image can carry a c= wider than its column and overflow into the next cell. We
    // rewrite c to the final width and scale r by the same ratio to keep the aspect ratio the
    // render already chose. (Sixel images carry no c=/r=; kittyRewriteFootprint no-ops on a
    // non-Kitty block, so the guard is just isImage.) imgRows/imgCellW are also corrected so the
    // band reserves the right number of rows.
    for (size_t j = 0; j < ncols; ++j) {
        if (imgWidth[j] <= 0) continue;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (!isImage[i][j] || !isKittyImage(rows[i][j]) || imgCellW[i][j] <= 0) continue;
            int newC = widths[j];
            int newR = std::max(1, (imgRows[i][j] * newC + imgCellW[i][j] / 2) / imgCellW[i][j]);
            rows[i][j] = kittyRewriteFootprint(rows[i][j], newC, newR);
            imgCellW[i][j] = newC;
            imgRows[i][j] = newR;
        }
    }

    int total = 0;
    for (int w : widths) total += w;
    total += 2 * static_cast<int>(ncols - 1);  // two-space column separators
    std::string hline = horizontalLine(total);

    if (std::getenv("MDCAT_DEBUG_TABLE")) {
        std::fprintf(stderr, "mdcat: table W=%d budget=%d total=%d widths=[", W, budget, total);
        for (size_t j = 0; j < ncols; ++j) std::fprintf(stderr, "%s%d", j ? "," : "", widths[j]);
        std::fprintf(stderr, "] imgW=[");
        for (size_t j = 0; j < ncols; ++j) std::fprintf(stderr, "%s%d", j ? "," : "", imgWidth[j]);
        std::fprintf(stderr, "]\n");
    }

    // The 1-based starting column of each cell, accounting for the two-space separators. Used to
    // position image sixels during the overlay pass.
    std::vector<int> colOrigin(ncols, 1);
    for (size_t j = 1; j < ncols; ++j) colOrigin[j] = colOrigin[j - 1] + widths[j - 1] + 2;

    // Render one table row. The row occupies a band as tall as the tallest cell — text line count
    // or image row height, whichever is greater. Text is laid out first, top-aligned, one line at a
    // time across the band; image cells contribute blank padding of their column width during this
    // pass (reserving their space). Then, in a second pass, each image's sixel is painted into its
    // reserved column with relative cursor moves: up to the band top, across to the cell's column,
    // replay (which leaves the cursor at column 1 just below the image), then back down to the band
    // bottom. This keeps every image inside its own cell and keeps row separators below the band.
    auto emitRow = [&](size_t rowIdx, bool bold) {
        const std::vector<std::string>& cells = rows[rowIdx];
        std::vector<std::vector<std::string>> cellLines(ncols);
        int bandH = 1;
        for (size_t j = 0; j < ncols; ++j) {
            if (isImage[rowIdx][j]) {
                bandH = std::max(bandH, imgRows[rowIdx][j]);
            } else {
                cellLines[j] = splitOnNewlines(cells[j]);
                bandH = std::max(bandH, static_cast<int>(cellLines[j].size()));
            }
        }
        // Text pass: bandH lines. Image cells reserve their column width with spaces.
        for (int k = 0; k < bandH; ++k) {
            std::string line;
            for (size_t j = 0; j < ncols; ++j) {
                if (j) line += "  ";
                if (isImage[rowIdx][j]) {
                    line.append(static_cast<size_t>(widths[j]), ' ');
                } else {
                    std::string piece = static_cast<size_t>(k) < cellLines[j].size()
                        ? cellLines[j][k]
                        : std::string();
                    if (bold && !piece.empty()) piece = kBoldOn + piece + kBoldOff;
                    line += padAligned(piece, widths[j], aligns[j]);
                }
            }
            out << line << '\n';
        }
        // Overlay pass: paint each image into its reserved space. The cursor starts at column 1 on
        // the row just below the band; every move is relative to that, so the band's earlier scroll
        // (if any) does not matter. Each sixel is bracketed by DECSC/DECRC (in replaySixel), so it
        // restores the cursor to the band-top position regardless of how the terminal advances
        // after a sixel — then we step deterministically back to the band bottom for the next
        // image.
        for (size_t j = 0; j < ncols; ++j) {
            if (!isImage[rowIdx][j]) continue;
            out << "\033[" << bandH << 'A';          // up to the band top
            out << "\033[" << colOrigin[j] << 'G';   // across to this cell's column
            replaySixel(cells[j], out);              // paint; cursor restored to band top
            out << "\033[" << bandH << 'B' << '\r';  // down to the band bottom, column 1
        }
    };

    for (size_t i = 0; i < rows.size(); ++i) {
        emitRow(i, /*bold=*/i == 0);
        if (i == 0)
            out << hline << '\n';  // header underline
        else if (i + 1 < rows.size())
            out << kLightGray << hline << kReset << '\n';  // light-gray row separator
    }
}

// ---------------------------------------------------------------------------
// Block parser / document driver
// ---------------------------------------------------------------------------

void render(const std::vector<std::string>& lines, std::ostream& out);

// Drop a single trailing '\n' if present. render() terminates every block with one, so the buffered
// output of a recursive render ends in a newline; removing it keeps splitOnNewlines from yielding a
// spurious empty final line (which would draw an extra bare quote rule).
std::string trimTrailingNewline(const std::string& s) {
    if (!s.empty() && s.back() == '\n') return s.substr(0, s.size() - 1);
    return s;
}

// True if `line` opens a block quote: up to three spaces of indentation, then a '>'. (GFM allows
// the block-quote marker to be indented by at most three spaces; four would be a code indent, which
// this renderer does not implement, but checking the bound keeps the detection honest.)
bool isBlockQuote(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ') ++i;
    return i < line.size() && line[i] == '>';
}

// Strip one block-quote marker from a line that isBlockQuote(): drop up to three leading spaces,
// the '>', and one optional space after it. The remainder is the line's content one level in.
std::string stripBlockQuote(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ') ++i;
    if (i < line.size() && line[i] == '>') ++i;  // the marker itself
    if (i < line.size() && line[i] == ' ') ++i;  // one optional space of padding
    return line.substr(i);
}

// A parsed list-item marker. `markerWidth` is the number of leading columns the marker plus its
// trailing spaces occupy on the first line — i.e. the column the item's content begins at, which is
// also the indent every continuation line and nested block must reach to stay in the item.
struct ListMarker {
    bool ordered = false;    // true for "1." / "1)", false for a "-"/"+"/"*" bullet
    char delim = 0;          // bullet char ('-','+','*') or ordered delimiter ('.'/')')
    int start = 0;           // ordered list's starting number (the digits before the delimiter)
    size_t markerWidth = 0;  // columns from line start to the item's content (the content indent)
};

// If `line` begins a list item, fill `m` and return true. A marker is up to three spaces of indent,
// then either a bullet (-, +, *) or an ordered marker (1-9 digits then '.' or ')'), then at least
// one space (or end of line). The trailing run of spaces is folded into markerWidth so the content
// indent is known. An empty item (marker then end of line) gets a content indent of marker + 1, the
// conventional single space. (We don't implement GFM's 4-space-content rule for over-indented item
// bodies; up to a normal run of spaces after the marker is consumed.)
bool parseListMarker(const std::string& line, ListMarker& m) {
    size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ') ++i;
    if (i >= line.size()) return false;
    size_t markerStart = i;
    if (line[i] == '-' || line[i] == '+' || line[i] == '*') {
        m.ordered = false;
        m.delim = line[i];
        ++i;
    } else if (std::isdigit(static_cast<unsigned char>(line[i]))) {
        size_t d = i;
        while (d < line.size() && std::isdigit(static_cast<unsigned char>(line[d]))) ++d;
        if (d - i > 9 || d >= line.size() || (line[d] != '.' && line[d] != ')')) return false;
        m.ordered = true;
        m.start = std::atoi(line.substr(i, d - i).c_str());
        m.delim = line[d];
        i = d + 1;
    } else {
        return false;
    }
    // The marker must be followed by a space (or be the whole line, an empty item).
    if (i < line.size() && line[i] != ' ') return false;
    (void)markerStart;
    size_t afterMarker = i;
    while (i < line.size() && line[i] == ' ') ++i;
    // An empty item, or one indented by many spaces, still uses a single space of content indent.
    if (i >= line.size() || i - afterMarker == 0)
        m.markerWidth = afterMarker + 1;
    else
        m.markerWidth = i;
    return true;
}

// True if `line` begins a list item of any kind.
bool isListItem(const std::string& line) {
    ListMarker m;
    return parseListMarker(line, m);
}

// Two list markers belong to the same list when they are the same type, and for bullets the same
// bullet character, and for ordered lists the same delimiter ('.'/')'). A change of any of these
// (or a blank line followed by a different type) starts a new list. (GFM also treats a change of
// bullet character as a new list; we follow that.)
bool sameListType(const ListMarker& a, const ListMarker& b) {
    if (a.ordered != b.ordered) return false;
    return a.delim == b.delim;
}

// True if the trimmed line is a valid ATX heading marker (1-6 '#' then space or end of line).
bool isAtxHeading(const std::string& t) {
    size_t h = 0;
    while (h < t.size() && t[h] == '#') ++h;
    return h >= 1 && h <= 6 && (h == t.size() || t[h] == ' ');
}

// True if the trimmed line opens or closes a fenced code block (>= 3 backticks or tildes).
bool isCodeFence(const std::string& t) {
    if (t.size() < 3 || (t[0] != '`' && t[0] != '~')) return false;
    size_t r = 0;
    while (r < t.size() && t[r] == t[0]) ++r;
    return r >= 3;
}

// True if `line` is indented enough to be an indented code line: at least four leading columns of
// indentation, counting a leading tab as reaching the four-column stop. A blank line does not count
// (it is handled separately as a possible interior blank). See GFM 4.4.
bool isIndentedCode(const std::string& line) {
    int cols = 0;
    for (char c : line) {
        if (c == ' ')
            ++cols;
        else if (c == '\t')
            cols += 4;
        else
            break;
        if (cols >= 4) return true;
    }
    return false;  // ran out of leading whitespace before reaching column four (or the line is
                   // blank)
}

// Remove the four-column indent that introduces an indented code block, leaving the rest verbatim.
// A leading tab counts as four columns and is consumed whole; otherwise up to four leading spaces
// are dropped. Content beyond the first four columns is preserved exactly.
std::string stripCodeIndent(const std::string& line) {
    size_t i = 0;
    int cols = 0;
    while (i < line.size() && cols < 4) {
        if (line[i] == ' ') {
            ++cols;
            ++i;
        } else if (line[i] == '\t') {
            cols += 4;
            ++i;
        } else
            break;
    }
    return line.substr(i);
}

// True if the trimmed line is a thematic break: >= 3 of the same -, * or _ (spaces allowed).
bool isThematicBreak(const std::string& t) {
    std::string compact;
    for (char c : t)
        if (c != ' ' && c != '\t') compact += c;
    if (compact.size() < 3) return false;
    return compact.find_first_not_of('-') == std::string::npos ||
        compact.find_first_not_of('*') == std::string::npos ||
        compact.find_first_not_of('_') == std::string::npos;
}

// True if line `j` begins a new leaf block, so a paragraph in progress must stop before it.
bool startsBlock(const std::vector<std::string>& lines, size_t j) {
    const std::string t = trim(lines[j]);
    if (t.empty()) return true;
    if (isAtxHeading(t)) return true;
    if (isCodeFence(t)) return true;
    if (isThematicBreak(t)) return true;
    if (isBlockQuote(lines[j])) return true;
    if (isListItem(lines[j])) return true;
    if (lines[j].find('|') != std::string::npos && j + 1 < lines.size() &&
        isTableDelimiterRow(lines[j + 1]))
        return true;  // a GFM table header row
    return false;
}

// Strip up to `n` leading spaces from `line` (fewer if the line has fewer). Used to remove a list
// item's content indent from its continuation lines so the item's body renders at column zero.
std::string stripIndent(const std::string& line, size_t n) {
    size_t i = 0;
    while (i < n && i < line.size() && line[i] == ' ') ++i;
    return line.substr(i);
}

// Count the leading spaces of `line` (its indentation in columns; tabs are not expanded — the
// inputs here use spaces, matching the rest of the parser).
size_t leadingSpaces(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    return i;
}

// One gathered list item: the content lines with the item's content indent already stripped, plus
// whether a blank line appeared anywhere inside it (which, like a blank line between items, makes
// the whole list loose — GFM 5.3).
struct ListItem {
    std::vector<std::string> lines;
    bool hadBlank = false;
};

// If `s` begins with a GFM task-list checkbox — "[ ]", "[x]" or "[X]" followed by a space (or end
// of the content) — set `checked` and strip the checkbox plus its trailing space, returning true.
// The checkbox must be the very start of the item's content (GFM task-list extension).
bool stripTaskCheckbox(std::string& s, bool& checked) {
    if (s.size() < 3 || s[0] != '[' || s[2] != ']') return false;
    char c = s[1];
    if (c == ' ')
        checked = false;
    else if (c == 'x' || c == 'X')
        checked = true;
    else
        return false;
    if (s.size() == 3) {
        s.clear();
        return true;
    }  // checkbox is the whole content
    if (s[3] != ' ') return false;  // must be followed by whitespace
    s.erase(0, 4);
    return true;
}

// Render a gathered list. Each item's content is a full block sequence rendered recursively (one
// container level deeper) into a buffer; the buffer's first line is prefixed with the item marker
// and the rest with equal-width padding so the body hangs under the marker. Bullets use a
// depth-varying glyph (● ○ ▪︎, matching GitHub's disc/circle/square); ordered items keep their
// number with a '.'. An item whose content opens with a task-list checkbox ("[ ]"/"[x]") swaps the
// marker for a ☐/☑ glyph (GFM task-list extension). gListDepth tracks bullet nesting for the glyph;
// gIndent is raised by the marker width so inner content reflows within the narrower column. A
// loose list prints a blank line between items.
void emitList(const std::vector<ListItem>& items,
              const ListMarker& marker,
              bool loose,
              std::ostream& out) {
    static const std::vector<std::string> kBullets = {"●", "○", "▪︎"};
    // Each list level indents its content by four columns (matching most Markdown viewers), so the
    // marker is padded out to at least four columns. A longer ordered marker (e.g. "10. ") keeps
    // its natural width.
    const int kListIndent = 4;
    // The outermost list block is set off from surrounding text by a two-space left lead; nested
    // lists already step in through their parent item's marker, so they add no further lead.
    const std::string lead = (gListNesting == 0) ? "  " : "";
    gIndent += static_cast<int>(lead.size());
    ++gListNesting;
    for (size_t idx = 0; idx < items.size(); ++idx) {
        if (idx && loose) out << '\n';

        // A task-list item: strip the leading checkbox from its content and remember its state so
        // the marker becomes a checkbox glyph below.
        std::vector<std::string> itemLines = items[idx].lines;
        bool isTask = false, checked = false;
        if (!itemLines.empty() && stripTaskCheckbox(itemLines[0], checked)) isTask = true;

        // Build the marker text and the matching blank padding for continuation lines.
        std::string mark;
        if (isTask) {
            mark = (checked ? "☑" : "☐") + std::string(" ");  // ☑ / ☐
        } else if (marker.ordered) {
            mark = std::to_string(marker.start + static_cast<int>(idx)) + marker.delim + ' ';
        } else {
            mark = kBullets[std::min<size_t>(gListDepth, kBullets.size() - 1)] + std::string(" ");
        }
        // Pad the marker out to the four-column indent so the body hangs at the indented column.
        if (displayWidth(mark) < kListIndent)
            mark += std::string(static_cast<size_t>(kListIndent - displayWidth(mark)), ' ');
        int markWidth = displayWidth(mark);
        std::string pad(static_cast<size_t>(markWidth), ' ');

        std::ostringstream buf;
        gIndent += markWidth;
        if (!marker.ordered) ++gListDepth;
        if (!loose) gTightItem = true;  // suppress lead-paragraph/sublist spacing in a tight item
        render(itemLines, buf);
        if (!marker.ordered) --gListDepth;
        gIndent -= markWidth;

        std::vector<std::string> body = splitOnNewlines(trimTrailingNewline(buf.str()));
        for (size_t k = 0; k < body.size(); ++k)
            out << lead << (k == 0 ? mark : pad) << body[k] << '\n';
    }
    --gListNesting;
    gIndent -= static_cast<int>(lead.size());
}

// Render the gathered lines of a block quote (markers already stripped) one container level deeper.
// The contents are a full sequence of blocks, so they go through render() recursively; the rendered
// output is then prefixed line by line with the quote rule. gIndent is raised by the rule's width
// while the contents render, so paragraphs reflow and rules fill within the narrower inner column.
void emitBlockQuote(const std::vector<std::string>& inner, std::ostream& out) {
    const int prefixWidth = 2;  // displayWidth(kQuoteBar): the bar glyph plus one space
    std::ostringstream buf;
    gIndent += prefixWidth;
    render(inner, buf);
    gIndent -= prefixWidth;
    // Prefix every line of the rendered contents with the quote rule. Blank separator lines between
    // inner blocks get the bar too, so the rule is continuous down the whole quote.
    for (const std::string& line : splitOnNewlines(trimTrailingNewline(buf.str())))
        out << kQuoteBar << line << '\n';
}

void render(const std::vector<std::string>& lines, std::ostream& out) {
    size_t n = lines.size();
    bool emitted = false;  // has any block been printed yet?
    // A tight list item suppresses the blank line between its own top-level blocks (its lead
    // paragraph and a nested list). Snapshot the flag and clear it immediately, so this applies
    // only to this render's top level — blocks rendered deeper, and recursive renders, separate
    // normally.
    bool tight = gTightItem;
    gTightItem = false;
    // Print a blank line before every block except the first, so blocks are visually separated
    // without a leading or trailing blank line in the output. A tight item skips that blank.
    auto separate = [&] {
        if (emitted && !tight) out << '\n';
        emitted = true;
    };
    for (size_t i = 0; i < n;) {
        const std::string& line = lines[i];
        std::string t = trim(line);

        if (t.empty()) {
            ++i;
            continue;
        }  // blank lines separate blocks

        // Stream incrementally: flush the output sink before starting the next block, so each block
        // (and any subprocess-rendered image it contains) reaches the terminal as soon as it is
        // ready rather than the whole document being withheld until the slowest image finishes. The
        // previous block is fully written by now and the cursor sits at a line boundary, so a flush
        // here never splits a Kitty APC chunk mid-stream. At the top level `out` is the SlotSink,
        // whose flush commits the finished block as an ordered slot for the writer thread; in
        // recursive renders it is a local ostringstream and the flush is a no-op. A possibly-empty
        // first flush is harmless.
        if (emitted) out.flush();

        // ATX heading: 1-6 '#' followed by a space (or end of line).
        if (isAtxHeading(t)) {
            separate();
            size_t h = 0;
            while (h < t.size() && t[h] == '#') ++h;
            std::string text = trim(t.substr(h));
            // A heading may have an optional closing run of '#'.
            size_t e = text.size();
            while (e > 0 && text[e - 1] == '#') --e;
            if (e < text.size() && (e == 0 || text[e - 1] == ' ')) text = trim(text.substr(0, e));
            emitHeading(static_cast<int>(h), text, out);
            ++i;
            continue;
        }

        // Fenced code block: a run of >= 3 backticks or tildes; closed by a matching fence.
        if (isCodeFence(t)) {
            separate();
            char fence = t[0];
            size_t run = 0;
            while (run < t.size() && t[run] == fence) ++run;
            std::string lang = trim(t.substr(run));  // info string, e.g. "cpp" or "python"
            std::vector<std::string> body;
            size_t j = i + 1;
            for (; j < n; ++j) {
                std::string tj = trim(lines[j]);
                size_t r = 0;
                while (r < tj.size() && tj[r] == fence) ++r;
                if (r >= run && tj.find_first_not_of(fence) == std::string::npos)
                    break;  // closing fence
                body.push_back(lines[j]);
            }
            // A ```mermaid block renders as a diagram via the mermaid CLI when it (and a graphics
            // terminal) are available; otherwise it falls through to being shown as ordinary code.
            std::string mlang = lang;
            for (char& c : mlang)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            int mAvailW = terminalWidth();
            if (mlang == "mermaid") {
                // The conversion produces the final bytes: the framed diagram on success, else the
                // ordinary code block (the fallback). Computing both in the worker lets the whole
                // block be a single deferred slot (ADR 0003). emitCodeBlock is pure here (top-level
                // gIndent==0, terminalWidth memoized), so it is safe to run off-thread into a local
                // buffer.
                auto convert = [body, lang, mAvailW](uint32_t kid) {
                    std::string mimg;
                    int miw = 0, mih = 0;
                    if (renderMermaidBlock(body, mAvailW, mimg, miw, mih, kid) &&
                        isImageBlock(mimg))
                        return imageParagraphBytes(mimg, mih);
                    std::ostringstream cb;
                    emitCodeBlock(body, lang, cb);
                    return cb.str();
                };
                if (SlotSink* sink = asSlotSink(out)) {
                    // Defer the Kitty id (kKittyIdDefer): the writer assigns a document-order id
                    // when it drains the slot, so out-of-order completion stays serial-identical.
                    slotDeferImage(
                        sink, imagePool().submit([convert] { return convert(kKittyIdDefer); }));
                } else {
                    out << imagePool().submit([convert] { return convert(0); }).get();
                }
            } else {
                emitCodeBlock(body, lang, out);
            }
            i = (j < n) ? j + 1 : j;  // skip the closing fence if present
            continue;
        }

        // Thematic break: a line of three or more -, * or _ (optionally spaced).
        if (isThematicBreak(t)) {
            separate();
            emitThematicBreak(out);
            ++i;
            continue;
        }

        // Block quote: one or more lines starting with '>'. Gather the run, including lazy
        // continuation lines — a non-blank line without its own '>' that continues a paragraph
        // inside the quote (GFM 5.1). A blank line, or an unquoted line that itself starts a new
        // block, ends the quote. The markers are stripped and the contents rendered recursively.
        if (isBlockQuote(line)) {
            separate();
            std::vector<std::string> inner;
            size_t j = i;
            for (; j < n; ++j) {
                if (isBlockQuote(lines[j])) {
                    inner.push_back(stripBlockQuote(lines[j]));
                } else if (!trim(lines[j]).empty() && !startsBlock(lines, j)) {
                    inner.push_back(lines[j]);  // lazy paragraph continuation
                } else {
                    break;
                }
            }
            emitBlockQuote(inner, out);
            i = j;
            continue;
        }

        // List: a run of items sharing a marker type (GFM 5.3). Reached only after the
        // thematic-break check above, so a line like "- - -" or "***" is a rule, not a one-item
        // list. Each item is gathered as the content lines belonging to it (its content indent
        // stripped); a line at the list's own indent that opens a same-type marker starts the next
        // item; one that opens a different-type marker, a non-indented block start, or follows a
        // blank line at a lower indent ends the list. The list is loose if any blank line falls
        // between items or inside an item.
        if (isListItem(line)) {
            separate();
            ListMarker listMarker;
            parseListMarker(line, listMarker);  // defines the list's type for the whole run

            std::vector<ListItem> items;
            bool loose = false;
            size_t j = i;
            while (j < n) {
                ListMarker m;
                // A new item: a marker line at this list's indent level and matching type.
                if (parseListMarker(lines[j], m) &&
                    leadingSpaces(lines[j]) < listMarker.markerWidth) {
                    if (!sameListType(m, listMarker))
                        break;  // different marker type: a separate list
                    ListItem item;
                    // The first line keeps everything after the marker (drop the marker itself, not
                    // just leading spaces — there are none before it at this point).
                    item.lines.push_back(lines[j].substr(std::min(m.markerWidth, lines[j].size())));
                    size_t k = j + 1;
                    bool pendingBlank =
                        false;  // a blank line seen but not yet committed to the item
                    for (; k < n; ++k) {
                        if (trim(lines[k]).empty()) {
                            pendingBlank = true;
                            continue;
                        }
                        size_t ind = leadingSpaces(lines[k]);
                        if (ind >= m.markerWidth) {
                            // Indented to (or past) the content column: part of this item's body. A
                            // committed blank line before it is preserved and makes the list loose.
                            if (pendingBlank) {
                                item.hadBlank = true;
                                item.lines.push_back("");
                                pendingBlank = false;
                            }
                            item.lines.push_back(stripIndent(lines[k], m.markerWidth));
                        } else if (!pendingBlank && !startsBlock(lines, k)) {
                            // Lazy continuation: an unindented paragraph line directly following
                            // the item's text (no blank between) still belongs to its paragraph.
                            item.lines.push_back(lines[k]);
                        } else {
                            break;  // unindented block start, marker, or post-blank line: item
                                    // ends.
                        }
                    }
                    if (item.hadBlank) loose = true;
                    // A blank line separating this item from a following same-type item at the
                    // list's own indent makes the whole list loose (GFM 5.3).
                    if (pendingBlank && k < n) {
                        ListMarker next;
                        if (parseListMarker(lines[k], next) &&
                            leadingSpaces(lines[k]) < listMarker.markerWidth &&
                            sameListType(next, listMarker))
                            loose = true;
                    }
                    items.push_back(std::move(item));
                    j = k;
                } else {
                    break;
                }
            }
            emitList(items, listMarker, loose, out);
            i = j;
            continue;
        }

        // GFM table: a header row whose next line is a delimiter row. Gather contiguous rows.
        if (line.find('|') != std::string::npos && i + 1 < n && isTableDelimiterRow(lines[i + 1])) {
            separate();
            std::vector<std::string> rows;
            rows.push_back(line);          // header
            rows.push_back(lines[i + 1]);  // delimiter
            size_t j = i + 2;
            for (; j < n; ++j) {
                if (trim(lines[j]).empty() || lines[j].find('|') == std::string::npos) break;
                rows.push_back(lines[j]);
            }
            emitTable(rows, out);
            i = j;
            continue;
        }

        // Indented code block: lines indented four or more columns, rendered verbatim (GFM 4.4).
        // Reached only at a fresh block position — an indented line right after a paragraph line is
        // gathered as that paragraph's lazy continuation instead, because isIndentedCode is
        // deliberately NOT part of startsBlock(), so a code block cannot interrupt a paragraph.
        // Interior blank lines are kept; trailing blank lines are dropped. The four-column indent
        // is stripped from each line and the rest emitted exactly, reusing the fenced-code panel
        // style.
        if (isIndentedCode(line)) {
            separate();
            std::vector<std::string> body;
            size_t j = i;
            for (; j < n; ++j) {
                if (trim(lines[j]).empty()) {
                    body.push_back(std::string());
                    continue;
                }
                if (!isIndentedCode(lines[j])) break;
                body.push_back(stripCodeIndent(lines[j]));
            }
            while (!body.empty() && body.back().empty()) body.pop_back();  // drop trailing blanks
            emitCodeBlock(body, "", out);
            // Resume right after the lines that became code; any trailing blank lines we gathered
            // but popped are left for the main loop, which treats blank lines as block separators.
            i += body.size();
            continue;
        }

        // Paragraph: line i is not a block start, so consume it and any following lines until a
        // blank line or the start of another block. The same startsBlock() predicate that the
        // dispatcher relies on is reused here, so the two can never disagree (which previously
        // produced an empty paragraph and an infinite loop on lines like "#620](...)").
        separate();
        std::vector<std::string> para;
        size_t j = i;
        do {
            para.push_back(lines[j]);
            ++j;
        } while (j < n && !startsBlock(lines, j));
        emitParagraph(para, out);
        i = j;
    }
}

std::vector<std::string> splitLines(std::istream& in) {
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // tolerate CRLF
        lines.push_back(line);
    }
    return lines;
}

// An ordered slot list with a dedicated writer thread (ADR 0003), used as the non-tty output sink.
// render() writes text into it exactly like an ostream; at each block boundary (sync()) the
// accumulated text is committed as one ordered slot. A deferred image instead reserves a *future*
// slot (deferImage) and returns immediately, so the render thread keeps converting later images
// while this one is still in flight.
//
// A single block's bytes still reach the wire contiguously — a text slot is written in one
// writeAll, and an image's framed bytes arrive as one slot — so a Kitty APC chunk is never split by
// a buffer-boundary write (a consumer that sees a chunk arrive in pieces with a gap can give up and
// dump the partial base64 as text).
//
// The writer thread walks slots strictly front-to-back: for a ready slot it writes the bytes; for a
// future slot it blocks on the future, then writes. Because the writer is its own thread, a stalled
// write() blocks only the writer — image workers keep filling later future slots. Output emerges in
// document order (the writer never reorders), so the bytes are identical to the serial renderer's
// output; only timing changes.
//
// All Kitty image ids are assigned HERE, on the writer thread, in slot (= document) order: workers
// and tables render with kKittyIdDefer (leaving timg's default id), and writerLoop runs
// kittyRenumberAll on every slot's bytes. That is the one place ids are minted, so parallel,
// out-of-order conversion still yields the exact id sequence the single-threaded serial renderer
// produced. (Text slots contain no APC, so renumbering them is a cheap no-op.)
class SlotSink : public std::streambuf {
public:
    // maxPending bounds the lookahead window: the producer (render thread) blocks before adding a
    // slot once this many slots are queued-but-unwritten, so a near-bottom giant image can't make
    // workers buffer the whole rest of the document in RAM ahead of a stalled writer (ADR 0003
    // backpressure). 2*N (pool size) keeps every worker busy without unbounded buffering.
    explicit SlotSink(int fd)
        : fd_(fd),
          maxPending_(2 * std::max(1u, std::thread::hardware_concurrency())),
          writer_([this] { writerLoop(); }) {}
    // Buffer mode (fd_ == -1): the writer appends each drained slot to buffer_ instead of
    // write()ing to a descriptor. The pager path (ADR 0003 step 6) uses this to get parallel image
    // conversion via the deferred-slot machinery while still assembling the whole document into one
    // buffer for gmore. No backpressure cap is needed — there is no stalling consumer, the buffer
    // is the consumer — so the window is left unbounded so the producer never blocks.
    SlotSink()
        : fd_(-1),
          maxPending_(std::numeric_limits<size_t>::max()),
          writer_([this] { writerLoop(); }) {}
    ~SlotSink() override { finish(); }

    // Buffer-mode result: the fully assembled, Kitty-renumbered document bytes. Call after
    // finish().
    std::string takeBuffer() { return std::move(buffer_); }

    // Commit any buffered text, then append `fut` as the next ordered slot. Returns without waiting
    // on the conversion, but may block briefly for backpressure if the lookahead window is full.
    void deferImage(std::future<std::string> fut) {
        commitText();
        appendSlot(Slot(std::move(fut)));
    }

    // Flush any trailing text and join the writer thread. Idempotent.
    void finish() {
        if (done_) return;
        commitText();
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
        }
        cv_.notify_one();
        if (writer_.joinable()) writer_.join();
        done_ = true;
    }

protected:
    int_type overflow(int_type ch) override {
        if (ch != traits_type::eof()) text_.push_back(static_cast<char>(ch));
        return ch;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        text_.append(s, static_cast<size_t>(n));
        return n;
    }
    // render() flushes at every top-level block boundary; commit the accumulated text as one slot
    // so it is ordered ahead of whatever the next block produces (text or a deferred image).
    int sync() override {
        commitText();
        return 0;
    }

private:
    struct Slot {
        bool ready;
        std::string bytes;             // valid when ready
        std::future<std::string> fut;  // valid when !ready
        explicit Slot(std::string b) : ready(true), bytes(std::move(b)) {}
        explicit Slot(std::future<std::string> f) : ready(false), fut(std::move(f)) {}
    };

    void commitText() {
        if (text_.empty()) return;
        appendSlot(Slot(std::move(text_)));
        text_.clear();
    }

    // Append a slot, blocking first if the unwritten-slot window is full so the producer can't race
    // far ahead of the writer (ADR 0003 backpressure). The writer signals drained_ each time it
    // pops a slot.
    void appendSlot(Slot&& s) {
        std::unique_lock<std::mutex> lk(mu_);
        drained_.wait(lk, [this] { return slots_.size() < maxPending_; });
        slots_.emplace_back(std::move(s));
        lk.unlock();
        cv_.notify_one();
    }

    void writeAll(const std::string& b) {
        size_t off = 0;
        while (off < b.size()) {
            ssize_t n = write(fd_, b.data() + off, b.size() - off);
            if (n <= 0) {
                if (errno == EINTR) continue;
                return;
            }
            off += static_cast<size_t>(n);
        }
    }

    void writerLoop() {
        for (;;) {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return closed_ || !slots_.empty(); });
            if (slots_.empty()) {
                if (closed_) return;
                continue;
            }
            // Move the front slot out; release the lock before any blocking write/get so the
            // producer can keep appending and image workers can keep filling later future slots.
            Slot front = std::move(slots_.front());
            slots_.pop_front();
            lk.unlock();
            drained_.notify_one();  // a slot freed: a producer blocked on backpressure may proceed
            // Assign Kitty ids in slot order (document order): every image in this slot's bytes
            // gets the next id. Text slots have no APC, so this is a no-op for them.
            std::string bytes = front.ready ? std::move(front.bytes) : front.fut.get();
            std::string renumbered = kittyRenumberAll(bytes);
            if (fd_ < 0)
                buffer_ += renumbered;  // buffer mode: assemble in RAM for the pager
            else
                writeAll(renumbered);
        }
    }

    int fd_;              // output descriptor, or -1 in buffer mode
    std::string buffer_;  // buffer mode (fd_ < 0): assembled document, read via takeBuffer()
    size_t maxPending_;   // backpressure cap on unwritten slots
    std::string text_;    // current (render-thread) text buffer, pre-commit
    std::deque<Slot> slots_;
    std::mutex mu_;
    std::condition_variable cv_;       // writer waits for work
    std::condition_variable drained_;  // producer waits for room (backpressure)
    bool closed_ = false;
    bool done_ = false;
    std::thread writer_;
};

SlotSink* asSlotSink(std::ostream& out) {
    return dynamic_cast<SlotSink*>(out.rdbuf());
}
void slotDeferImage(SlotSink* sink, std::future<std::string> fut) {
    sink->deferImage(std::move(fut));
}

}  // namespace

int main(int argc, char** argv) {
    // Collect the file operands, parsing options first. Supported: --width N / -w N / --width=N to
    // force the render width (overriding $COLUMNS and the terminal size), and -- to end options so
    // a filename may begin with a dash. Option parsing stops at the first non-option operand.
    std::vector<std::string> files;
    auto usage = [&] {
        std::cerr << "usage: mdcat [--width N] [--img[=kitty|sixel|none]] [--] [file ...]\n";
    };
    // Map a --img protocol argument to a backend, or return -2 for an unrecognized value.
    auto parseBackend = [](const std::string& s) -> int {
        if (s == "kitty") return static_cast<int>(GraphicsBackend::Kitty);
        if (s == "sixel") return static_cast<int>(GraphicsBackend::Sixel);
        if (s == "none") return static_cast<int>(GraphicsBackend::None);
        return -2;
    };

    int a = 1;
    for (; a < argc; ++a) {
        std::string arg = argv[a];
        if (arg == "--") {
            ++a;
            break;
        }
        if (arg.size() < 2 || arg[0] != '-') break;  // first operand: stop option parsing

        if (arg == "--img") {  // force image output even when piped
            gForceGraphics = true;
            // Optional protocol argument: only consume the next token if it names a backend,
            // so `--img file.md` still treats file.md as the operand.
            if (a + 1 < argc) {
                int b = parseBackend(argv[a + 1]);
                if (b >= 0) {
                    gForcedBackend = b;
                    ++a;
                }
            }
            continue;
        }
        if (arg.rfind("--img=", 0) == 0) {  // inline protocol: --img=kitty|sixel|none
            int b = parseBackend(arg.substr(6));
            if (b < 0) {
                std::cerr << "mdcat: invalid --img protocol: " << arg.substr(6) << "\n";
                usage();
                return 2;
            }
            gForceGraphics = true;
            gForcedBackend = b;
            continue;
        }

        std::string val;
        bool haveVal = false;
        if (arg == "-w" || arg == "--width") {  // value is the next argument
            if (a + 1 >= argc) {
                std::cerr << "mdcat: " << arg << " requires a value\n";
                return 2;
            }
            val = argv[++a];
            haveVal = true;
        } else if (arg.rfind("--width=", 0) == 0) {  // value is inline after '='
            val = arg.substr(8);
            haveVal = true;
        } else {
            std::cerr << "mdcat: unknown option: " << arg << "\n";
            usage();
            return 2;
        }
        if (haveVal) {
            int w = std::atoi(val.c_str());
            if (w <= 0) {
                std::cerr << "mdcat: invalid width: " << val << "\n";
                return 2;
            }
            gWidthOverride = w;
        }
    }
    for (; a < argc; ++a) files.emplace_back(argv[a]);

    auto renderAll = [&](std::ostream& out) -> int {
        if (!files.empty()) {
            for (const auto& path : files) {
                std::ifstream f(path);
                if (!f) {
                    std::cerr << "mdcat: " << path << ": cannot open file\n";
                    return 1;
                }
                size_t slash = path.rfind('/');
                gFileDir = (slash != std::string::npos) ? path.substr(0, slash) : std::string();
                render(splitLines(f), out);
            }
            gFileDir.clear();
        } else {
            render(splitLines(std::cin), out);
        }
        return 0;
    };

    // Eagerly initialize terminal-query state before rendering starts. This avoids deferred
    // probe traffic while the pager is active and keeps worker threads off terminal I/O.
    (void)graphicsBackend();
    (void)cellMetrics();
    (void)queryBackgroundColor();
    initTheme();

    // On a TTY, page through gmore by default. The render thread streams bytes into a pipe while
    // gmore parses from the read end and can paint the first page before the full document is
    // ready.
    bool usePager = isatty(STDOUT_FILENO);
    if (usePager) {
        int pfd[2];
        if (pipe(pfd) != 0) {
            std::perror("mdcat: pipe");
            return 1;
        }
        pid_t pid = fork();
        if (pid < 0) {
            std::perror("mdcat: fork");
            close(pfd[0]);
            close(pfd[1]);
            return 1;
        }
        if (pid == 0) {
            close(pfd[0]);
            SlotSink sink(pfd[1]);
            std::ostream out(&sink);
            int rc = renderAll(out);
            out.flush();
            sink.finish();
            close(pfd[1]);
            _exit(rc);
        }

        close(pfd[1]);
        int pagerRc = gmore::run(std::string(), false, false, false, false, pfd[0]);
        close(pfd[0]);

        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
            std::perror("mdcat: waitpid");
            return 1;
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return pagerRc;
    }

    // Non-tty: stream blocks out as they are rendered. Each file is rendered independently and the
    // results concatenated — the last block of one file can never merge with the first of the next
    // — which makes rendering distribute over the argument list (`mdcat a b` == `mdcat a` then
    // `mdcat b`), as checked by tests/property-concat.sh.
    // SlotSink reserves an ordered slot per block and drains them on a dedicated writer thread, so
    // a deferred image (standalone <img>, mermaid) returns immediately while its conversion runs on
    // the pool and a stalled write() blocks only the writer (ADR 0003). Output stays in document
    // order and byte-identical to the old serial StreamingSink; only timing changes.
    SlotSink sink(STDOUT_FILENO);
    std::ostream out(&sink);
    if (renderAll(out) != 0) return 1;
    out.flush();    // commit the final block's text as a slot
    sink.finish();  // flush trailing text, drain all slots, join the writer thread
    return 0;
}
