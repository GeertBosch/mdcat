// gmore — a graphics-aware pager. It emulates a pragmatic subset of a terminal
// into a cell grid (the scrollback buffer), so it can page ANY text+sixel stream
// with no constraints on the producer — including cursor movement. See
// docs/adr/0001-gmore-data-model.md for why a cell grid (not line spans).
//
// STATUS: sixel decode + encode + strip rendering complete. The cell-grid emulator
// handles text, UTF-8, wrap, SGR colour/style, cursor movement, DECSC/DECRC, EL/ED,
// backspace overstrike (man/nroff `X\bX` bold, `_\bX` underline), sixel DCS (decoded
// to RGBA rasters, re-encoded as 18px strips on display), and OSC 8 hyperlinks
// (parsed, stored as interned linkId, re-emitted on render).
//
// Keys: space/f page down, b page up, Enter/j line down, k/y line up, d/u (^D/^U)
// half-screen down/up, g/G to top/bottom (or line N with a count), =/^G show the
// current position, /pat (?pat) search forward (backward), n/N repeat the search
// (in reverse), h help overlay, ^L repaint, q quit. A leading count repeats a
// motion; a count to d/u sets the half-screen step. Search is regex, smart-case,
// and highlights matches with a blue background (brighter for the current match).
// Up-scroll full-repaints the window (an incremental up-paint clobbers image cells).
// +N opens with line N at the top (like more/less +N).
// --dump renders the text grid to stdout (no images, for testing).
// --dump-images renders text + re-encoded sixel strips (for render testing).
// --imginfo prints decoded image metadata + ASCII rasters.

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gmore_run.h"
#include "width_probe.h"

static std::string readAll(int fd) {
    std::string s;
    char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) s.append(buf, static_cast<size_t>(n));
    return s;
}

static int streamCopy(int inFd, int outFd) {
    char buf[65536];
    for (;;) {
        ssize_t n = read(inFd, buf, sizeof buf);
        if (n == 0) return 0;
        if (n < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        ssize_t off = 0;
        while (off < n) {
            ssize_t m = write(outFd, buf + off, static_cast<size_t>(n - off));
            if (m < 0) {
                if (errno == EINTR) continue;
                return 1;
            }
            off += m;
        }
    }
}

static const char* kUsage =
    "usage: gmore [options] [+N] [file]\n"
    "\n"
    "A graphics-aware pager. Reads standard input when no file (or '-') is given.\n"
    "\n"
    "options:\n"
    "  +N              open with line N at the top (like more/less)\n"
    "  -h, --help      show this help and exit\n"
    "  --dump          render the text grid to stdout (no images)\n"
    "  --dump-images   render text and re-encoded sixel strips to stdout\n"
    "  --imginfo       print decoded image metadata and ASCII rasters\n"
    "  --nav-trace     trace navigation (for testing)\n"
    "  --              treat the remaining argument as a file name\n"
    "\n"
    "Press h inside the pager for the key bindings.\n";

int main(int argc, char** argv) {
    bool dump = false, dumpImages = false, imginfo = false, navTrace = false;
    const char* path = nullptr;
    size_t startLine = 0;
    bool endOfOptions = false;
    for (int i = 1; i < argc; ++i) {
        if (endOfOptions)
            path = argv[i];
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::fputs(kUsage, stdout);
            return 0;
        } else if (std::strcmp(argv[i], "--") == 0)
            endOfOptions = true;
        else if (std::strcmp(argv[i], "--dump") == 0)
            dump = true;
        else if (std::strcmp(argv[i], "--dump-images") == 0) {
            dump = true;
            dumpImages = true;
        } else if (std::strcmp(argv[i], "--imginfo") == 0)
            imginfo = true;
        else if (std::strcmp(argv[i], "--nav-trace") == 0)
            navTrace = true;
        else if (argv[i][0] == '+') {
            char* end = nullptr;
            errno = 0;
            unsigned long n = std::strtoul(argv[i] + 1, &end, 10);
            if (argv[i][1] < '0' || argv[i][1] > '9' || *end != '\0' || errno == ERANGE) {
                std::fprintf(stderr, "gmore: invalid line number '%s'\n", argv[i]);
                std::fputs("Try 'gmore --help' for more information.\n", stderr);
                return 2;
            }
            startLine = n;
        } else if (std::strcmp(argv[i], "-") == 0)
            path = nullptr;
        else if (argv[i][0] == '-') {
            std::fprintf(stderr, "gmore: unrecognized option '%s'\n", argv[i]);
            std::fputs("Try 'gmore --help' for more information.\n", stderr);
            return 2;
        } else
            path = argv[i];
    }

    // Model the terminal's text widths (grapheme clusters or code point by code point) so the
    // emulated grid wraps exactly where the terminal will. Only when paging to a terminal.
    uw::initMode(isatty(STDOUT_FILENO) && !dump && !imginfo && !navTrace &&
                 !std::getenv("GMORE_KEYS"));

    // Fast passthrough: when stdout is not a terminal, gmore behaves like cat and
    // forwards bytes incrementally instead of buffering all input.
    if (!dump && !dumpImages && !imginfo && !navTrace && !std::getenv("GMORE_KEYS")) {
        if (!isatty(STDOUT_FILENO)) {
            if (path) {
                int fd = open(path, O_RDONLY);
                if (fd < 0) {
                    std::perror(path);
                    return 1;
                }
                int rc = streamCopy(fd, STDOUT_FILENO);
                close(fd);
                return rc;
            }
            return streamCopy(STDIN_FILENO, STDOUT_FILENO);
        }
        if (!path && !isatty(STDIN_FILENO)) {
            return gmore::run(std::string(),
                              dump,
                              dumpImages,
                              imginfo,
                              navTrace,
                              STDIN_FILENO,
                              {},
                              {},
                              nullptr,
                              startLine);
        }
    }

    std::string data;
    if (path) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            std::perror(path);
            return 1;
        }
        data = readAll(fd);
        close(fd);
    } else {
        data = readAll(STDIN_FILENO);
    }

    return gmore::run(
        std::move(data), dump, dumpImages, imginfo, navTrace, -1, {}, {}, nullptr, startLine);
}
