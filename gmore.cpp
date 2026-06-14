// gmore — a graphics-aware pager. It emulates a pragmatic subset of a terminal
// into a cell grid (the scrollback buffer), so it can page ANY text+sixel stream
// with no constraints on the producer — including cursor movement. See
// docs/adr/0001-gmore-data-model.md for why a cell grid (not line spans).
//
// STATUS: sixel decode + encode + strip rendering complete. The cell-grid emulator
// handles text, UTF-8, wrap, SGR colour/style, cursor movement, DECSC/DECRC, EL/ED,
// sixel DCS (decoded to RGBA rasters, re-encoded as 18px strips on display), and
// OSC 8 hyperlinks (parsed, stored as interned linkId, re-emitted on render).
//
// Keys: space/f page down, b page up, Enter/j line down, k/y line up, q quit.
// Up-scroll full-repaints the window (an incremental up-paint clobbers image cells).
// --dump renders the text grid to stdout (no images, for testing).
// --dump-images renders text + re-encoded sixel strips (for render testing).
// --imginfo prints decoded image metadata + ASCII rasters.

#include "gmore_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <fcntl.h>

static std::string readAll(int fd) {
    std::string s; char buf[65536]; ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) s.append(buf, static_cast<size_t>(n));
    return s;
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
