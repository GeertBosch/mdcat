#include "mdcat_terminal.h"

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "highlight.h"
#include "mdcat_state.h"
#include "mdcat_style.h"
#include "width_probe.h"

namespace mdcat {

int fullTerminalWidth() {
    // A resize-driven override wins and is NOT cached (it changes across re-renders).
    if (gRenderWidth > 0) return gRenderWidth;
    // Otherwise determined once and cached for the rest of the run: the width is fixed for our
    // purposes and there is no reason to repeat the syscall for every block and reflowed line.
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

int terminalWidth() {
    return std::max(1, fullTerminalWidth() - gIndent);
}

namespace {

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
    struct termios saved = {};
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
    ttyguard::enter(fd, saved);
    // A terminal that does not parse APC prints part of the query as text (Terminal.app showed
    // "a=q,f=24,s=1,v=1;AAAA"). Note where the cursor is before and after, and erase whatever moved
    // it. A terminal that answers no cursor report just skips the cleanup.
    int row0 = 0, col0 = 0;
    bool havePos = write(fd, "\033[6n", 4) == 4 && uw::readCursorPosition(fd, row0, col0);
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
    int row1 = 0, col1 = 0;
    if (havePos && write(fd, "\033[6n", 4) == 4 && uw::readCursorPosition(fd, row1, col1) &&
        (row1 != row0 || col1 != col0)) {
        std::string erase = "\033[" + std::to_string(row0) + ';' + std::to_string(col0) + "H\033[J";
        ssize_t ignored = write(fd, erase.data(), erase.size());
        (void)ignored;
    }
    tcsetattr(fd, TCSANOW, &saved);
    ttyguard::leave();
    close(fd);
    return result;
}

}  // namespace

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

bool terminalSupportsGraphics() {
    return graphicsBackend() != GraphicsBackend::None;
}

bool mmdcAvailable() {
    static const bool available = [] {
        if (!terminalSupportsGraphics()) return false;
        return std::system("command -v mmdc >/dev/null 2>&1") == 0;
    }();
    return available;
}

namespace {

struct TwoInts {
    int a;
    int b;
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
TwoInts queryWindowOp(const char* op) {
    int fd = open("/dev/tty", O_RDWR | O_NOCTTY);
    if (fd < 0) return {0, 0};
    struct termios saved = {};
    if (tcgetattr(fd, &saved) != 0) {
        close(fd);
        return {0, 0};
    }
    struct termios raw = saved;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 2;  // 0.2s between bytes before giving up
    tcsetattr(fd, TCSANOW, &raw);
    ttyguard::enter(fd, saved);
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
    ttyguard::leave();
    close(fd);
    return out;
}

}  // namespace

std::string queryBackgroundColor() {
    static const std::string color = []() -> std::string {
        if (const char* env = std::getenv("MDCAT_BG"))  // explicit override / testability
            return env[0] ? std::string(env) : std::string();
        int fd = open("/dev/tty", O_RDWR | O_NOCTTY);
        if (fd < 0) return std::string();
        struct termios saved = {};
        if (tcgetattr(fd, &saved) != 0) {
            close(fd);
            return std::string();
        }
        struct termios raw = saved;
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 2;  // 0.2s between bytes
        tcsetattr(fd, TCSANOW, &raw);
        ttyguard::enter(fd, saved);
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
        ttyguard::leave();
        close(fd);
        return out;
    }();
    return color;
}

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

void initTheme() {
    if (darkBackground()) {
        kCodeOn = "\033[48;5;236;38;5;252m";  // dark-gray bg, light-gray fg
        kLightGray = "\033[38;5;240m";        // a dimmer separator that reads on a dark background
        // kMathGroupOn is theme-independent (mid-gray 245): legible on both backgrounds, and robust
        // when OSC 11 theme detection is unavailable. See its declaration.
        setHighlightTheme(true);
    } else {
        setHighlightTheme(false);  // kCodeOn/kLightGray keep their light-theme defaults
    }
}

namespace {

// Read a positive integer from an environment variable, or 0 if unset/non-positive.
int envInt(const char* name) {
    if (const char* v = std::getenv(name)) {
        int n = std::atoi(v);
        if (n > 0) return n;
    }
    return 0;
}

}  // namespace

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

bool isITerm() {
    const char* tp = std::getenv("TERM_PROGRAM");
    const char* lc = std::getenv("LC_TERMINAL");
    return (tp && std::string(tp) == "iTerm.app") || (lc && std::string(lc) == "iTerm2");
}

int imageXOffset(int cellW) {
    if (const char* v = std::getenv("MDCAT_IMAGE_X")) return std::max(0, std::atoi(v));
    if (!isITerm()) return 0;
    return std::min(5 * (cellW >= 12 ? 2 : 1), std::max(0, cellW - 1));
}

// Mutable theme colours (declared in mdcat_style.h); initTheme overrides them for a dark theme.
std::string kCodeOn =
    "\033[48;5;254;38;5;236m";  // light-gray bg, dark-gray fg (light-theme default)
std::string kLightGray = "\033[38;5;250m";

}  // namespace mdcat
