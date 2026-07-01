#pragma once

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

/**
 * Column width of a code point, mirrored from mdcat.cpp's codePointWidth so the pager
 * measures text exactly as the renderer laid it out. 0 = zero-width (combining/joiner/VS),
 * 2 = wide, else 1.
 */
inline int cellWidthOf(char32_t cp) {
    auto in = [&](char32_t lo, char32_t hi) { return cp >= lo && cp <= hi; };
    if (cp == 0) return 0;
    if (in(0x0300, 0x036F) || in(0x1AB0, 0x1AFF) || in(0x1DC0, 0x1DFF) || in(0x20D0, 0x20FF) ||
        in(0xFE20, 0xFE2F) || cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF ||
        in(0xFE00, 0xFE0F) || in(0xE0100, 0xE01EF))
        return 0;
    if (in(0x1100, 0x115F) || cp == 0x2329 || cp == 0x232A || in(0x2E80, 0x303E) ||
        in(0x3041, 0x33FF) || in(0x3400, 0x4DBF) || in(0x4E00, 0x9FFF) || in(0xA000, 0xA4CF) ||
        in(0xAC00, 0xD7A3) || in(0xF900, 0xFAFF) || in(0xFE10, 0xFE19) || in(0xFE30, 0xFE6F) ||
        in(0xFF00, 0xFF60) || in(0xFFE0, 0xFFE6) || in(0x1F300, 0x1F64F) || in(0x1F680, 0x1F6FF) ||
        in(0x1F900, 0x1F9FF) || in(0x1FA70, 0x1FAFF) || in(0x20000, 0x3FFFD))
        return 2;
    return 1;
}

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

/**
 * Map a Unicode code point to its "base" ASCII equivalent for font-independent
 * search: math italic/bold letters → A-Z/a-z; super/subscript digits and
 * punctuation → their ASCII originals; letterlike math symbols (ℝ, ℤ, ℎ …) →
 * their plain-letter equivalents.  Returns `cp` unchanged when no mapping
 * applies.
 *
 * The covered math-alphabet ranges are those used by mdcat's LaTeX renderer
 * (italic default, bold \mathbf, double-struck \mathbb) plus the most common
 * additional Mathematical-Alphanumeric-Symbols blocks so that font variants
 * from any source are normalised the same way.
 */
