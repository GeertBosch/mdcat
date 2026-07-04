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

int run(std::string data,
        bool dump = false,
        bool dumpImages = false,
        bool imginfo = false,
        bool navTrace = false,
        int streamFd = -1,
        Relayout relayout = {});

}  // namespace gmore
