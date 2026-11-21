#pragma once

#include "unicode_width.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gmore {

/**
 * Attributes (interned). Colour encoding: 0 = default; tag in the high byte:
 * PAL|idx for a 256-palette index, TRUE|rgb for 24-bit. linkId 0 means no link.
 */
constexpr uint32_t PAL = 0x01000000u, TRUE = 0x02000000u;
inline uint32_t pal(int i) {
    return PAL | (i & 0xFF);
}
inline uint32_t tru(int r, int g, int b) {
    return TRUE | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
}

enum { A_BOLD = 1, A_DIM = 2, A_ITALIC = 4, A_UNDER = 8, A_INVERSE = 16 };

struct Attr {
    uint32_t fg = 0, bg = 0;
    uint16_t flags = 0;
    /** OSC 8 linkId (0 = no link). */
    uint32_t link = 0;
    bool operator==(const Attr& o) const {
        return fg == o.fg && bg == o.bg && flags == o.flags && link == o.link;
    }
};

struct AttrHash {
    size_t operator()(const Attr& a) const {
        size_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t v) {
            h ^= v;
            h *= 1099511628211ull;
        };
        mix(a.fg);
        mix(a.bg);
        mix(a.flags);
        mix(a.link);
        return h;
    }
};

/**
 * Search-match highlight for one rendered row: cell-column spans [startCol,endCol)
 * to paint with a blue background (foreground left unchanged, like VSCode's find
 * highlight).
 */
struct Highlight {
    /** Sorted, non-overlapping spans in cell columns. */
    std::vector<std::pair<int, int>> spans;
    /** Index into spans of the active match, or -1. */
    int current = -1;
    /** Light steel blue. */
    static constexpr const char* OTHER = "\033[48;5;153m";
    /** Medium sky blue (#5fafff). */
    static constexpr const char* CUR = "\033[48;5;75m";
    bool empty() const { return spans.empty(); }
    /** Which highlight (if any) cell col falls in: 0 = none, 1 = other, 2 = current. */
    int at(int col) const {
        for (size_t k = 0; k < spans.size(); ++k)
            if (col >= spans[k].first && col < spans[k].second) return (int)k == current ? 2 : 1;
        return 0;
    }
};

struct Cell {
    char32_t cp = U' ';
    /** Index into gAttrs (0 = default). */
    uint16_t attr = 0;
    /** Display columns: 1 (normal), 2 (wide/fullwidth), 0 (wide continuation). */
    uint8_t width = 1;
    // uint8_t flags = 0;
    /**
     * Trailing zero-width code points (combining marks, variation selectors,
     * ZWJ-joined parts) that belong to this cell's grapheme cluster.
     */
    std::u32string combine;
};

inline void appendUtf8(std::string& out, char32_t cp) {
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

struct Image {
    /** Absolute grid row of the image's top. */
    size_t row = 0;
    /** Column of the image's left edge. */
    int col = 0;
    /** Painted pixel size. */
    int Ph = 0, Pv = 0;
    std::vector<uint32_t> px;
    std::string sixel;
    std::string kitty;
    /** Kitty image id (parsed from the APC's i=). */
    uint32_t kid = 0;
    int footCols = 0, footRows = 0;
    /** Kitty X= pixel offset inside the first cell, re-sent on every placement. */
    int xOffset = 0;

    bool isKitty() const { return !kitty.empty(); }
    int heightCells(int cellH) const {
        return footRows > 0 ? footRows : std::max(1, (Pv + cellH - 1) / cellH);
    }
    int pxPerRow(int cellH) const {
        int hc = heightCells(cellH);
        return std::max(1, (Pv + hc - 1) / hc);
    }
};

}  // namespace gmore