inline char32_t foldFontVariant(char32_t cp) {
    // Mathematical Bold A-Z (U+1D400-U+1D419) and a-z (U+1D41A-U+1D433)
    if (cp >= 0x1D400 && cp <= 0x1D433) {
        unsigned idx = cp - 0x1D400;
        return idx < 26 ? (char32_t)(U'A' + idx) : (char32_t)(U'a' + (idx - 26));
    }
    // Mathematical Italic A-Z (U+1D434-U+1D44D) and a-z (U+1D44E-U+1D467)
    // U+1D455 is unassigned (italic h is U+210E, handled below).
    if (cp >= 0x1D434 && cp <= 0x1D467) {
        unsigned idx = cp - 0x1D434;
        return idx < 26 ? (char32_t)(U'A' + idx) : (char32_t)(U'a' + (idx - 26));
    }
    // Mathematical Bold Italic A-Z (U+1D468-U+1D481) and a-z (U+1D482-U+1D49B)
    if (cp >= 0x1D468 && cp <= 0x1D49B) {
        unsigned idx = cp - 0x1D468;
        return idx < 26 ? (char32_t)(U'A' + idx) : (char32_t)(U'a' + (idx - 26));
    }
    // Mathematical Double-Struck A-Z (U+1D538-U+1D551) and a-z (U+1D552-U+1D56B)
    // Some uppercase slots are unassigned (those letters live in Letterlike Symbols below).
    if (cp >= 0x1D538 && cp <= 0x1D56B) {
        unsigned idx = cp - 0x1D538;
        return idx < 26 ? (char32_t)(U'A' + idx) : (char32_t)(U'a' + (idx - 26));
    }
    // Mathematical Sans-Serif A-Z (U+1D5A0-U+1D5B9) and a-z (U+1D5BA-U+1D5D3)
    if (cp >= 0x1D5A0 && cp <= 0x1D5D3) {
        unsigned idx = cp - 0x1D5A0;
        return idx < 26 ? (char32_t)(U'A' + idx) : (char32_t)(U'a' + (idx - 26));
    }
    // Mathematical Monospace A-Z (U+1D670-U+1D689) and a-z (U+1D68A-U+1D6A3)
    if (cp >= 0x1D670 && cp <= 0x1D6A3) {
        unsigned idx = cp - 0x1D670;
        return idx < 26 ? (char32_t)(U'A' + idx) : (char32_t)(U'a' + (idx - 26));
    }
    // Letterlike symbols used as math letter variants
    switch (cp) {
    case 0x210E: return U'h';  // ℎ Planck constant (math italic h)
    case 0x2102: return U'C';  // ℂ double-struck C
    case 0x210D: return U'H';  // ℍ double-struck H
    case 0x2115: return U'N';  // ℕ double-struck N
    case 0x2119: return U'P';  // ℙ double-struck P
    case 0x211A: return U'Q';  // ℚ double-struck Q
    case 0x211D: return U'R';  // ℝ double-struck R
    case 0x2124:
        return U'Z';  // ℤ double-struck Z
    // Superscript digits and operators
    case 0x2070: return U'0';  // ⁰
    case 0x00B9: return U'1';  // ¹
    case 0x00B2: return U'2';  // ²
    case 0x00B3: return U'3';  // ³
    case 0x2074: return U'4';  // ⁴
    case 0x2075: return U'5';  // ⁵
    case 0x2076: return U'6';  // ⁶
    case 0x2077: return U'7';  // ⁷
    case 0x2078: return U'8';  // ⁸
    case 0x2079: return U'9';  // ⁹
    case 0x207A: return U'+';  // ⁺
    case 0x207B: return U'-';  // ⁻
    case 0x207C: return U'=';  // ⁼
    case 0x207D: return U'(';  // ⁽
    case 0x207E: return U')';  // ⁾
    case 0x207F: return U'n';  // ⁿ
    case 0x2071: return U'i';  // ⁱ
    case 0x00B7:
        return U'.';  // · middle dot (superscript '.' in mdcat)
    // Subscript digits and operators
    case 0x2080: return U'0';  // ₀
    case 0x2081: return U'1';  // ₁
    case 0x2082: return U'2';  // ₂
    case 0x2083: return U'3';  // ₃
    case 0x2084: return U'4';  // ₄
    case 0x2085: return U'5';  // ₅
    case 0x2086: return U'6';  // ₆
    case 0x2087: return U'7';  // ₇
    case 0x2088: return U'8';  // ₈
    case 0x2089: return U'9';  // ₉
    case 0x208A: return U'+';  // ₊
    case 0x208B: return U'-';  // ₋
    case 0x208C: return U'=';  // ₌
    case 0x208D: return U'(';  // ₍
    case 0x208E: return U')';  // ₎
    }
    return cp;
}

/**
 * Return a copy of the UTF-8 string `s` with every code point replaced by its
 * foldFontVariant() equivalent.  Used to normalise search patterns and grid
 * row text so that, e.g., a search for plain 'a' matches math-italic '𝑎'.
 */
inline std::string foldText(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        auto b0 = static_cast<unsigned char>(s[i++]);
        char32_t cp;
        if (b0 < 0x80) {
            cp = b0;
        } else if (b0 < 0xC2 || b0 >= 0xF8) {
            out += static_cast<char>(b0);  // invalid lead byte — pass through
            continue;
        } else {
            int n = (b0 < 0xE0) ? 2 : (b0 < 0xF0) ? 3 : 4;
            cp = b0 & static_cast<unsigned char>(0x3F >> (n - 1));
            for (int k = 1; k < n; ++k) {
                if (i >= s.size()) {
                    cp = 0xFFFDu;
                    break;
                }
                auto b = static_cast<unsigned char>(s[i++]);
                if ((b & 0xC0) != 0x80) {
                    cp = 0xFFFDu;
                    --i;
                    break;
                }
                cp = (cp << 6) | (b & 0x3Fu);
            }
        }
        appendUtf8(out, foldFontVariant(cp));
    }
    return out;
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
