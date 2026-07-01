#include "gmore_run.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "gmore_attrs.h"
#include "gmore_emulator.h"

namespace gmore {

struct Search {
    std::string pattern;  // last pattern (empty = no search yet)
    bool forward = true;  // direction of the last `/` (n repeats it, N reverses)
    std::regex re;
    bool valid = false;
    std::string error;  // set when compile fails, for the prompt
    // Position of the CURRENT match (the one n/N step from and the brighter highlight
    // tracks), as a (row, start-column) pair; {-1,-1} = none yet. A column, not just a
    // row, so n/N visit EVERY match — including several on one line. Kept apart from
    // viewTop because a match near the end clamps viewTop below its row.
    long curRow = -1;
    int curCol = -1;

    static bool hasUpper(const std::string& s) {
        for (unsigned char ch : s)
            if (std::isupper(ch)) return true;
        return false;
    }

    // Compile `pat` as the active pattern searched in direction `fwd`. Returns
    // false (and sets error) if the regex is malformed; the prior pattern is kept.
    // The pattern is folded (foldText) before compilation so that math-font
    // variants typed or pasted by the user match the same folded row text.
    bool compile(const std::string& pat, bool fwd) {
        if (pat.empty()) return valid;  // empty /  re-uses the last pattern
        std::string folded = foldText(pat);
        auto flags = std::regex::ECMAScript;
        if (!hasUpper(folded)) flags |= std::regex::icase;
        try {
            re = std::regex(folded, flags);
        } catch (const std::regex_error& e) {
            error = std::string("Invalid regex: ") + e.what();
            return false;
        }
        pattern = pat;
        forward = fwd;
        valid = true;
        error.clear();
        curRow = -1;
        curCol = -1;  // a new pattern searches from the current view
        return true;
    }

    // A located match: its row and the cell column it starts at.
    struct Pos {
        long row;
        int col;
        bool found;
    };

    // The next match from (fromRow, fromCol) scanning in `dir` (+1/-1), considering
    // EVERY match on every row (so several hits on one line are distinct stops), and
    // wrapping around the file like less. `fromCol < 0` (forward) / large (backward)
    // means "from the very start/end of fromRow" — used when seeding from a view top
    // with no prior column. `getSpans(row)` returns that row's match spans (sorted by
    // start column). Returns {found=false} when the pattern matches nowhere.
    template <class GetSpans>
    Pos findPos(long fromRow, int fromCol, int dir, long total, GetSpans getSpans) const {
        if (!valid || total == 0) return {-1, -1, false};
        for (long i = 0; i <= total; ++i) {  // <= total: revisit fromRow last (other cols)
            long r = ((fromRow + dir * i) % total + total) % total;
            auto spans = getSpans((size_t)r);
            if (spans.empty()) continue;
            if (dir > 0) {
                for (auto& s : spans)
                    if (i != 0 || s.first > fromCol) return {r, s.first, true};
            } else {
                for (auto it = spans.rbegin(); it != spans.rend(); ++it)
                    if (i != 0 || it->first < fromCol) return {r, it->first, true};
            }
        }
        return {-1, -1, false};
    }
};

// ---------------------------------------------------------------------------
// terminal raw-mode (restored on exit/signal)
// ---------------------------------------------------------------------------
static int gTtyFd = -1;
static struct termios gSaved;
static bool gRaw = false;
static inline void restoreTty() {
    if (gRaw && gTtyFd >= 0) {
        tcsetattr(gTtyFd, TCSANOW, &gSaved);
        gRaw = false;
    }
}
static inline void onSignal(int sig) {
    restoreTty();
    signal(sig, SIG_DFL);
    raise(sig);
}
static inline bool enterRaw() {
    if (tcgetattr(gTtyFd, &gSaved) != 0) return false;
    struct termios raw = gSaved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(gTtyFd, TCSANOW, &raw) != 0) return false;
    gRaw = true;
    return true;
}

static inline int envInt(const char* name, int def) {
    const char* v = std::getenv(name);
    if (!v) return def;
    int x = std::atoi(v);
    return x > 0 ? x : def;
}

// Terminal rows/cols (and pixel size when the kernel reports it). Tries stdout,
// then stderr/stdin, then /dev/tty — so it still works when stdout is a pipe
// (e.g. `timg … | gmore`), where ioctl on stdout fails.
static inline bool getWinsize(struct winsize& w) {
    for (int fd : {STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO})
        if (ioctl(fd, TIOCGWINSZ, &w) == 0 && w.ws_row > 0) return true;
    int t = open("/dev/tty", O_RDONLY | O_NOCTTY);
    if (t >= 0) {
        bool ok = ioctl(t, TIOCGWINSZ, &w) == 0 && w.ws_row > 0;
        close(t);
        if (ok) return true;
    }
    return false;
}

