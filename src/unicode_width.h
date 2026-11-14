// Terminal display widths, shared by mdcat (layout and reflow) and gmore's emulator (painting),
// so the renderer and the pager always agree on how many columns text occupies.
//
// This is a pragmatic subset of UAX #11 / the Unicode emoji data — the ranges that actually occur
// in the content mdcat renders — not a full property database.
#pragma once

#include <cstdint>

namespace uw {

inline bool inRange(char32_t cp, char32_t lo, char32_t hi) {
    return cp >= lo && cp <= hi;
}

inline bool isRegionalIndicator(char32_t cp) {
    return inRange(cp, 0x1F1E6, 0x1F1FF);
}

// Terminal column width of one code point: 0 for zero-width (combining marks, joiners, variation
// selectors, the BOM), 2 for East-Asian-Wide / Fullwidth, emoji drawn double width and regional
// indicators (a flag is a pair of them), and 1 otherwise.
inline int codePointWidth(char32_t cp) {
    if (cp == 0) return 0;
    if (inRange(cp, 0x0300, 0x036F) ||                   // combining diacritical marks
        inRange(cp, 0x1AB0, 0x1AFF) ||                   // combining diacritical marks extended
        inRange(cp, 0x1DC0, 0x1DFF) ||                   // combining diacritical marks supplement
        inRange(cp, 0x20D0, 0x20FF) ||                   // combining marks for symbols
        inRange(cp, 0xFE20, 0xFE2F) ||                   // combining half marks
        cp == 0x200B || cp == 0x200C || cp == 0x200D ||  // ZWSP, ZWNJ, ZWJ
        cp == 0xE000 || cp == 0xE001 ||                  // mdcat's math-atom sentinels
        cp == 0xFEFF ||                                  // ZWNBSP / BOM
        inRange(cp, 0xFE00, 0xFE0F) ||                   // variation selectors 1-16
        inRange(cp, 0xE0100, 0xE01EF))                   // variation selectors supplement
        return 0;
    if (inRange(cp, 0x1100, 0x115F) ||    // Hangul Jamo
        cp == 0x2329 || cp == 0x232A ||   // angle brackets
        inRange(cp, 0x2E80, 0x303E) ||    // CJK radicals, Kangxi, CJK symbols/punctuation
        inRange(cp, 0x3041, 0x33FF) ||    // Hiragana, Katakana, CJK symbols, enclosed CJK
        inRange(cp, 0x3400, 0x4DBF) ||    // CJK Extension A
        inRange(cp, 0x4E00, 0x9FFF) ||    // CJK Unified Ideographs
        inRange(cp, 0xA000, 0xA4CF) ||    // Yi
        inRange(cp, 0xAC00, 0xD7A3) ||    // Hangul Syllables
        inRange(cp, 0xF900, 0xFAFF) ||    // CJK compatibility ideographs
        inRange(cp, 0xFE10, 0xFE19) ||    // vertical forms
        inRange(cp, 0xFE30, 0xFE6F) ||    // CJK compatibility forms, small form variants
        inRange(cp, 0xFF00, 0xFF60) ||    // fullwidth forms
        inRange(cp, 0xFFE0, 0xFFE6) ||    // fullwidth signs
        isRegionalIndicator(cp) ||        // flag halves
        inRange(cp, 0x1F300, 0x1F64F) ||  // Misc symbols & pictographs, emoticons
        inRange(cp, 0x1F680, 0x1F6FF) ||  // transport & map symbols
        inRange(cp, 0x1F900, 0x1F9FF) ||  // supplemental symbols & pictographs
        inRange(cp, 0x1FA70, 0x1FAFF) ||  // symbols & pictographs extended-A
        inRange(cp, 0x20000, 0x3FFFD))    // CJK Extension B+ and plane 3
        return 2;
    return 1;
}

// Does `next` continue the grapheme cluster whose most recent code point is `prev`? True for
// zero-width followers (combining marks, selectors), skin-tone modifiers, whatever follows a ZWJ,
// and the second regional indicator of a flag.
inline bool continuesCluster(char32_t prev, char32_t next) {
    return codePointWidth(next) == 0 || inRange(next, 0x1F3FB, 0x1F3FF) || prev == 0x200D ||
        (isRegionalIndicator(prev) && isRegionalIndicator(next));
}

// Width of a cluster currently `width` columns wide once `next` (which continuesCluster) joins it.
// A cluster is as wide as its widest part; an emoji presentation selector (U+FE0F) is what makes a
// narrow base such as U+2764 draw as a two-column emoji.
inline int widthWithFollower(int width, char32_t next) {
    if (next == 0xFE0F && width < 2) return 2;
    return codePointWidth(next) > width ? codePointWidth(next) : width;
}

}  // namespace uw
