#pragma once

#include <memory>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "gmore_types.h"

namespace gmore {

class Emulator {
public:
    Emulator(int w, int h, int cw, int ch);
    ~Emulator();
    Emulator(Emulator&&) noexcept;

    Emulator(const Emulator&) = delete;
    Emulator& operator=(const Emulator&) = delete;

    void forgetKittyTransmissions() const;
    void feed(const char* p, size_t n);
    size_t contentRows() const;
    size_t pendingImageRow() const;
    std::vector<std::pair<int, int>> matchSpans(size_t absRow, const std::regex& re) const;
    void renderRow(size_t absRow,
                   std::string& out,
                   bool withImages = true,
                   size_t viewTop = 0,
                   size_t viewBot = 0,
                   bool withLinks = true,
                   const Highlight* hl = nullptr) const;
    void paintImages(std::string& out, size_t winFirst, int winRows, size_t winBot) const;
    const std::vector<Image>& images() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gmore