// Send `CSI <arg> t` to /dev/tty and read the reply (so it works even when stdout
// is piped, e.g. timg | gmore). Raw mode + a short inter-byte timeout keep the
// reply from being echoed/line-buffered and stop us hanging if none arrives. The
// reply is read up to its terminating 't'. Returns "" on no controlling tty / no
// answer. Shared by the cell-size queries below.
static inline std::string queryCsiT(const char* arg) {
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
    raw.c_cc[VTIME] = 2;  // 0.2s inter-byte timeout
    tcsetattr(fd, TCSANOW, &raw);
    std::string query = std::string("\033[") + arg + "t";
    std::string reply;
    if (write(fd, query.data(), query.size()) == static_cast<ssize_t>(query.size())) {
        char c;
        while (reply.size() < 32) {
            ssize_t n = read(fd, &c, 1);
            if (n <= 0) break;
            reply += c;
            if (c == 't') break;
        }
    }
    tcsetattr(fd, TCSANOW, &saved);
    close(fd);
    return reply;
}

// Determine the terminal's character-cell pixel size by querying it. Two methods:
//   CSI 16t -> "ESC[6;H;Wt": cell size directly (VSCode reports it only this way).
//   CSI 14t -> "ESC[4;H;Wt" (text-area px) and CSI 18t -> "ESC[8;R;Ct" (text-area
//     cells): cell = areaPx / areaCells. iTerm2 ignores 16t but answers 14t/18t.
// Needed because some terminals (VSCode, iTerm2) don't report pixels via
// TIOCGWINSZ; without a real cell size gmore's row-reservation and scroll-clip math
// (rows*cellH) would be wrong, scaling with font size — e.g. a 21px cell vs the 16
// default mis-clips a scrolled image by ~5px/row. Returns {0,0} if neither answers.
struct CellSize {
    int w, h;
};
static inline CellSize queryCellSize() {
    // Method A: direct cell size.
    int h = 0, w = 0;
    if (std::sscanf(queryCsiT("16").c_str(), "\033[6;%d;%dt", &h, &w) == 2 && w > 0 && h > 0)
        return {w, h};
    // Method B: derive from text-area px / text-area cells.
    int ah = 0, aw = 0, rows = 0, cols = 0;
    bool gotPx = std::sscanf(queryCsiT("14").c_str(), "\033[4;%d;%dt", &ah, &aw) == 2;
    bool gotCell = std::sscanf(queryCsiT("18").c_str(), "\033[8;%d;%dt", &rows, &cols) == 2;
    if (gotPx && gotCell && aw > 0 && ah > 0 && cols > 0 && rows > 0) return {aw / cols, ah / rows};
    return {0, 0};
}

// ---------------------------------------------------------------------------
// Nav — the pager's navigation state machine, factored out of run()'s I/O so
// the motion commands can be unit-tested without a tty, sixels, or RIS repaints.
// run() owns viewTop's *rendering*; Nav owns how a key (with an optional repeat
// count) moves viewTop within [0, maxTop]. dispatch() returns an action telling
// run() what to do; it never touches stdout. See --nav-trace and the
// tests/gmore-nav.sh harness, which drive Nav through GMORE_KEYS and assert the
// resulting view window in plain text.
// ---------------------------------------------------------------------------
struct Nav {
    size_t viewTop = 0;     // top visible grid row
    int pageH = 1;          // visible rows (screen height minus the prompt line)
    size_t total = 0;       // total content rows
    size_t scrollSize = 0;  // d/u half-screen step; a count to d/u sets it (0 = half page)

    size_t maxTop() const { return total > (size_t)pageH ? total - (size_t)pageH : 0; }
    bool atEnd() const { return viewTop >= maxTop(); }
    // Percent of content scrolled past the bottom of the view, like more(1).
    int percent() const {
        size_t bottom = viewTop + (size_t)pageH;
        return total ? (int)(std::min(bottom, total) * 100 / total) : 100;
    }

    // MESSAGE: don't move; run() shows an informational prompt (e.g. = position).
    // REDRAW: don't move; clear and repaint the current window (^L), to recover
    // from a corrupted screen.
    enum Action { NONE, REPAINT, QUIT, MESSAGE, REDRAW };

    // 1-based line number at the bottom of the view (the last visible line),
    // clamped to total — what more(1)'s "=" reports.
    size_t bottomLine() const { return std::min(viewTop + (size_t)pageH, total); }

    void down(size_t n) { viewTop = std::min(maxTop(), viewTop + n); }
    void up(size_t n) { viewTop = n <= viewTop ? viewTop - n : 0; }
    // Put 1-based line `ln` at the top of the view, clamped to [0, maxTop].
    void gotoLine(size_t ln) { viewTop = std::min(maxTop(), ln ? ln - 1 : 0); }
    // d/u step: the sticky scrollSize if set, else half a page (at least 1).
    size_t scrollStep() const {
        return scrollSize ? scrollSize : (size_t)(pageH > 1 ? pageH / 2 : 1);
    }

