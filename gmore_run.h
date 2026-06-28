#pragma once

#include <string>

namespace gmore {

int run(std::string data, bool dump = false, bool dumpImages = false, bool imginfo = false,
        bool navTrace = false, int streamFd = -1);

}  // namespace gmore
