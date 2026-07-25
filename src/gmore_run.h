#pragma once

#include <functional>
#include <string>

namespace gmore {

// Re-layout hook for the integrated pager. On a SIGWINCH resize gmore calls this with the
// new terminal width; the callback re-renders the held markdown source at that width and
// returns a read fd streaming the fresh bytes (or -1 on failure). gmore rebuilds its grid
// from that fd and restores the reading position via source-line provenance. An empty
// callback (the default) disables re-layout — standalone gmore has no re-renderer, so it
// just repaints its existing grid at the new size.
using Relayout = std::function<int(int width)>;

// `promptSuffix` is appended to the --More--/(END) prompt (e.g. " (file 2 of 5)"), letting a
// caller that pages multiple files one gmore::run() at a time show less(1)'s file position.
// `quitRequested`, if non-null, is set to true iff the user pressed q/Q (as opposed to reaching
// the end of input and dismissing the last page with space) — the signal a multi-file caller
// needs to stop advancing to the next file.
int run(std::string data,
        bool dump = false,
        bool dumpImages = false,
        bool imginfo = false,
        bool navTrace = false,
        int streamFd = -1,
        Relayout relayout = {},
        const std::string& promptSuffix = {},
        bool* quitRequested = nullptr);

}  // namespace gmore