    // Apply a command key with repeat count `count` (0 = "not given"; commands
    // pick their own default). Returns the action run() must take. Keeps the
    // more(1) quirk that space/forward at (END) quits (space) or no-ops (others).
    Action dispatch(unsigned char c, long count) {
        if (c == 'q' || c == 'Q') return QUIT;
        long n = count > 0 ? count : 0;
        bool fwd = (c == ' ' || c == 'f' || c == 'j' || c == '\r' || c == '\n');
        if (atEnd() && fwd) return c == ' ' ? QUIT : NONE;
        switch (c) {
        case ' ':
        case 'f': down(n > 0 ? (size_t)n : (size_t)pageH); return REPAINT;
        case 'b': up(n > 0 ? (size_t)n : (size_t)pageH); return REPAINT;
        case '\r':
        case '\n':
        case 'j': down(n > 0 ? (size_t)n : 1); return REPAINT;
        case 'k':
        case 'y': up(n > 0 ? (size_t)n : 1); return REPAINT;
        // g/G: go to line N (1-based); default g=first line, G=last line.
        case 'g': gotoLine(n > 0 ? (size_t)n : 1); return REPAINT;
        case 'G': gotoLine(n > 0 ? (size_t)n : total); return REPAINT;
        // d/^D, u/^U: scroll half a screen; a count sets the step and sticks,
        // like more(1). Default step is half the page height (min 1).
        case 'd':
        case 0x04:
            if (n > 0) scrollSize = (size_t)n;
            down(scrollStep());
            return REPAINT;
        case 'u':
        case 0x15:
            if (n > 0) scrollSize = (size_t)n;
            up(scrollStep());
            return REPAINT;
        // =/^G: report position (line number + percent) without moving.
        case '=':
        case 0x07: return MESSAGE;
        // ^L: clear and repaint the current screen without moving.
        case 0x0C: return REDRAW;
        default: return NONE;
        }
    }
};

