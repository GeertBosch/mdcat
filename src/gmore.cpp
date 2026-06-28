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
// --dump renders the text grid to stdout (no images, for testing).
// --dump-images renders text + re-encoded sixel strips (for render testing).
// --imginfo prints decoded image metadata + ASCII rasters.

#include "gmore.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static std::string readAll(int fd) {
    std::string s; char buf[65536]; ssize_t n;
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

int main(int argc, char** argv) {
    bool dump = false, dumpImages = false, imginfo = false, navTrace = false;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump") == 0) dump = true;
        else if (std::strcmp(argv[i], "--dump-images") == 0) { dump = true; dumpImages = true; }
        else if (std::strcmp(argv[i], "--imginfo") == 0) imginfo = true;
        else if (std::strcmp(argv[i], "--nav-trace") == 0) navTrace = true;
        else if (std::strcmp(argv[i], "-") == 0) path = nullptr;
        else path = argv[i];
    }

    // Fast passthrough: when stdout is not a terminal, gmore behaves like cat and
    // forwards bytes incrementally instead of buffering all input.
    if (!dump && !dumpImages && !imginfo && !navTrace && !std::getenv("GMORE_KEYS")) {
        if (!isatty(STDOUT_FILENO)) {
            if (path) {
                int fd = open(path, O_RDONLY);
                if (fd < 0) { std::perror(path); return 1; }
                int rc = streamCopy(fd, STDOUT_FILENO);
                close(fd);
                return rc;
            }
            return streamCopy(STDIN_FILENO, STDOUT_FILENO);
        }
        if (!path && !isatty(STDIN_FILENO)) {
            return gmore::run(std::string(), dump, dumpImages, imginfo, navTrace, STDIN_FILENO);
        }
    }

    std::string data;
    if (path) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) { std::perror(path); return 1; }
        data = readAll(fd); close(fd);
    } else {
        data = readAll(STDIN_FILENO);
    }

    return gmore::run(std::move(data), dump, dumpImages, imginfo, navTrace);
}