// ---------------------------------------------------------------------------
// run() — the gmore pager entry point. Accepts already-read input data, or (when
// streamFd >= 0) incrementally reads and parses bytes from that fd.
// Returns 0 on success. When stdout is not a tty and neither dump nor imginfo
// mode is requested, passes the data through verbatim (pager-as-cat).
// ---------------------------------------------------------------------------
int run(std::string data, bool dump, bool dumpImages, bool imginfo, bool navTrace, int streamFd) {
    internAttr(Attr{});  // id 0 = default

    // Terminal geometry. Env wins (for --dump/tests); then the kernel's TIOCGWINSZ
    // (rows/cols, and pixel size if present); then CSI 16t for the cell pixel size
    // (VSCode reports it only this way); then defaults. Size is read from /dev/tty/
    // stderr too, so it's correct even when stdout is a pipe (timg | gmore).
    int H = envInt("LINES", 0), W = envInt("COLUMNS", 0);
    int cellW = envInt("GMORE_CELLW", 0), cellH = envInt("GMORE_CELLH", 0);
    struct winsize ws;
    if (getWinsize(ws)) {
        if (!H) H = ws.ws_row;
        if (!W) W = ws.ws_col;
        if (!cellW && ws.ws_xpixel > 0 && ws.ws_col > 0) cellW = ws.ws_xpixel / ws.ws_col;
        if (!cellH && ws.ws_ypixel > 0 && ws.ws_row > 0) cellH = ws.ws_ypixel / ws.ws_row;
    }
    if (!cellW || !cellH) {
        CellSize cs = queryCellSize();
        if (cs.w > 0 && cs.h > 0) {
            if (!cellW) cellW = cs.w;
            if (!cellH) cellH = cs.h;
        }
    }
    if (!H) H = 24;
    if (!W) W = 80;
    if (!cellW) cellW = 8;
    if (!cellH) cellH = 16;
    if (std::getenv("GMORE_DEBUG"))
        std::fprintf(stderr, "[gmore] geometry W=%d H=%d cell=%dx%d\n", W, H, cellW, cellH);

    // Not a terminal and not --dump/--imginfo: pass through verbatim (pager-as-cat).
    // GMORE_KEYS forces the pager path even off-tty so a scripted session can be
    // captured to a file for inspection.
    if (!isatty(STDOUT_FILENO) && !dump && !imginfo && !std::getenv("GMORE_KEYS")) {
        fwrite(data.data(), 1, data.size(), stdout);
        return 0;
    }

    Emulator em(W, H, cellW, cellH);
    bool inputEof = true;
    auto feedOneChunk = [&]() {
        char buf[65536];
        for (;;) {
            ssize_t n = read(streamFd, buf, sizeof buf);
            if (n == 0) return true;  // EOF
            if (n < 0) {
                if (errno == EINTR) continue;
                return true;  // treat read error as EOF
            }
            em.feed(buf, (size_t)n);
            return false;
        }
    };
    auto feedUntilRows = [&](size_t needRows) {
        while (em.contentRows() < needRows) {
            if (feedOneChunk()) return true;
        }
        return false;
    };
    auto feedAll = [&]() {
        while (!feedOneChunk()) {
        }
        return true;
    };
    if (streamFd >= 0) {
        inputEof = feedUntilRows((size_t)std::max(1, H - 1));
    } else {
        em.feed(data.data(), data.size());
        inputEof = true;
    }
    size_t total = em.contentRows();

    // Non-interactive/reporting modes expect a complete input model.
    if (streamFd >= 0 && !inputEof && (imginfo || dump || navTrace || std::getenv("GMORE_KEYS"))) {
        inputEof = feedAll();
        total = em.contentRows();
    }

    if (imginfo) {
        for (size_t k = 0; k < em.images().size(); ++k) {
            const Image& I = em.images()[k];
            int cols = I.footCols > 0 ? I.footCols : (I.Ph + cellW - 1) / cellW;
            int rws = I.heightCells(cellH);
            std::printf("image %zu @%zu,%d %dx%dpx %dx%dcells\n",
                        k + 1,
                        I.row,
                        I.col,
                        I.Ph,
                        I.Pv,
                        cols,
                        rws);
            // ASCII raster only for sixels: Kitty images keep no decoded px raster
            // (they're transmitted verbatim), so I.px is empty — indexing it would crash.
            if (!I.isKitty() && I.Ph <= 40 && I.Pv <= 40) {  // small enough to show as ASCII
                for (int y = 0; y < I.Pv; ++y) {
                    std::string line;
                    for (int x = 0; x < I.Ph; ++x) line += I.px[(size_t)y * I.Ph + x] ? '#' : '.';
                    std::printf("%s\n", line.c_str());
                }
            }
        }
        return 0;
    }

    // --dump is the text-grid test surface; suppress images so tests stay deterministic.
    // vTop/vBot give the visible row window so an image paints one sixel clipped to it
    // (vBot==0 => no clip: paint the whole image at its anchor, used by --dump-images).
    auto emitRow = [&](size_t r, size_t vTop = 0, size_t vBot = 0) {
        std::string s;
        em.renderRow(r, s, true, vTop, vBot);
        fwrite(s.data(), 1, s.size(), stdout);
    };
    auto emitRowText = [&](size_t r) {
        std::string s;
        em.renderRow(r, s, false, 0, 0, /*withLinks=*/false);
        fwrite(s.data(), 1, s.size(), stdout);
    };

    if (dump) {
        for (size_t r = 0; r < total; ++r) {
            if (dumpImages)
                emitRow(r);
            else
                emitRowText(r);
            std::fputc('\n', stdout);
        }
        return 0;
    }

    // --nav-trace is the navigation test surface: replay the GMORE_KEYS script
    // through Nav (no tty, no painting) and print the final view window as plain
    // text — "top=R bottom=B total=T pct=P% END?" — so motion commands can be
    // asserted deterministically. Counts are leading digits before a command.
    // Search is driven here too against `em`'s grid: `/`/`?` read a pattern up to
    // the next newline, `n`/`N` repeat it; `notfound`/`badre` is appended when a
    // search fails so those paths are assertable. Mirrors run()'s search handling.
    if (navTrace) {
        Nav t;
        int H2 = H ? H : 24;
        t.pageH = H2 - 1;
        t.total = total;
        Search search;
        const char* note = "";
        auto doSearch = [&](int dir) {
            if (!search.valid) {
                note = " notfound";
                return;
            }
            long fromRow = search.curRow >= 0 ? search.curRow : (long)t.viewTop;
            int fromCol = search.curRow >= 0 ? search.curCol : (dir > 0 ? -1 : INT_MAX);
            Search::Pos p = search.findPos(fromRow, fromCol, dir, (long)total, [&](size_t r) {
                return em.matchSpans(r, search.re);
            });
            if (!p.found) {
                note = " notfound";
                return;
            }
            search.curRow = p.row;
            search.curCol = p.col;
            t.gotoLine((size_t)p.row + 1);
        };
        const char* keys = std::getenv("GMORE_KEYS");
        long count = 0;
        for (const char* p = keys; p && *p; ++p) {
            unsigned char c = (unsigned char)*p;
            if (c == '/' || c == '?') {  // read pattern up to newline
                std::string pat;
                while (*++p && *p != '\n') pat.push_back(*p);
                bool fwd = (c == '/');
                if (search.compile(pat, fwd))
                    doSearch(fwd ? +1 : -1);
                else
                    note = " badre";
                if (!*p) break;  // newline consumed by ++p above
                continue;
            }
            if (c == 'n' || c == 'N') {
                int dir = search.forward ? +1 : -1;
                if (c == 'N') dir = -dir;
                doSearch(dir);
                continue;
            }
            if (c >= '0' && c <= '9') {
                if (count <= (long)total) count = count * 10 + (c - '0');
                continue;
            }
            Nav::Action a = t.dispatch(c, count);
            count = 0;
            if (a == Nav::QUIT) break;
        }
        std::printf("top=%zu bottom=%zu total=%zu pct=%d%% %s%s\n",
                    t.viewTop,
                    std::min(t.viewTop + (size_t)t.pageH, t.total),
                    t.total,
                    t.percent(),
                    t.atEnd() ? "END" : "more",
                    note);
        return 0;
    }

    // Scroll-safety for the first paint. gmore models a clean grid from row 0, but the
    // REAL terminal may start with a non-blank screen and the cursor near the bottom.
    // Painting top-down from there makes each tall sixel force the terminal to scroll to
    // make room — and a scroll mid-row invalidates the DECSC/DECRC anchors, so the
    // second and later images in a table row cascade down a row each (the reported bug;
    // a later page-up/down repaints via RIS and fixes it). The cure is the same trick
    // mdcat's emitImageParagraph uses: print `n` newlines first to reserve the band
    // (this scrolls any existing content into scrollback, guaranteeing room below), then
    // move the cursor back up `n` rows (relative, so the scroll doesn't matter) to the
    // top of the freshly-cleared region, and paint there. No row emission then forces a
    // scroll, so every sixel anchors correctly — without clearing the screen (RIS), which
    // would be wrong for short output that doesn't need paging.
    auto reserveRows = [&](size_t n) {
        if (n == 0) return;
        for (size_t i = 0; i < n; ++i) std::fputc('\n', stdout);
        std::fprintf(stdout, "\033[%zuA", n);
    };

    // Search state (the active pattern + the row of the current match). Declared here,
    // before paintWindow, so paintWindow can highlight matches against search.re.
    Search search;

    // Per-row search-match highlight for `absRow`: the cell-column spans matching the
    // active pattern, with the exact current match (search.curRow/curCol) flagged as
    // the "current" one (more saturated blue). Matching by COLUMN, not just row, so the
    // brighter highlight lands on the precise match n/N is on — even with several
    // matches on one line. Returns an empty Highlight when there is no active search,
    // so non-search rendering is byte-for-byte unchanged.
    auto rowHighlight = [&](size_t absRow) -> Highlight {
        Highlight h;
        if (!search.valid) return h;
        h.spans = em.matchSpans(absRow, search.re);
        if ((long)absRow == search.curRow)
            for (size_t k = 0; k < h.spans.size(); ++k)
                if (h.spans[k].first == search.curCol) {
                    h.current = (int)k;
                    break;
                }
        return h;
    };

    // Emit rows [first, first+count) the safe two-pass way: all text first (committing
    // every line), then images painted into those committed rows (see paintImages).
    // Leaves the cursor on the line just below the window. vBot is the absolute row past
    // the window for bottom-clipping (0 => clip to first+count, the whole window).
    auto paintWindow = [&](size_t first, int count, size_t vBot) {
        std::string s;
        for (int i = 0; i < count; ++i) {
            size_t r = first + (size_t)i;
            if (r < total) {
                Highlight hl = rowHighlight(r);
                em.renderRow(r,
                             s,
                             /*withImages=*/false,
                             0,
                             0,
                             /*withLinks=*/true,
                             hl.empty() ? nullptr : &hl);
            }
            s += '\n';
        }
        em.paintImages(s, first, count, vBot ? vBot : first + (size_t)count);
        fwrite(s.data(), 1, s.size(), stdout);
    };

    // Fits on one screen: print and exit, no prompt. In streaming mode only take this
    // path when input has reached EOF; otherwise keep going so we can paint early and
    // continue ingesting in the background phase below.
    if (inputEof && total <= static_cast<size_t>(H - 1)) {
        reserveRows(total);
        paintWindow(0, (int)total, total);
        return 0;
    }

    // GMORE_KEYS replays a scripted key sequence with no tty (output can be a
    // pipe/file) — skip raw mode entirely in that case.
    const bool scripted = std::getenv("GMORE_KEYS") != nullptr;
    if (!scripted) {
        gTtyFd = open("/dev/tty", O_RDWR);
        if (gTtyFd < 0) gTtyFd = STDIN_FILENO;
        if (!enterRaw()) {
            paintWindow(0, (int)total, total);
            return 0;
        }
        std::atexit(restoreTty);
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
    }

    // view = rows [viewTop, viewTop + pageH); pageH = H-1 (bottom row = prompt).
    // Navigation lives in Nav (see above); viewTop/pageH are references into it so
    // the existing paint code reads unchanged.
    Nav nav;
    nav.pageH = H - 1;
    nav.total = total;
    int& pageH = nav.pageH;
    size_t& viewTop = nav.viewTop;

    // Observability: GMORE_DEBUG logs paint operations to stderr (which absolute
    // grid rows we paint onto which screen lines, and where image strips land), so
    // the scroll logic can be reasoned about without a sixel-capable tty.
    const bool dbg = std::getenv("GMORE_DEBUG") != nullptr;
    auto trace = [&](const char* what) {
        if (!dbg) return;
        std::fprintf(
            stderr, "[gmore] %-10s viewTop=%zu pageH=%d total=%zu\n", what, viewTop, pageH, total);
    };
    // For an image-bearing absolute row, report which screen line it's being
    // painted on and the image's anchor row — a mismatch is the drift bug.
    auto traceRow = [&](size_t absRow, int screenLine) {
        if (!dbg) return;
        for (size_t k = 0; k < em.images().size(); ++k) {
            const Image& I = em.images()[k];
            size_t imgEnd = I.row + (size_t)((I.Pv + cellH - 1) / cellH);
            if (absRow >= I.row && absRow < imgEnd)
                std::fprintf(stderr,
                             "[gmore]   row=%zu -> screenLine=%d  img%zu anchorRow=%zu strip=%zu\n",
                             absRow,
                             screenLine,
                             k,
                             I.row,
                             absRow - I.row);
        }
    };

    // A transient status message (e.g. the = position report) shown on the prompt
    // row in place of --More-- until the next keystroke, then cleared.
    std::string message;
    auto showPrompt = [&] {
        if (!message.empty())
            std::fprintf(stdout, "\033[7m%s\033[27m", message.c_str());
        else if (!inputEof)
            std::fputs("\033[7m--More--\033[27m", stdout);
        else if (nav.atEnd())
            std::fputs("\033[7m(END)\033[27m", stdout);
        else
            std::fprintf(stdout, "\033[7m--More--(%d%%)\033[27m", nav.percent());
        std::fflush(stdout);
    };
    auto clearPrompt = [&] { std::fputs("\r\033[K", stdout); };

    // Help overlay (the `h` command): RIS-clear and list the key bindings, leaving
    // a prompt at the bottom. Any key dismisses it (the caller repaints after).
    auto showHelp = [&] {
        static const char* lines[] = {
            "gmore — key commands",
            "",
            "  space, f       forward one screen (N: N lines)",
            "  b              backward one screen",
            "  Enter, j       forward one line (N: N lines)",
            "  k, y           backward one line",
            "  d, ^D          forward half screen (N sets the step)",
            "  u, ^U          backward half screen",
            "  g / G          go to first / last line (N: line N)",
            "  /pattern       search forward (regex, smart-case)",
            "  ?pattern       search backward",
            "  n / N          repeat search / in reverse",
            "  =, ^G          show position (line / total / %)",
            "  ^L             clear and repaint the screen",
            "  h              this help",
            "  q, Q           quit",
            "",
        };
        std::fputs("\033c", stdout);  // RIS: clear like repaint()
        for (const char* l : lines) std::fprintf(stdout, "%s\r\n", l);
        std::fputs("\033[7mPress any key to continue\033[27m", stdout);
        std::fflush(stdout);
    };

    // Initial paint: fill the first page. paintWindow commits all text rows first, then
    // paints images into them (a tall sixel painted before its rows exist would scroll
    // the terminal mid-paint and clip the image's top — see paintImages).
    size_t initEnd = std::min(total, (size_t)pageH);
    trace("init");
    reserveRows((size_t)pageH);  // make room below so no sixel forces a mid-paint scroll
    for (size_t r = 0; r < initEnd; ++r) traceRow(r, (int)r);
    paintWindow(0, (int)initEnd, initEnd);
    viewTop = initEnd < (size_t)pageH ? 0 : initEnd - (size_t)pageH;

    // Repaint the whole visible window for the current viewTop. Both scroll
    // directions use this: with whole-sixel image painting (one clipped sixel per
    // image, not a per-row strip stack — see renderRow) an incremental scroll can't
    // cleanly relocate a sixel whose top has moved off-screen, so we full-repaint
    // every move. Each row's image (if any) paints once, clipped to [viewTop, vBot).
    //
    // We wipe with a FULL RESET (ESC c / RIS), not ESC[2J. On VSCode's terminal
    // ESC[2J erases text cells but NOT a relocated sixel raster, so scrolled-off
    // images ghosted at the top; ESC[3J, soft reset (ESC[!p), cell-overwrite and an
    // opaque cover sixel all left artifacts. Only RIS fully clears the raster
    // (verified live). gmore relies on no terminal state RIS disturbs — it re-emits
    // every SGR/color per row, uses no scroll region or custom charset, and raw mode
    // is kernel-side (untouched).
    auto repaint = [&] {
        size_t vBot = std::min(total, viewTop + (size_t)pageH);
        std::fputs("\033c", stdout);
        em.forgetKittyTransmissions();  // RIS dropped the terminal's Kitty images; re-send them
        for (int i = 0; i < pageH; ++i) traceRow(viewTop + (size_t)i, i);
        paintWindow(viewTop, pageH, vBot);
    };

    // Forward-only incremental paint that PRESERVES SCROLLBACK. When the view moves
    // strictly down by `delta` rows (a contiguous forward shift), we don't reset the
    // terminal — we just emit the `delta` newly-revealed rows at the bottom. The prompt
    // was already erased (clearPrompt) leaving the cursor at column 0 of the old prompt
    // row, which sits one line below the old window's last text row; emitting rows there
    // scrolls the terminal up naturally, pushing the rows that leave the top of the view
    // into the terminal's scrollback (exactly how more(1)/cat behave). Because no RIS
    // (\033c) is issued, VSCode keeps the scrollback the user wants to keep.
    //
    // The cost vs repaint(): an image whose top has scrolled above the new view can't be
    // re-clipped (paintImages anchors whole sixels), so forward-append is only used while
    // images aren't being split across the top fold — see the call site, which falls back
    // to repaint() for any non-contiguous or backward move. A clean text/forward page is
    // the common case and now leaves history intact.
    auto advance = [&](size_t prevTop) {
        size_t delta = viewTop - prevTop;           // caller guarantees viewTop > prevTop
        size_t firstNew = prevTop + (size_t)pageH;  // first row not previously visible
        size_t vBot = std::min(total, viewTop + (size_t)pageH);
        for (size_t i = 0; i < delta; ++i) traceRow(firstNew + i, pageH - (int)delta + (int)i);
        paintWindow(firstNew, (int)delta, vBot);
    };

    // Paint after the view moved from `prevTop` to the current viewTop, picking the
    // cheapest correct strategy. advance() (scrollback-preserving incremental append)
    // is ONLY valid for a CONTIGUOUS forward move — delta <= pageH. At delta < pageH
    // the new window overlaps the old; at delta == pageH it abuts it (the normal
    // full-page scroll). Either way advance() appends exactly the newly-revealed rows
    // below the screen and the terminal scrolls the rest into scrollback, leaving every
    // on-screen sixel/text cell valid. A backward move, no move, OR a forward JUMP
    // (delta > pageH: g/G, a big count, search to a distant line) leaves a GAP — the old
    // screen has no row in common with the new one, so its sixels would linger (the
    // reported bug). Those must full-repaint via RIS, which wipes the screen.
    auto paintMove = [&](size_t prevTop) {
        if (viewTop > prevTop && viewTop - prevTop <= (size_t)pageH)
            advance(prevTop);
        else if (viewTop != prevTop)
            repaint();
        // viewTop == prevTop: clamped at an edge, nothing moved — leave the screen.
    };

    // GMORE_KEYS scripts the keystroke stream (one char = one key) instead of
    // reading the tty, so a session like "jk" can be replayed non-interactively
    // and its exact output (escapes + sixel strips) captured for inspection.
    // Motion logic lives in Nav::dispatch; this loop only does I/O and accumulates
    // a leading decimal repeat count (more(1)'s "10j", etc.).
    const char* scriptedKeys = std::getenv("GMORE_KEYS");
    size_t keyPos = 0;

    // One keystroke from the scripted stream or the tty; returns false at EOF so
    // both the main loop and the search input editor share one read path.
    auto nextKey = [&](unsigned char& c) -> bool {
        if (scriptedKeys) {
            if (!scriptedKeys[keyPos]) return false;
            c = (unsigned char)scriptedKeys[keyPos++];
            return true;
        }
        return read(gTtyFd, &c, 1) == 1;
    };

    // On streaming input, forward motion from (the current) end needs more rows first.
    auto maybeLoadMoreForForward = [&](unsigned char c) {
        if (streamFd < 0 || inputEof) return;
        bool forwardRequest = (c == ' ' || c == 'f' || c == 'j' || c == '\r' || c == '\n' ||
                               c == 'd' || c == 0x04 || c == 'G');
        bool corpusWideRequest = (c == '/' || c == '?' || c == 'n' || c == 'N' || c == 'G');
        if (!corpusWideRequest && (!forwardRequest || !nav.atEnd())) return;
        // We can paint the first page early, but once the user asks for movement/search beyond the
        // currently available tail, finish ingesting so pagination and multi-image rows are
        // coherent.
        inputEof = feedAll();
        total = em.contentRows();
        nav.total = total;
    };

    // Read a pattern on the prompt row after the leading `/`, echoing as typed.
    // Returns true with `out` set on Enter; false if cancelled (ESC) or EOF.
    // Backspace edits; an empty Enter returns true with out empty (reuse last).
    auto readLine = [&](char lead, std::string& out) -> bool {
        out.clear();
        auto draw = [&] {
            std::fputs("\r\033[K", stdout);
            std::fputc(lead, stdout);
            fwrite(out.data(), 1, out.size(), stdout);
            std::fflush(stdout);
        };
        draw();
        for (;;) {
            unsigned char c;
            if (!nextKey(c)) return false;
            if (c == '\r' || c == '\n') return true;
            if (c == 0x1b) return false;   // ESC cancels
            if (c == 0x7f || c == 0x08) {  // backspace
                if (!out.empty()) {
                    // Drop a whole UTF-8 char (its trailing continuation bytes too).
                    do {
                        out.pop_back();
                    } while (!out.empty() && (out.back() & 0xc0) == 0x80);
                    draw();
                }
                continue;
            }
            if (c >= 0x20 || c >= 0x80) {
                out.push_back((char)c);
                draw();
            }
        }
    };

    // Run a search: advance to the next match in `dir` (+1 forward / -1 backward) and
    // scroll its row to the top of the view. Sets `message` on not-found / no-prior.
    // Returns true if a match was found (the caller repaints — the current-match
    // highlight moves even when the view itself doesn't, e.g. a later match on the
    // same row). Steps from the current match's exact (row,col); see findPos.
    auto runSearch = [&](int dir) -> bool {
        if (!search.valid) {
            message = "No previous search";
            return false;
        }
        // Step from the current match's exact (row,col) so n/N visit every match,
        // including several on one line. With no prior match, seed from viewTop's edge
        // (col -1 forward = before the first; INT_MAX backward = after the last) so the
        // first hit is the nearest match from the current view.
        long fromRow = search.curRow >= 0 ? search.curRow : (long)viewTop;
        int fromCol = search.curRow >= 0 ? search.curCol : (dir > 0 ? -1 : INT_MAX);
        Search::Pos p = search.findPos(fromRow, fromCol, dir, (long)total, [&](size_t r) {
            return em.matchSpans(r, search.re);
        });
        if (!p.found) {
            message = "Pattern not found";
            return false;
        }
        search.curRow = p.row;
        search.curCol = p.col;
        nav.gotoLine((size_t)p.row + 1);
        return true;  // a match was found; caller repaints (the current highlight moved
                      // even when the view itself didn't — e.g. next match on the same row)
    };

    long count = 0;
    bool counting = false;  // mid-count: don't reprint the prompt between digits
    for (;;) {
        if (!counting) showPrompt();
        unsigned char c;
        if (!nextKey(c)) break;
        maybeLoadMoreForForward(c);
        // Search and help are handled here, before Nav: they need the grid text
        // (search) or take over the prompt row (input editor / overlay), which Nav
        // is deliberately ignorant of.
        if (c == '/' || c == '?') {
            std::string pat;
            bool ok = readLine(c, pat);
            clearPrompt();
            message.clear();
            count = 0;
            counting = false;
            if (!ok) continue;  // cancelled — leave the view
            bool fwd = (c == '/');
            if (!search.compile(pat, fwd)) {
                message = search.error;
                continue;
            }
            runSearch(fwd ? +1 : -1);
            // Always full-repaint after a search: the highlight must be applied across
            // the WHOLE visible window (including matches already on screen), which the
            // incremental advance() path can't do — it only paints newly-revealed rows.
            repaint();
            continue;
        }
        if (c == 'n' || c == 'N') {
            clearPrompt();
            message.clear();
            count = 0;
            counting = false;
            // n repeats in the search's direction; N reverses it.
            int dir = search.forward ? +1 : -1;
            if (c == 'N') dir = -dir;
            if (runSearch(dir)) repaint();  // repaint to (re)highlight the visible window
            continue;
        }
        if (c == 'h') {
            clearPrompt();
            showHelp();
            unsigned char k;
            nextKey(k);  // any key dismisses
            repaint();
            count = 0;
            counting = false;
            message.clear();
            continue;
        }
        if (c >= '0' && c <= '9') {
            // Clamp so a long digit run can't overflow; no motion exceeds `total`.
            if (count <= (long)total) count = count * 10 + (c - '0');
            counting = true;
            continue;
        }
        counting = false;
        clearPrompt();
        message.clear();
        size_t prevTop = viewTop;
        Nav::Action a = nav.dispatch(c, count);
        count = 0;
        if (a == Nav::QUIT) break;
        if (a == Nav::REPAINT) {
            trace("dispatch");
            // A contiguous forward step appends the newly-revealed rows below the screen,
            // scrolling old content into scrollback (preserving history). A forward jump
            // (g/G, a big count), any backward move, or a no-op falls back to RIS full
            // repaint, which relocates images correctly but resets the screen. See
            // paintMove for why a non-overlapping forward move can't use advance().
            paintMove(prevTop);
        }
        if (a == Nav::REDRAW) {
            trace("redraw");
            repaint();
        }
        if (a == Nav::MESSAGE) {
            char buf[64];
            std::snprintf(
                buf, sizeof buf, "line %zu/%zu (%d%%)", nav.bottomLine(), nav.total, nav.percent());
            message = buf;
        }
    }
    clearPrompt();
    std::fflush(stdout);
    return 0;
}

}  // namespace gmore
