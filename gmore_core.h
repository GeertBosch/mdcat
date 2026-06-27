// gmore_core.h — shared implementation for the gmore pager.
// Included by both gmore.cpp (which provides a standalone main()) and mdcat.cpp
// (which calls gmore::run() when stdout is a tty). All state is local to the
// translation unit that includes this header; including it in two TUs would
// create duplicate definitions, so it must be included exactly once per binary.

#pragma once

#include <algorithm>
#include <cctype>
#include <csignal>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace gmore {

// ---------------------------------------------------------------------------
// Attributes (interned). Colour encoding: 0 = default; tag in the high byte —
// PAL|idx for a 256-palette index, TRUE|rgb for 24-bit. linkId 0 = no link.
// ---------------------------------------------------------------------------
constexpr uint32_t PAL = 0x01000000u, TRUE = 0x02000000u;
static inline uint32_t pal(int i) { return PAL | (i & 0xFF); }
static inline uint32_t tru(int r, int g, int b) { return TRUE | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF); }

enum { A_BOLD = 1, A_DIM = 2, A_ITALIC = 4, A_UNDER = 8, A_INVERSE = 16 };

struct Attr {
    uint32_t fg = 0, bg = 0;
    uint16_t flags = 0;
    uint32_t link = 0;  // OSC 8 linkId (0 = no link)
    bool operator==(const Attr& o) const {
        return fg == o.fg && bg == o.bg && flags == o.flags && link == o.link;
    }
};
struct AttrHash {
    size_t operator()(const Attr& a) const {
        size_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
        mix(a.fg); mix(a.bg); mix(a.flags); mix(a.link);
        return h;
    }
};
static std::vector<Attr> gAttrs;
static std::unordered_map<Attr, uint16_t, AttrHash> gAttrMap;

static inline uint16_t internAttr(const Attr& a) {
    auto it = gAttrMap.find(a);
    if (it != gAttrMap.end()) return it->second;
    uint16_t id = static_cast<uint16_t>(gAttrs.size());
    gAttrs.push_back(a);
    gAttrMap.emplace(a, id);
    return id;
}

// ---------------------------------------------------------------------------
// URI interning for OSC 8 hyperlinks. id 0 = no link; ids start at 1.
// ---------------------------------------------------------------------------
static std::vector<std::string> gUris;  // gUris[0] unused; real URIs start at index 1
static std::unordered_map<std::string, uint32_t> gUriMap;

static inline uint32_t internUri(const std::string& uri) {
    if (uri.empty()) return 0;
    auto it = gUriMap.find(uri);
    if (it != gUriMap.end()) return it->second;
    if (gUris.empty()) gUris.emplace_back();  // reserve slot 0
    uint32_t id = static_cast<uint32_t>(gUris.size());
    gUris.push_back(uri);
    gUriMap.emplace(uri, id);
    return id;
}

// ---------------------------------------------------------------------------
// Cells + UTF-8 helpers
// ---------------------------------------------------------------------------
// Search-match highlight for one rendered row: cell-column spans [startCol,endCol)
// to paint with a blue BACKGROUND (foreground left unchanged, like VSCode's find
// highlight). The span at index `current` (if any) gets a more saturated blue —
// the match search just jumped to; the rest get a light blue. Both backgrounds are
// light enough that black/dark text still contrasts.
struct Highlight {
    std::vector<std::pair<int,int>> spans;   // sorted, non-overlapping, in cell columns
    int current = -1;                        // index into spans of the active match, or -1
    static constexpr const char* OTHER = "\033[48;5;153m";   // light steel blue
    static constexpr const char* CUR   = "\033[48;5;75m";    // medium sky blue (#5fafff)
    bool empty() const { return spans.empty(); }
    // Which highlight (if any) cell `col` falls in: 0 = none, 1 = other, 2 = current.
    int at(int col) const {
        for (size_t k = 0; k < spans.size(); ++k)
            if (col >= spans[k].first && col < spans[k].second)
                return (int)k == current ? 2 : 1;
        return 0;
    }
};

struct Cell {
    char32_t cp = U' ';
    uint16_t attr = 0;   // index into gAttrs (0 = default)
    uint8_t width = 1;   // display columns: 1 (normal), 2 (wide/fullwidth), 0 (wide continuation)
    uint8_t flags = 0;
    // Trailing zero-width code points (combining marks, variation selectors, ZWJ-joined parts) that
    // belong to this cell's grapheme cluster. Empty for the common case; non-empty keeps an emoji
    // sequence or an accented letter rendering as one unit. A width-0 continuation cell never carries
    // these — they hang off the wide base cell that precedes it.
    std::u32string combine;
};

// Column width of a code point, mirrored from mdcat.cpp's codePointWidth so the pager measures text
// exactly as the renderer laid it out. 0 = zero-width (combining/joiner/VS), 2 = wide, else 1.
static inline int cellWidthOf(char32_t cp) {
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

static inline void appendUtf8(std::string& out, char32_t cp) {
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

static std::string sgrFor(uint16_t id) {
    const Attr& a = gAttrs[id];
    std::string s = "\033[0";  // reset, then apply — always correct from any prior state
    if (a.flags & A_BOLD) s += ";1";
    if (a.flags & A_DIM) s += ";2";
    if (a.flags & A_ITALIC) s += ";3";
    if (a.flags & A_UNDER) s += ";4";
    if (a.flags & A_INVERSE) s += ";7";
    auto color = [&](uint32_t c, const char* p38) {
        if (!c) return;
        if ((c & 0xFF000000u) == PAL) { s += ";"; s += p38; s += ";5;"; s += std::to_string(c & 0xFF); }
        else { int v = c & 0xFFFFFF; s += ";"; s += p38; s += ";2;"; s += std::to_string((v >> 16) & 0xFF);
               s += ";"; s += std::to_string((v >> 8) & 0xFF); s += ";"; s += std::to_string(v & 0xFF); }
    };
    color(a.fg, "38");
    color(a.bg, "48");
    s += "m";
    return s;
}

// ---------------------------------------------------------------------------
// Sixel images. Decoded once to an RGBA raster (px[y*Ph + x]; 0 = unset/transparent,
// else 0xFFrrggbb). Anchored in the grid at an absolute row + column. The 18px-strip
// rendering re-encodes row ranges on demand (next milestone).
// ---------------------------------------------------------------------------
struct Image {
    size_t row = 0;       // absolute grid row of the image's top
    int col = 0;          // column of the image's left edge
    int Ph = 0, Pv = 0;   // painted pixel size
    std::vector<uint32_t> px;
    // The producer's original DCS payload (everything after `ESC P`, before ST).
    // We replay these exact bytes when painting (verbatim is highest-fidelity and
    // avoids re-encoding artifacts — our re-encoder redefines the palette per band
    // with shifting indices, which some terminals, e.g. iTerm2, render wrong). The
    // decoded `px` raster is kept for layout, --imginfo, and clipped re-encoding.
    std::string sixel;

    // Kitty graphics: the producer's full chunked APC transmission (ESC _ G ... ESC \,
    // possibly many chunks), captured verbatim. When non-empty this is a Kitty image,
    // not a sixel: `kid` is its Kitty image id and Pw/Pv (= Ph here is unused) the PNG's
    // pixel size read from its IHDR. We transmit the bytes ONCE (rewriting a=T->a=t so it
    // does not draw at the wrong spot) and thereafter paint visible bands with cheap a=p
    // crop placements — no per-scroll re-encode. See transmitKitty / placeKitty.
    std::string kitty;
    uint32_t kid = 0;     // Kitty image id (parsed from the APC's i=)
    bool isKitty() const { return !kitty.empty(); }
};

// Decode a sixel DCS payload (everything after `ESC P`, up to but not including ST)
// into img. Handles the intro params, `"`-raster attrs, `#`-colour define/select
// (RGB; HLS approximated), the `?`..`~` sixel bytes, `!`-run-length, `$` (CR) and
// `-` (graphics newline). Returns true on a non-empty raster.
static bool decodeSixel(const std::string& s, Image& img) {
    size_t i = 0;
    while (i < s.size() && s[i] != 'q') ++i;   // skip intro params P1;P2;P3
    if (i >= s.size()) return false;
    ++i;                                       // past 'q'

    auto readNums = [&](std::vector<int>& v) {
        std::string n;
        while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == ';')) {
            if (s[i] == ';') { v.push_back(n.empty() ? 0 : std::atoi(n.c_str())); n.clear(); }
            else n += s[i];
            ++i;
        }
        if (!n.empty()) v.push_back(std::atoi(n.c_str()));
    };

    int Ph = 0, Pv = 0;
    std::unordered_map<int, uint32_t> pal;
    int cur = 0, x = 0, band = 0, maxX = 0;
    std::vector<std::vector<uint32_t>> g;       // g[pixelRow][x]
    auto colorOf = [&](int idx) { auto it = pal.find(idx); return it != pal.end() ? it->second : 0xFF000000u; };
    auto setpx = [&](int px_x, int px_y, uint32_t c) {
        if ((size_t)px_y >= g.size()) g.resize(px_y + 1);
        auto& r = g[px_y];
        if ((size_t)px_x >= r.size()) r.resize(px_x + 1, 0);
        r[px_x] = c;
    };
    auto plot = [&](int bits, int n) {
        for (int r = 0; r < n; ++r) {
            for (int b = 0; b < 6; ++b) if (bits & (1 << b)) setpx(x, band * 6 + b, colorOf(cur));
            ++x;
        }
        if (x > maxX) maxX = x;
    };

    while (i < s.size()) {
        char c = s[i];
        if (c == '"') { ++i; std::vector<int> v; readNums(v); if (v.size() >= 4) { Ph = v[2]; Pv = v[3]; } continue; }
        if (c == '#') {
            ++i; std::vector<int> v; readNums(v);
            if (v.size() == 1) cur = v[0];
            else if (v.size() >= 5) {
                int idx = v[0], pu = v[1];
                uint32_t rgb = (pu == 2)
                    ? (0xFF000000u | ((v[2] * 255 / 100) << 16) | ((v[3] * 255 / 100) << 8) | (v[4] * 255 / 100))
                    : 0xFF808080u;            // HLS: approximate (our sources use RGB)
                pal[idx] = rgb; cur = idx;
            }
            continue;
        }
        if (c == '!') {
            ++i; std::string n; while (i < s.size() && std::isdigit((unsigned char)s[i])) n += s[i++];
            int cnt = std::max(1, std::atoi(n.c_str()));
            if (i < s.size() && s[i] >= '?' && s[i] <= '~') plot(s[i++] - '?', cnt);
            continue;
        }
        if (c == '$') { x = 0; ++i; continue; }
        if (c == '-') { x = 0; ++band; ++i; continue; }
        if (c >= '?' && c <= '~') { plot(c - '?', 1); ++i; continue; }
        ++i;                                   // ignore stray bytes (whitespace, etc.)
    }

    if (Ph <= 0) Ph = maxX;
    if (Pv <= 0) Pv = (int)g.size();
    if (Ph <= 0 || Pv <= 0) return false;
    img.Ph = Ph; img.Pv = Pv;
    img.px.assign((size_t)Ph * Pv, 0);
    for (int y = 0; y < Pv && y < (int)g.size(); ++y)
        for (int xx = 0; xx < Ph && xx < (int)g[y].size(); ++xx)
            img.px[(size_t)y * Ph + xx] = g[y][xx];
    return true;
}

// Encode raster pixels [y0, y1) × [0, Ph) of img into a sixel DCS sequence.
// y0/y1 are pixel rows; caller must clamp to [0, img.Pv). Returns the full
// ESC P … ST sequence ready to write to the terminal.
static std::string encodeSixel(const Image& img, int y0, int y1) {
    std::string out;
    int h = y1 - y0;
    if (h <= 0 || img.Ph <= 0) return out;

    out += "\033P0;1;0q";
    // Raster attribute: pixel aspect 1:1, Ph wide, h tall
    out += '"'; out += "1;1;"; out += std::to_string(img.Ph); out += ';'; out += std::to_string(h); out += '\n';

    // Walk sixel bands (6 px each)
    for (int band = 0; y0 + band * 6 < y1; ++band) {
        int py0 = y0 + band * 6;           // first pixel row of this band
        int py1 = std::min(py0 + 6, y1);   // exclusive

        // Collect unique colours in this band (in encounter order, index 0…)
        std::vector<uint32_t> palette;          // palette[n] = 0xFFrrggbb
        std::unordered_map<uint32_t, int> idx;
        for (int py = py0; py < py1; ++py) {
            for (int x = 0; x < img.Ph; ++x) {
                uint32_t c = img.px[(size_t)py * img.Ph + x];
                if (c && !idx.count(c)) { idx[c] = (int)palette.size(); palette.push_back(c); }
            }
        }
        if (palette.empty()) {
            // blank band — emit a single transparent sixel row then advance band
            out += '-';
            continue;
        }

        // Emit colour definitions and bitmask rows. Sixel `#n;2;R;G;B` takes the
        // RGB components as PERCENTAGES (0–100), not 0–255 — so convert (with
        // rounding, which inverts the decoder's 0–100→0–255 expansion exactly).
        bool first_color = true;
        auto to100 = [](int v) { return (v * 100 + 127) / 255; };
        for (int n = 0; n < (int)palette.size(); ++n) {
            uint32_t c = palette[n];
            int r = to100((c >> 16) & 0xFF), g = to100((c >> 8) & 0xFF), b = to100(c & 0xFF);
            out += '#'; out += std::to_string(n);
            out += ";2;"; out += std::to_string(r);
            out += ';';  out += std::to_string(g);
            out += ';';  out += std::to_string(b);

            // Build the 6-bit bitmask row for this colour
            // bits[x] = bitmask byte ('?' + bits) for this colour at column x
            std::vector<unsigned char> bits(img.Ph, 0);
            for (int py = py0; py < py1; ++py) {
                int bit = 1 << (py - py0);
                for (int x = 0; x < img.Ph; ++x) {
                    if (img.px[(size_t)py * img.Ph + x] == c) bits[x] |= (unsigned char)bit;
                }
            }

            // Emit sixel bytes with run-length encoding (threshold: run ≥ 5)
            out += '#'; out += std::to_string(n);   // select colour register
            int x = 0;
            while (x < img.Ph) {
                unsigned char byte = (unsigned char)('?' + bits[x]);
                int run = 1;
                while (x + run < img.Ph && bits[x + run] == bits[x]) ++run;
                if (run >= 5) {
                    out += '!'; out += std::to_string(run); out += (char)byte;
                } else {
                    for (int k = 0; k < run; ++k) out += (char)byte;
                }
                x += run;
            }

            if (n + 1 < (int)palette.size()) out += '$';  // CR: more colours on same band
            (void)first_color; first_color = false;
        }
        out += '-';  // graphics newline: advance to next band
    }
    out += "\033\\";
    return out;
}

// Build a full `ESC P … ST` sequence that replays a producer's original sixel
// payload (`img.sixel`, the bytes after `ESC P`), showing only the pixel-row range
// [skipPx, img.Pv) clipped to `keepPx` rows. Replaying verbatim is highest-fidelity:
// the producer (timg) defines its ≤256-colour palette ONCE up front with stable
// register indices, which is what terminals expect — unlike our per-band re-encoder,
// which redefines registers every band with shifting indices and some terminals
// (iTerm2) render garbled.
//
// A sixel payload is: intro params up to `q`, the raster attribute `"Pan;Pad;Ph;Pv`,
// the `#`-colour definitions, then 6px BANDS delimited by `-` (graphics newline; the
// only structural `-`, since pixel data is `?`..`~`). timg emits all colour defs
// before the first band (verified), so dropping leading bands (skipPx, top-clip when
// an image has scrolled partly off the top) or trailing bands (keepPx, bottom-clip
// when it runs past the screen) only removes rows — never drops a colour a kept band
// needs, never adds one. We keep the up-front palette, the bands covering the
// requested rows, and rewrite Pv to the kept height.
static std::string replaySixel(const Image& img, int skipPx, int keepPx) {
    const std::string& s = img.sixel;
    int skipBands = (skipPx > 0) ? skipPx / 6 : 0;     // align top clip to a band boundary
    int alignedSkip = skipBands * 6;
    int avail = img.Pv - alignedSkip;
    if (avail <= 0) return std::string();              // nothing visible
    int outPv = (keepPx > 0 && keepPx < avail) ? keepPx : avail;

    if (s.empty()) return encodeSixel(img, alignedSkip, alignedSkip + outPv);  // no bytes: re-encode

    // Whole image, verbatim — the common case (image fits the window).
    if (skipBands == 0 && outPv >= img.Pv) { return "\033P" + s + "\033\\"; }

    size_t q = s.find('q');
    size_t quote = (q == std::string::npos) ? std::string::npos : s.find('"', q);
    if (quote == std::string::npos) return encodeSixel(img, alignedSkip, alignedSkip + outPv);

    // Parse `Pan;Pad;Ph;Pv`; rewrite Pv to outPv.
    size_t p = quote + 1;
    std::string nums;
    while (p < s.size() && (std::isdigit((unsigned char)s[p]) || s[p] == ';')) nums += s[p++];
    int pan = 1, pad = 1, ph = img.Ph, pv = img.Pv;
    std::sscanf(nums.c_str(), "%d;%d;%d;%d", &pan, &pad, &ph, &pv);

    // The segment from `p` to the first `-` holds the colour defs (and band 0's pixel
    // data). Find the first band separator; the colour defs are the `#n;…;…;…;…`
    // (5-number) statements within that segment.
    size_t firstDash = s.find('-', p);
    if (firstDash == std::string::npos) firstDash = s.size();

    // Locate the first KEPT band's start (after skipBands separators) and the cut
    // (after skipBands + ceil(outPv/6) separators).
    int keepBands = (outPv + 5) / 6;
    size_t firstKept = p;
    size_t cut = s.size();
    int seen = 0;
    for (size_t i = p; i < s.size(); ++i) {
        if (s[i] != '-') continue;
        ++seen;
        if (seen == skipBands) firstKept = i + 1;          // band `skipBands` begins here
        if (seen == skipBands + keepBands) { cut = i; break; }
    }

    std::string out = "\033P";
    out += s.substr(0, quote + 1);                         // through the opening '"'
    out += std::to_string(pan); out += ';'; out += std::to_string(pad); out += ';';
    out += std::to_string(ph);  out += ';'; out += std::to_string(outPv);
    if (skipBands == 0) {
        out += s.substr(p, cut - p);                       // colour defs + kept bands (from top)
    } else {
        // Keep the up-front colour defs, then the kept bands. Extract just the
        // `#n;2;R;G;B` defines from the pre-first-band segment (skip band-0 pixels).
        std::string seg = s.substr(p, firstDash - p);
        for (size_t i = 0; i < seg.size();) {
            if (seg[i] == '#') {
                size_t j = i + 1; int semis = 0;
                while (j < seg.size() && (std::isdigit((unsigned char)seg[j]) || seg[j] == ';')) {
                    if (seg[j] == ';') ++semis;
                    ++j;
                }
                if (semis >= 3) out += seg.substr(i, j - i);   // a #n;Pu;R;G;B colour define
                i = j;
            } else ++i;
        }
        out += s.substr(firstKept, cut - firstKept);       // then the kept bands
    }
    out += "\033\\";
    return out;
}

// ---------------------------------------------------------------------------
// Kitty graphics helpers. gmore ingests a Kitty image as the producer's exact
// chunked APC bytes (it never decodes the PNG); it only needs the image's pixel
// size for crop math, which it reads from the PNG IHDR carried in the first chunk.
// ---------------------------------------------------------------------------

// Decode a Kitty base64 payload prefix into raw bytes (enough for the PNG IHDR).
// Standard base64 alphabet; stops once `want` bytes are produced.
static std::string kittyB64DecodePrefix(const std::string& b64, size_t want) {
    static const std::string tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (char c : b64) {
        if (c == '=') break;
        size_t pos = tbl.find(c);
        if (pos == std::string::npos) continue;
        val = (val << 6) | static_cast<int>(pos);
        bits += 6;
        if (bits >= 0) { out.push_back(static_cast<char>((val >> bits) & 0xFF)); bits -= 8; }
        if (out.size() >= want) break;
    }
    return out;
}

// Read a PNG's pixel width/height from its IHDR (big-endian u32 at byte 16 and 20,
// after the 8-byte signature + 4-byte length + "IHDR"). Returns false if not a PNG.
static bool kittyPngSize(const std::string& png, int& w, int& h) {
    if (png.size() < 24) return false;
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8; ++i)
        if (static_cast<unsigned char>(png[i]) != sig[i]) return false;
    auto u32 = [&](size_t o) {
        return (static_cast<unsigned char>(png[o]) << 24) |
               (static_cast<unsigned char>(png[o + 1]) << 16) |
               (static_cast<unsigned char>(png[o + 2]) << 8) | static_cast<unsigned char>(png[o + 3]);
    };
    w = u32(16); h = u32(20);
    return w > 0 && h > 0;
}

// Parse a Kitty APC transmission `apc` (ESC _ G <controls>;<b64> ESC \ ... chunks) for
// its image id (i=) and pixel size (from the first chunk's PNG IHDR). Returns false if
// it is not a recoverable Kitty image transmission.
static bool kittyParse(const std::string& apc, uint32_t& id, int& pw, int& ph) {
    size_t g = apc.find("\033_G");
    if (g == std::string::npos) return false;
    size_t semi = apc.find(';', g);
    if (semi == std::string::npos) return false;
    std::string controls = apc.substr(g + 3, semi - (g + 3));
    size_t ip = controls.find("i=");
    id = (ip != std::string::npos) ? static_cast<uint32_t>(std::atoi(controls.c_str() + ip + 2)) : 0;
    size_t end = apc.find("\033\\", semi);
    if (end == std::string::npos) return false;
    std::string png = kittyB64DecodePrefix(apc.substr(semi + 1, end - semi - 1), 24);
    return kittyPngSize(png, pw, ph);
}

// Build the transmit-ONLY form of a captured Kitty APC: rewrite the first chunk's
// `a=T` (transmit+display) to `a=t` (transmit only) so sending it defines the image
// by id WITHOUT drawing it at the current cursor. Byte-exact; other chunks untouched.
static std::string kittyTransmitOnly(const std::string& apc) {
    std::string out = apc;
    size_t a = out.find("a=T");
    if (a != std::string::npos) out[a + 2] = 't';
    return out;
}

// Build a placement command for an already-transmitted Kitty image `id`: display a
// SOURCE-PIXEL crop rectangle (x,0..)+(w=pw,h=keepPx, starting at y=skipPx) scaled into
// a `cols`x`rows` cell box. q=2 suppresses the terminal's OK reply. No payload, so it is
// cheap to re-emit on every scroll — this is how a partially-scrolled image is clipped
// without re-encoding pixels (validated by tools/probe-kitty-clip.sh).
static std::string kittyPlace(uint32_t id, int pw, int skipPx, int keepPx, int cols, int rows) {
    if (keepPx < 1) keepPx = 1;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    std::string out = "\033_Ga=p,i=";
    out += std::to_string(id);
    out += ",q=2,x=0,y="; out += std::to_string(skipPx);
    out += ",w=";         out += std::to_string(pw);
    out += ",h=";         out += std::to_string(keepPx);
    out += ",c=";         out += std::to_string(cols);
    out += ",r=";         out += std::to_string(rows);
    out += "\033\\";
    return out;
}

// ---------------------------------------------------------------------------
// The emulator: a cell grid (rows grow downward = scrollback) with a screen
// viewport [top, top+H) in which the cursor lives, fed bytes by a state machine.
// ---------------------------------------------------------------------------
struct Emulator {
    int W, H;
    int cellW, cellH;        // pixels per cell (for sizing sixel images)
    std::vector<std::vector<Cell>> rows;
    std::vector<Image> images;
    // Kitty image ids already transmitted to the terminal this session. A Kitty image is sent
    // ONCE (its data chunks), then displayed by id with cheap crop placements on every (re)paint
    // and scroll. Mutable because painting is logically const but must remember what it has sent.
    mutable std::set<uint32_t> kittyTransmitted;
    size_t top = 0;          // absolute index of screen row 0
    int cr = 0, cc = 0;      // cursor, screen-relative
    Attr pen;                // current pen
    int sr = 0, sc = 0;      // DECSC save
    size_t stop = 0;         // DECSC save of `top` (see decsc/decrc)
    Attr spen;

    // parser state
    enum St { GROUND, ESC, CSI, OSC, DCS, APC } st = GROUND;
    std::string seq;         // accumulated escape payload
    bool escPend = false;    // saw ESC inside OSC/DCS/APC (looking for ST '\')
    char32_t uacc = 0;       // UTF-8 accumulator
    int uneed = 0;
    bool absorbLf = false;   // swallow one LF right after a sixel (cursor is already below it)
    std::string kittyAcc;    // accumulates a Kitty image's chunks (ESC_G..ESC\ × N) until m=0

    // Overstrike (typewriter convention used by `man`/groff via nroff): bold is
    // `X \b X`, underline is `_ \b X` (or `X \b _`). When a backspace lands the
    // cursor back onto the cell `put` just wrote, the next `put` is an overstrike:
    // we merge the two glyphs into one styled cell instead of overwriting. Both
    // fields must match for the merge — any other cursor motion clears the back-up.
    int osRow = -1, osCol = -1;   // cell `put` last wrote (overstrike target)
    bool osBack = false;          // a bs() just backed the cursor onto (osRow,osCol)

    Emulator(int w, int h, int cw, int ch)
        : W(w < 1 ? 1 : w), H(h < 1 ? 1 : h), cellW(cw < 1 ? 1 : cw), cellH(ch < 1 ? 1 : ch) { ensure(); }

    void ensure() { while (rows.size() < top + static_cast<size_t>(H)) rows.emplace_back(W); }
    std::vector<Cell>& screen(int r) { return rows[top + r]; }
    void scrollUp() { ++top; ensure(); }
    void blank(std::vector<Cell>& L, int a, int b) {
        for (int i = a; i < b && i < W; ++i) L[i] = Cell{};
    }

    void lfWrap() { if (cr + 1 >= H) scrollUp(); else ++cr; }
    void lf() { cc = 0; lfWrap(); haveBase = false; }     // \n acts as CR+LF (no tty driver here)
    // The cell that the most recent base code point landed in, so a following zero-width code point
    // (combining mark, VS, ZWJ part) can attach to its grapheme cluster instead of taking a column.
    bool haveBase = false;
    int baseRow = 0, baseCol = 0;
    char32_t lastCp = 0;
    // True only when the cursor still sits immediately after the last base cell we wrote (no motion,
    // CR, or wrap since) — the precondition for attaching a combiner to that grapheme cluster.
    bool atBase() const {
        return haveBase && baseRow == cr &&
               cc == baseCol + screen2(baseRow, baseCol) &&
               baseCol < static_cast<int>(rows[top + baseRow].size());
    }
    int screen2(int r, int c) const { return rows[top + r][c].width ? rows[top + r][c].width : 1; }
    void put(char32_t cp) {
        int w = cellWidthOf(cp);
        // Zero-width follower, or any code point right after a ZWJ: glue it onto the current cluster.
        if ((w == 0 || lastCp == 0x200D) && atBase()) {
            screen(baseRow)[baseCol].combine.push_back(cp);
            lastCp = cp;
            return;
        }
        // A regional-indicator following another forms one flag grapheme: attach as a combiner so the
        // pair occupies the single wide cell already written for the first indicator.
        if (cp >= 0x1F1E6 && cp <= 0x1F1FF && lastCp >= 0x1F1E6 && lastCp <= 0x1F1FF && atBase()) {
            screen(baseRow)[baseCol].combine.push_back(cp);
            lastCp = cp;
            return;
        }
        if (w == 0) w = 1;                                 // a stray combiner with no base: show it
        // Overstrike: the cursor was backspaced onto the cell `put` just wrote, and
        // we're now writing over it again. `man`/nroff use only two patterns —
        // `X \b X` → bold, `_ \b X` / `X \b _` → underline — so we merge only those
        // into one styled cell. Any other backspace-then-write (`b \b c`) is a plain
        // overwrite (last glyph wins), matching how `less` disambiguates the two.
        if (osBack && cr == osRow && cc == osCol) {
            Cell& prev = screen(cr)[cc];
            bool bold = (prev.cp == cp);
            bool under = !bold && (prev.cp == U'_' || cp == U'_');
            if (bold || under) {
                Attr a = gAttrs[prev.attr]; a.flags |= bold ? A_BOLD : A_UNDER;
                prev.cp = (cp == U'_' && prev.cp != U'_') ? prev.cp : cp;
                prev.attr = internAttr(a);
                baseRow = cr; baseCol = cc; haveBase = true; lastCp = prev.cp;
                // Candidate stays on this cell so a triple overstrike (X\bX\bX) keeps merging.
                osBack = false;
                ++cc;
                if (prev.width == 2 && cc < W) ++cc;
                return;
            }
            // fall through: plain overwrite
        }
        if (cc + w > W) { cc = 0; lfWrap(); }              // a wide char won't straddle the edge
        Cell& c = screen(cr)[cc];
        c = Cell{};
        c.cp = cp; c.attr = internAttr(pen); c.width = static_cast<uint8_t>(w);
        baseRow = cr; baseCol = cc; haveBase = true; lastCp = cp;
        osRow = cr; osCol = cc; osBack = false;            // this cell becomes the overstrike candidate
        ++cc;
        if (w == 2 && cc < W) {                            // continuation cell holds the 2nd column
            Cell& cont = screen(cr)[cc];
            cont = Cell{};
            cont.cp = 0; cont.attr = internAttr(pen); cont.width = 0;
            ++cc;
        }
    }
    void tab() { cc = std::min(W - 1, ((cc / 8) + 1) * 8); }
    void bs() { if (cc > 0) --cc; osBack = (cr == osRow && cc == osCol); }
    void up(int n) { cr = std::max(0, cr - n); }
    void down(int n) { cr = std::min(H - 1, cr + n); }
    void right(int n) { cc = std::min(W - 1, cc + n); }
    void left(int n) { cc = std::max(0, cc - n); }
    void col(int c) { cc = std::min(std::max(0, c), W - 1); }
    void row(int r) { cr = std::min(std::max(0, r), H - 1); }
    void cup(int r, int c) { row(r); col(c); }
    // DECSC/DECRC save and restore the ABSOLUTE cursor position (top + cr), not just
    // the screen-relative cr: a sixel between them advances cr via finishSixel and may
    // scrollUp() (bumping `top`), which decrc must undo too — else each DECSC/DECRC-
    // bracketed image (mdcat's table/paragraph placement) leaves `top` one row higher,
    // and every following image drifts down a row. Saving `top` makes the bracket truly
    // position-neutral, matching the producer's intent. (timg doesn't bracket its
    // sixels, so its grid protocol is unaffected.)
    void decsc() { sr = cr; sc = cc; stop = top; spen = pen; }
    void decrc() { cr = sr; cc = sc; top = stop; pen = spen; ensure(); }
    void el(int m) {                                       // erase in line
        auto& L = screen(cr);
        if (m == 1) blank(L, 0, cc + 1);
        else if (m == 2) blank(L, 0, W);
        else blank(L, cc, W);
    }
    void ed(int m) {                                       // erase in display
        if (m == 1) { for (int r = 0; r < cr; ++r) blank(screen(r), 0, W); el(1); }
        else if (m == 2) { for (int r = 0; r < H; ++r) blank(screen(r), 0, W); }
        else { el(0); for (int r = cr + 1; r < H; ++r) blank(screen(r), 0, W); }
    }

    // ---- parser ----------------------------------------------------------
    void feed(const char* p, size_t n) {
        for (size_t k = 0; k < n; ++k) {
            unsigned char b = static_cast<unsigned char>(p[k]);
            switch (st) {
                case GROUND: ground(b); break;
                case ESC: esc(b); break;
                case CSI: csi(b); break;
                case OSC: osc(b); break;
                case DCS: dcs(b); break;
                case APC: apc(b); break;
            }
        }
    }

    void ground(unsigned char b) {
        // A sixel leaves the cursor below the image; producers (timg) still emit a
        // trailing LF, which the terminal absorbs. Swallow exactly that one LF, else
        // every image after the first drifts down a row (timg climbs back by the
        // image height, not height+1).
        if (absorbLf) { absorbLf = false; if (b == '\n') return; }
        if (b == 0x1B) { st = ESC; return; }
        if (uneed > 0) {                                   // UTF-8 continuation
            if ((b & 0xC0) == 0x80) { uacc = (uacc << 6) | (b & 0x3F); if (--uneed == 0) put(uacc); }
            else { uneed = 0; put(0xFFFD); ground(b); }    // malformed: replace, reprocess
            return;
        }
        if (b < 0x20) {
            switch (b) {
                case '\n': lf(); break;
                case '\r': cc = 0; break;
                case '\t': tab(); break;
                case '\b': bs(); break;
                default: break;                            // ignore other C0
            }
            return;
        }
        if (b < 0x80) { put(b); return; }
        if ((b & 0xE0) == 0xC0) { uacc = b & 0x1F; uneed = 1; }
        else if ((b & 0xF0) == 0xE0) { uacc = b & 0x0F; uneed = 2; }
        else if ((b & 0xF8) == 0xF0) { uacc = b & 0x07; uneed = 3; }
        else put(0xFFFD);
    }

    void esc(unsigned char b) {
        switch (b) {
            case '[': st = CSI; seq.clear(); break;
            case ']': st = OSC; seq.clear(); escPend = false; break;
            case 'P': st = DCS; seq.clear(); escPend = false; break;
            case '_': st = APC; seq.clear(); escPend = false; break;   // APC (Kitty graphics)
            case '7': decsc(); st = GROUND; break;
            case '8': decrc(); st = GROUND; break;
            case 'M': up(1); st = GROUND; break;           // RI (no scroll-down for now)
            default: st = GROUND; break;                   // ignore other ESC x
        }
    }

    void csi(unsigned char b) {
        if (b >= 0x40 && b <= 0x7E) { dispatchCsi(static_cast<char>(b)); st = GROUND; return; }
        seq.push_back(static_cast<char>(b));
    }
    // OSC: accumulate until BEL or ST, then dispatch. OSC 8 sets/clears pen.link;
    // all other OSC sequences are silently consumed (text not garbled, attrs ignored).
    void osc(unsigned char b) {
        if (b == 0x07) { dispatchOsc(); st = GROUND; return; }
        if (escPend) { if (b == '\\') { dispatchOsc(); st = GROUND; return; } escPend = false; }
        if (b == 0x1B) { escPend = true; return; }
        seq.push_back(static_cast<char>(b));
    }
    void dispatchOsc() {
        // OSC 8 format: "8;<params>;<uri>"  — empty uri closes the link.
        if (seq.size() >= 2 && seq[0] == '8' && seq[1] == ';') {
            size_t semi = seq.find(';', 2);
            std::string uri = (semi != std::string::npos) ? seq.substr(semi + 1) : std::string{};
            pen.link = internUri(uri);
        }
    }
    // DCS (sixel): accumulate until ST, then decode + anchor the image.
    void dcs(unsigned char b) {
        if (escPend) { if (b == '\\') { finishSixel(); st = GROUND; return; } escPend = false; }
        if (b == 0x1B) { escPend = true; return; }
        seq.push_back(static_cast<char>(b));
    }
    // Decode the captured sixel, anchor it at the cursor, and advance the cursor to
    // the row directly below the image, column 0 (the terminal's post-sixel behaviour).
    void finishSixel() {
        Image img;
        if (!decodeSixel(seq, img)) return;
        img.row = top + cr; img.col = cc;
        img.sixel = seq;   // keep the producer's exact bytes for verbatim replay
        images.push_back(std::move(img));
        int rowsCells = (images.back().Pv + cellH - 1) / cellH;
        cc = 0;
        for (int k = 0; k < rowsCells; ++k) { if (cr + 1 >= H) scrollUp(); else ++cr; }
        absorbLf = true;   // a single trailing LF from the producer is now redundant
    }

    // APC (Kitty graphics): accumulate one chunk's payload until ST, then assemble it
    // back into a full ESC_G..ESC\ chunk in kittyAcc. A large image arrives as several
    // chunks (m=1 ... m=0); only the chunk whose `m=` is 0 or absent completes the image.
    void apc(unsigned char b) {
        if (escPend) { if (b == '\\') { finishKittyChunk(); st = GROUND; return; } escPend = false; }
        if (b == 0x1B) { escPend = true; return; }
        seq.push_back(static_cast<char>(b));
    }
    // One Kitty APC chunk (seq holds everything between ESC_ and ESC\, i.e. "G<controls>;<b64>").
    // Re-wrap it as a full chunk and append to the image accumulator. If it is the last chunk
    // (m=0 or no m= key in the controls), finalize the image; otherwise keep accumulating.
    void finishKittyChunk() {
        kittyAcc += "\033_";
        kittyAcc += seq;
        kittyAcc += "\033\\";
        // Inspect this chunk's controls (up to the first ';') for the m= flag.
        size_t semi = seq.find(';');
        std::string controls = (semi == std::string::npos) ? seq : seq.substr(0, semi);
        size_t mp = controls.find("m=");
        bool more = (mp != std::string::npos) && controls[mp + 2] == '1';
        if (!more) finishKitty();
    }
    // The image's chunks are complete in kittyAcc. Parse its id + pixel size and anchor it
    // at the cursor, advancing the cursor below it exactly as for a sixel. gmore never decodes
    // the PNG; it keeps the verbatim chunks and paints visible bands via crop placements.
    void finishKitty() {
        uint32_t id = 0; int pw = 0, ph = 0;
        if (!kittyParse(kittyAcc, id, pw, ph)) { kittyAcc.clear(); return; }
        Image img;
        img.row = top + cr; img.col = cc;
        img.Ph = pw; img.Pv = ph;
        img.kitty = kittyAcc;
        img.kid = id;
        images.push_back(std::move(img));
        kittyAcc.clear();
        int rowsCells = (ph + cellH - 1) / cellH;
        cc = 0;
        for (int k = 0; k < rowsCells; ++k) { if (cr + 1 >= H) scrollUp(); else ++cr; }
        absorbLf = true;   // a single trailing LF from the producer is now redundant
    }

    void dispatchCsi(char final) {
        std::string s = seq;
        if (!s.empty() && (s[0] == '?' || s[0] == '>' || s[0] == '<' || s[0] == '=')) s.erase(0, 1);
        std::vector<int> ps;
        { size_t i = 0; while (i <= s.size()) {
              size_t j = s.find(';', i);
              std::string t = s.substr(i, (j == std::string::npos ? s.size() : j) - i);
              ps.push_back(t.empty() ? -1 : std::atoi(t.c_str()));
              if (j == std::string::npos) break;
              i = j + 1;
          } }
        auto P = [&](size_t i, int def) { return (i < ps.size() && ps[i] >= 0) ? ps[i] : def; };
        auto P1 = [&](size_t i) { return std::max(1, P(i, 1)); };  // 0/absent -> 1
        switch (final) {
            case 'A': up(P1(0)); break;
            case 'B': down(P1(0)); break;
            case 'C': right(P1(0)); break;
            case 'D': left(P1(0)); break;
            case 'E': cc = 0; down(P1(0)); break;
            case 'F': cc = 0; up(P1(0)); break;
            case 'G': case '`': col(P1(0) - 1); break;
            case 'd': row(P1(0) - 1); break;
            case 'H': case 'f': cup(P1(0) - 1, P1(1) - 1); break;
            case 'J': ed(P(0, 0)); break;
            case 'K': el(P(0, 0)); break;
            case 'm': applySgr(ps); break;
            default: break;                                 // ignore the rest (YAGNI)
        }
    }

    void applySgr(std::vector<int>& ps) {
        if (ps.empty()) ps.push_back(0);
        for (size_t i = 0; i < ps.size(); ++i) {
            int c = ps[i] < 0 ? 0 : ps[i];
            switch (c) {
                case 0: pen = Attr{}; break;
                case 1: pen.flags |= A_BOLD; break;
                case 2: pen.flags |= A_DIM; break;
                case 3: pen.flags |= A_ITALIC; break;
                case 4: pen.flags |= A_UNDER; break;
                case 7: pen.flags |= A_INVERSE; break;
                case 22: pen.flags &= ~(A_BOLD | A_DIM); break;
                case 23: pen.flags &= ~A_ITALIC; break;
                case 24: pen.flags &= ~A_UNDER; break;
                case 27: pen.flags &= ~A_INVERSE; break;
                case 39: pen.fg = 0; break;
                case 49: pen.bg = 0; break;
                case 38: case 48: {
                    uint32_t& slot = (c == 38) ? pen.fg : pen.bg;
                    int mode = (i + 1 < ps.size()) ? ps[i + 1] : -1;
                    if (mode == 5) { slot = pal((i + 2 < ps.size() && ps[i + 2] >= 0) ? ps[i + 2] : 0); i += 2; }
                    else if (mode == 2) {
                        int r = (i + 2 < ps.size() && ps[i + 2] >= 0) ? ps[i + 2] : 0;
                        int g = (i + 3 < ps.size() && ps[i + 3] >= 0) ? ps[i + 3] : 0;
                        int bb = (i + 4 < ps.size() && ps[i + 4] >= 0) ? ps[i + 4] : 0;
                        slot = tru(r, g, bb); i += 4;
                    }
                    break;
                }
                default:
                    if (c >= 30 && c <= 37) pen.fg = pal(c - 30);
                    else if (c >= 90 && c <= 97) pen.fg = pal(c - 90 + 8);
                    else if (c >= 40 && c <= 47) pen.bg = pal(c - 40);
                    else if (c >= 100 && c <= 107) pen.bg = pal(c - 100 + 8);
                    break;
            }
        }
    }

    // Number of meaningful rows = up to the last non-blank row (trailing blanks trimmed).
    // Plain UTF-8 text of one grid row, with no SGR/links/images — what search
    // matches against. Trailing blanks are dropped; a wide-char continuation cell
    // (width 0, cp 0) contributes nothing, and each cell's combining tail is kept
    // so an accented letter or emoji sequence reads as it displays. If `cellAt` is
    // non-null it is filled so cellAt[byteOffset] = the cell index that contributed
    // the byte at that offset (one entry per byte, plus a terminating entry) — used
    // to map a regex match's byte range back to cell columns for highlighting.
    std::string rowText(size_t absRow, std::vector<int>* cellAt = nullptr) const {
        std::string out;
        if (cellAt) cellAt->clear();
        if (absRow >= rows.size()) return out;
        const std::vector<Cell>& L = rows[absRow];
        int last = -1;
        for (int i = 0; i < (int)L.size(); ++i)
            if (!(L[i].cp == U' ' || (L[i].cp == 0 && L[i].width == 0))) last = i;
        for (int i = 0; i <= last; ++i) {
            if (L[i].width == 0 && L[i].cp == 0) continue;
            size_t before = out.size();
            appendUtf8(out, L[i].cp);
            for (char32_t comb : L[i].combine) appendUtf8(out, comb);
            if (cellAt) cellAt->resize(out.size(), i);   // bytes [before,out.size()) belong to cell i
            (void)before;
        }
        if (cellAt) cellAt->push_back((int)L.size());     // terminator: maps end offset past the last cell
        return out;
    }

    // Cell-column spans [startCol, endCol) of every match of `re` in row `absRow`.
    // Byte ranges from the regex are mapped through the cellAt table so spans land
    // on whole cells (wide chars / combining marks included). Empty matches are
    // skipped (an empty span would highlight nothing and could loop).
    std::vector<std::pair<int,int>> matchSpans(size_t absRow, const std::regex& re) const {
        std::vector<std::pair<int,int>> spans;
        std::vector<int> cellAt;
        std::string text = rowText(absRow, &cellAt);
        if (text.empty()) return spans;
        auto begin = std::sregex_iterator(text.begin(), text.end(), re);
        for (auto it = begin; it != std::sregex_iterator(); ++it) {
            size_t b0 = (size_t)it->position(), len = (size_t)it->length();
            if (len == 0) continue;
            int startCol = cellAt[std::min(b0, cellAt.size() - 1)];
            int endCol = cellAt[std::min(b0 + len, cellAt.size() - 1)];   // exclusive
            if (endCol > startCol) spans.emplace_back(startCol, endCol);
        }
        return spans;
    }

    size_t contentRows() const {
        size_t last = 0;
        for (size_t r = 0; r < rows.size(); ++r) {
            for (const Cell& c : rows[r]) {
                if (!(c.cp == U' ' && c.attr == 0)) { last = r + 1; break; }
            }
        }
        for (const Image& img : images) {
            size_t imgEnd = img.row + (size_t)((img.Pv + cellH - 1) / cellH);
            if (imgEnd > last) last = imgEnd;
        }
        return last;
    }

    // Paint images as ONE sixel each, clipped to the visible row window, rather than
    // a stack of per-row strips. The old per-row scheme relied on each sixel
    // advancing the cursor exactly one cell so the next strip overwrote the prior's
    // overspill; that holds on some terminals but NOT iTerm2 (where a sixel leaves
    // the cursor on the same row — see tools/probe-cursor-after-sixel.sh), so the
    // strips tore. A single sixel renders seamlessly everywhere (its own 6px bands
    // stack internally), so we replay the producer's ORIGINAL sixel verbatim
    // (highest fidelity — see replaySixel) on the image's top row, bracketed in
    // DECSC/DECRC so the cursor is restored however the terminal moves it.
    //
    // An image paints once, on its topmost VISIBLE row: its own anchor row `img.row`
    // when that is on screen, or the window's top row `viewTop` when the image's top
    // has scrolled above the window (then we skip the off-screen leading rows). The
    // bottom is clipped to `viewBot` (absolute row just past the window; 0 = no clip,
    // used by --dump). replaySixel does the top/bottom band clipping on the original
    // bytes — both directions only drop rows, never colours.
    // Emit the visible band of an image (its [skipPx, skipPx+keepPx) pixel rows), at the cursor's
    // current position. Sixel: replay the original bytes clipped to that pixel band. Kitty: transmit
    // the image once (rewriting a=T->a=t so the transmit itself draws nothing), then display the
    // matching SOURCE crop scaled into the cells it spans — a cheap, payload-free a=p placement that
    // re-clips correctly on every scroll without re-encoding pixels.
    void paintImageBand(const Image& img, int skipPx, int keepPx, std::string& out) const {
        if (!img.isKitty()) { out += replaySixel(img, skipPx, keepPx); return; }
        if (kittyTransmitted.insert(img.kid).second)
            out += kittyTransmitOnly(img.kitty);                 // first paint: send the data once
        int cols = std::max(1, (img.Ph + cellW - 1) / cellW);    // full image width in cells
        int rows = std::max(1, (keepPx + cellH - 1) / cellH);    // visible band height in cells
        out += kittyPlace(img.kid, img.Ph, skipPx, keepPx, cols, rows);
    }

    // `withImages` inline-paints sixels (the --dump-images path); the interactive pager
    // paints images separately via paintImages() and passes false here. `withLinks`
    // re-emits OSC 8 hyperlinks; it is independent of images — only the plain-text grid
    // (--dump / nav traces) suppresses links so its output stays deterministic.
    void renderRow(size_t absRow, std::string& out, bool withImages = true,
                   size_t viewTop = 0, size_t viewBot = 0, bool withLinks = true,
                   const Highlight* hl = nullptr) const {
        if (withImages) {
            for (const Image& img : images) {
                size_t imgEnd = img.row + (size_t)((img.Pv + cellH - 1) / cellH);
                if (absRow < img.row || absRow >= imgEnd) continue;   // row not within the image
                size_t firstVisible = std::max(img.row, viewTop);
                if (absRow != firstVisible) continue;        // paint only on the top visible row
                int skipPx = (int)(firstVisible - img.row) * cellH;   // off-screen rows above
                int keepPx = img.Pv - skipPx;
                if (viewBot > firstVisible)                  // clip to the window bottom
                    keepPx = std::min(keepPx, (int)(viewBot - firstVisible) * cellH);
                out += "\0337";                              // DECSC: save cursor
                if (img.col > 0) { out += "\033["; out += std::to_string(img.col + 1); out += 'G'; }
                paintImageBand(img, skipPx, keepPx, out);
                out += "\0338";                              // DECRC: restore cursor
            }
        }

        // (Inline image painting above is kept for --dump-images, which renders a
        // single row in isolation. The interactive pager uses paintImages() instead:
        // it commits every text row first, then paints images in a second pass, so a
        // tall sixel never paints into not-yet-emitted rows — iTerm2 otherwise scrolls
        // mid-paint and clips the image's top. See run().)
        if (absRow >= rows.size()) return;
        const std::vector<Cell>& L = rows[absRow];
        int last = -1;
        for (int i = 0; i < static_cast<int>(L.size()); ++i)
            if (!((L[i].cp == U' ' || (L[i].cp == 0 && L[i].width == 0)) && L[i].attr == 0)) last = i;
        uint16_t cur = 0;
        uint32_t curLink = 0;
        auto emitOsc8 = [&](uint32_t linkId) {
            out += "\033]8;;";
            if (linkId && linkId < gUris.size()) out += gUris[linkId];
            out += "\033\\";
            curLink = linkId;
        };
        auto visualEq = [](uint16_t a, uint16_t b) {
            // Two attr indices are visually equivalent if their SGR-visible fields match.
            const Attr& A = gAttrs[a]; const Attr& B = gAttrs[b];
            return A.flags == B.flags && A.fg == B.fg && A.bg == B.bg;
        };
        int curHl = 0;   // 0 none, 1 other-match bg, 2 current-match bg
        auto emitHl = [&](int h) {
            if (h == 1) out += Highlight::OTHER;
            else if (h == 2) out += Highlight::CUR;
            else out += "\033[49m";                     // back to default background
            curHl = h;
        };
        for (int i = 0; i <= last; ++i) {
            if (L[i].width == 0 && L[i].cp == 0) continue;  // wide-char continuation: occupies no byte
            bool needSgr = withImages ? (L[i].attr != cur) : !visualEq(L[i].attr, cur);
            if (needSgr) { out += sgrFor(L[i].attr); cur = L[i].attr; curHl = 0; }  // sgrFor reset bg
            // The highlight background rides ON TOP of the cell's own SGR. Re-emit it
            // whenever the desired state differs from what's live — including right
            // after an sgrFor() above, which may have reset the background (curHl=0).
            int wantHl = hl ? hl->at(i) : 0;
            if (wantHl != curHl) emitHl(wantHl);
            // Track the link from the cell's own attr, not `cur`: visualEq leaves `cur`
            // unchanged when two attrs differ only by link, so reading gAttrs[cur].link
            // would miss link-only transitions and drop the hyperlink.
            uint32_t lnk = gAttrs[L[i].attr].link;
            if (withLinks && lnk != curLink) emitOsc8(lnk);
            appendUtf8(out, L[i].cp);
            for (char32_t comb : L[i].combine) appendUtf8(out, comb);  // grapheme's combining tail
        }
        if (withLinks && curLink != 0) emitOsc8(0);   // close any open hyperlink
        if (curHl != 0) out += "\033[49m";            // drop any open highlight background
        if (cur != 0) out += "\033[0m";
    }

    // Paint every image visible in the window [winFirst, winFirst+winRows) as a SECOND
    // pass, after the caller has already emitted all winRows text lines (so the cursor
    // sits at column 1 on the line just below the window and every image row is already
    // committed). For each image we move up to its top-visible screen line with a
    // relative CUU, save (DECSC), paint the clipped sixel, restore (DECRC), then step
    // back down — exactly mdcat's emitTable overlay. Painting into committed rows means
    // no sixel forces a mid-paint scroll, so iTerm2 never clips the image's top.
    // `winBot` is the absolute row just past the window (for bottom band-clipping).
    void paintImages(std::string& out, size_t winFirst, int winRows, size_t winBot) const {
        for (const Image& img : images) {
            size_t imgEnd = img.row + (size_t)((img.Pv + cellH - 1) / cellH);
            size_t firstVisible = std::max(img.row, winFirst);
            if (firstVisible >= imgEnd || firstVisible >= winFirst + (size_t)winRows) continue;
            int skipPx = (int)(firstVisible - img.row) * cellH;   // rows above the window
            int keepPx = img.Pv - skipPx;
            if (winBot > firstVisible)                            // clip to the window bottom
                keepPx = std::min(keepPx, (int)(winBot - firstVisible) * cellH);
            int up = winRows - (int)(firstVisible - winFirst);    // lines up from below the window
            out += "\033["; out += std::to_string(up); out += 'A';  // up to the image's top line
            out += "\033[";  out += std::to_string(img.col + 1); out += 'G';  // to its column
            out += "\0337";                                       // DECSC: save the band-top position
            paintImageBand(img, skipPx, keepPx, out);             // paint (cursor left terminal-dependent)
            out += "\0338";                                       // DECRC: back to the band-top position
            out += "\033["; out += std::to_string(up); out += 'B'; out += '\r';  // down to below the window
        }
    }
};

// ---------------------------------------------------------------------------
// Search — regex matching over the cell grid's row text, the way more(1)/less(1)
// search. The pattern is an ECMAScript regex; matching is "smart-case" like less:
// case-insensitive unless the pattern contains an uppercase letter. State (the
// compiled pattern and whether it's valid) is held so `n`/`N` can repeat it.
// Search is kept out of Nav: Nav is deliberately grid-free so its motions unit-
// test without an Emulator, whereas matching needs the grid's text.
// ---------------------------------------------------------------------------
struct Search {
    std::string pattern;     // last pattern (empty = no search yet)
    bool forward = true;     // direction of the last `/` (n repeats it, N reverses)
    std::regex re;
    bool valid = false;
    std::string error;       // set when compile fails, for the prompt
    // Position of the CURRENT match (the one n/N step from and the brighter highlight
    // tracks), as a (row, start-column) pair; {-1,-1} = none yet. A column, not just a
    // row, so n/N visit EVERY match — including several on one line. Kept apart from
    // viewTop because a match near the end clamps viewTop below its row.
    long curRow = -1;
    int  curCol = -1;

    static bool hasUpper(const std::string& s) {
        for (unsigned char ch : s) if (std::isupper(ch)) return true;
        return false;
    }

    // Compile `pat` as the active pattern searched in direction `fwd`. Returns
    // false (and sets error) if the regex is malformed; the prior pattern is kept.
    bool compile(const std::string& pat, bool fwd) {
        if (pat.empty()) return valid;   // empty /  re-uses the last pattern
        auto flags = std::regex::ECMAScript;
        if (!hasUpper(pat)) flags |= std::regex::icase;
        try {
            re = std::regex(pat, flags);
        } catch (const std::regex_error& e) {
            error = std::string("Invalid regex: ") + e.what();
            return false;
        }
        pattern = pat; forward = fwd; valid = true; error.clear();
        curRow = -1; curCol = -1;   // a new pattern searches from the current view
        return true;
    }

    // A located match: its row and the cell column it starts at.
    struct Pos { long row; int col; bool found; };

    // The next match from (fromRow, fromCol) scanning in `dir` (+1/-1), considering
    // EVERY match on every row (so several hits on one line are distinct stops), and
    // wrapping around the file like less. `fromCol < 0` (forward) / large (backward)
    // means "from the very start/end of fromRow" — used when seeding from a view top
    // with no prior column. `getSpans(row)` returns that row's match spans (sorted by
    // start column). Returns {found=false} when the pattern matches nowhere.
    template <class GetSpans>
    Pos findPos(long fromRow, int fromCol, int dir, long total, GetSpans getSpans) const {
        if (!valid || total == 0) return {-1, -1, false};
        for (long i = 0; i <= total; ++i) {     // <= total: revisit fromRow last (other cols)
            long r = ((fromRow + dir * i) % total + total) % total;
            auto spans = getSpans((size_t)r);
            if (spans.empty()) continue;
            if (dir > 0) {
                for (auto& s : spans)
                    if (i != 0 || s.first > fromCol) return {r, s.first, true};
            } else {
                for (auto it = spans.rbegin(); it != spans.rend(); ++it)
                    if (i != 0 || it->first < fromCol) return {r, it->first, true};
            }
        }
        return {-1, -1, false};
    }
};

// ---------------------------------------------------------------------------
// terminal raw-mode (restored on exit/signal)
// ---------------------------------------------------------------------------
static int gTtyFd = -1;
static struct termios gSaved;
static bool gRaw = false;
static inline void restoreTty() { if (gRaw && gTtyFd >= 0) { tcsetattr(gTtyFd, TCSANOW, &gSaved); gRaw = false; } }
static inline void onSignal(int sig) { restoreTty(); signal(sig, SIG_DFL); raise(sig); }
static inline bool enterRaw() {
    if (tcgetattr(gTtyFd, &gSaved) != 0) return false;
    struct termios raw = gSaved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    if (tcsetattr(gTtyFd, TCSANOW, &raw) != 0) return false;
    gRaw = true; return true;
}

static inline int envInt(const char* name, int def) {
    const char* v = std::getenv(name);
    if (!v) return def;
    int x = std::atoi(v);
    return x > 0 ? x : def;
}

// Terminal rows/cols (and pixel size when the kernel reports it). Tries stdout,
// then stderr/stdin, then /dev/tty — so it still works when stdout is a pipe
// (e.g. `timg … | gmore`), where ioctl on stdout fails.
static inline bool getWinsize(struct winsize& w) {
    for (int fd : {STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO})
        if (ioctl(fd, TIOCGWINSZ, &w) == 0 && w.ws_row > 0) return true;
    int t = open("/dev/tty", O_RDONLY | O_NOCTTY);
    if (t >= 0) { bool ok = ioctl(t, TIOCGWINSZ, &w) == 0 && w.ws_row > 0; close(t); if (ok) return true; }
    return false;
}

// Send `CSI <arg> t` to /dev/tty and read the reply (so it works even when stdout
// is piped, e.g. timg | gmore). Raw mode + a short inter-byte timeout keep the
// reply from being echoed/line-buffered and stop us hanging if none arrives. The
// reply is read up to its terminating 't'. Returns "" on no controlling tty / no
// answer. Shared by the cell-size queries below.
static inline std::string queryCsiT(const char* arg) {
    int fd = open("/dev/tty", O_RDWR | O_NOCTTY);
    if (fd < 0) return std::string();
    struct termios saved {};
    if (tcgetattr(fd, &saved) != 0) { close(fd); return std::string(); }
    struct termios raw = saved;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 2;  // 0.2s inter-byte timeout
    tcsetattr(fd, TCSANOW, &raw);
    std::string query = std::string("\033[") + arg + "t";
    std::string reply;
    if (write(fd, query.data(), query.size()) == static_cast<ssize_t>(query.size())) {
        char c;
        while (reply.size() < 32) {
            ssize_t n = read(fd, &c, 1);
            if (n <= 0) break;
            reply += c;
            if (c == 't') break;
        }
    }
    tcsetattr(fd, TCSANOW, &saved);
    close(fd);
    return reply;
}

// Determine the terminal's character-cell pixel size by querying it. Two methods:
//   CSI 16t -> "ESC[6;H;Wt": cell size directly (VSCode reports it only this way).
//   CSI 14t -> "ESC[4;H;Wt" (text-area px) and CSI 18t -> "ESC[8;R;Ct" (text-area
//     cells): cell = areaPx / areaCells. iTerm2 ignores 16t but answers 14t/18t.
// Needed because some terminals (VSCode, iTerm2) don't report pixels via
// TIOCGWINSZ; without a real cell size gmore's row-reservation and scroll-clip math
// (rows*cellH) would be wrong, scaling with font size — e.g. a 21px cell vs the 16
// default mis-clips a scrolled image by ~5px/row. Returns {0,0} if neither answers.
struct CellSize { int w, h; };
static inline CellSize queryCellSize() {
    // Method A: direct cell size.
    int h = 0, w = 0;
    if (std::sscanf(queryCsiT("16").c_str(), "\033[6;%d;%dt", &h, &w) == 2 && w > 0 && h > 0)
        return {w, h};
    // Method B: derive from text-area px / text-area cells.
    int ah = 0, aw = 0, rows = 0, cols = 0;
    bool gotPx   = std::sscanf(queryCsiT("14").c_str(), "\033[4;%d;%dt", &ah, &aw) == 2;
    bool gotCell = std::sscanf(queryCsiT("18").c_str(), "\033[8;%d;%dt", &rows, &cols) == 2;
    if (gotPx && gotCell && aw > 0 && ah > 0 && cols > 0 && rows > 0)
        return {aw / cols, ah / rows};
    return {0, 0};
}

// ---------------------------------------------------------------------------
// Nav — the pager's navigation state machine, factored out of run()'s I/O so
// the motion commands can be unit-tested without a tty, sixels, or RIS repaints.
// run() owns viewTop's *rendering*; Nav owns how a key (with an optional repeat
// count) moves viewTop within [0, maxTop]. dispatch() returns an action telling
// run() what to do; it never touches stdout. See --nav-trace and the
// tests/gmore-nav.sh harness, which drive Nav through GMORE_KEYS and assert the
// resulting view window in plain text.
// ---------------------------------------------------------------------------
struct Nav {
    size_t viewTop = 0;     // top visible grid row
    int pageH = 1;          // visible rows (screen height minus the prompt line)
    size_t total = 0;       // total content rows
    size_t scrollSize = 0;  // d/u half-screen step; a count to d/u sets it (0 = half page)

    size_t maxTop() const { return total > (size_t)pageH ? total - (size_t)pageH : 0; }
    bool atEnd() const { return viewTop >= maxTop(); }
    // Percent of content scrolled past the bottom of the view, like more(1).
    int percent() const {
        size_t bottom = viewTop + (size_t)pageH;
        return total ? (int)(std::min(bottom, total) * 100 / total) : 100;
    }

    // MESSAGE: don't move; run() shows an informational prompt (e.g. = position).
    // REDRAW: don't move; clear and repaint the current window (^L), to recover
    // from a corrupted screen.
    enum Action { NONE, REPAINT, QUIT, MESSAGE, REDRAW };

    // 1-based line number at the bottom of the view (the last visible line),
    // clamped to total — what more(1)'s "=" reports.
    size_t bottomLine() const { return std::min(viewTop + (size_t)pageH, total); }

    void down(size_t n) { viewTop = std::min(maxTop(), viewTop + n); }
    void up(size_t n)   { viewTop = n <= viewTop ? viewTop - n : 0; }
    // Put 1-based line `ln` at the top of the view, clamped to [0, maxTop].
    void gotoLine(size_t ln) { viewTop = std::min(maxTop(), ln ? ln - 1 : 0); }
    // d/u step: the sticky scrollSize if set, else half a page (at least 1).
    size_t scrollStep() const { return scrollSize ? scrollSize : (size_t)(pageH > 1 ? pageH / 2 : 1); }

    // Apply a command key with repeat count `count` (0 = "not given"; commands
    // pick their own default). Returns the action run() must take. Keeps the
    // more(1) quirk that space/forward at (END) quits (space) or no-ops (others).
    Action dispatch(unsigned char c, long count) {
        if (c == 'q' || c == 'Q') return QUIT;
        long n = count > 0 ? count : 0;
        bool fwd = (c == ' ' || c == 'f' || c == 'j' || c == '\r' || c == '\n');
        if (atEnd() && fwd) return c == ' ' ? QUIT : NONE;
        switch (c) {
            case ' ': case 'f': down(n > 0 ? (size_t)n : (size_t)pageH); return REPAINT;
            case 'b':           up(n > 0 ? (size_t)n : (size_t)pageH);   return REPAINT;
            case '\r': case '\n': case 'j': down(n > 0 ? (size_t)n : 1); return REPAINT;
            case 'k': case 'y': up(n > 0 ? (size_t)n : 1);               return REPAINT;
            // g/G: go to line N (1-based); default g=first line, G=last line.
            case 'g': gotoLine(n > 0 ? (size_t)n : 1);              return REPAINT;
            case 'G': gotoLine(n > 0 ? (size_t)n : total);         return REPAINT;
            // d/^D, u/^U: scroll half a screen; a count sets the step and sticks,
            // like more(1). Default step is half the page height (min 1).
            case 'd': case 0x04: if (n > 0) scrollSize = (size_t)n; down(scrollStep()); return REPAINT;
            case 'u': case 0x15: if (n > 0) scrollSize = (size_t)n; up(scrollStep());   return REPAINT;
            // =/^G: report position (line number + percent) without moving.
            case '=': case 0x07: return MESSAGE;
            // ^L: clear and repaint the current screen without moving.
            case 0x0C: return REDRAW;
            default: return NONE;
        }
    }
};

// ---------------------------------------------------------------------------
// run() — the gmore pager entry point. Accepts already-read input data.
// Returns 0 on success. When stdout is not a tty and neither dump nor imginfo
// mode is requested, passes the data through verbatim (pager-as-cat).
// ---------------------------------------------------------------------------
static inline int run(std::string data, bool dump = false, bool dumpImages = false, bool imginfo = false,
                      bool navTrace = false) {
    internAttr(Attr{});  // id 0 = default

    // Terminal geometry. Env wins (for --dump/tests); then the kernel's TIOCGWINSZ
    // (rows/cols, and pixel size if present); then CSI 16t for the cell pixel size
    // (VSCode reports it only this way); then defaults. Size is read from /dev/tty/
    // stderr too, so it's correct even when stdout is a pipe (timg | gmore).
    int H = envInt("LINES", 0), W = envInt("COLUMNS", 0);
    int cellW = envInt("GMORE_CELLW", 0), cellH = envInt("GMORE_CELLH", 0);
    struct winsize ws;
    if (getWinsize(ws)) {
        if (!H) H = ws.ws_row;
        if (!W) W = ws.ws_col;
        if (!cellW && ws.ws_xpixel > 0 && ws.ws_col > 0) cellW = ws.ws_xpixel / ws.ws_col;
        if (!cellH && ws.ws_ypixel > 0 && ws.ws_row > 0) cellH = ws.ws_ypixel / ws.ws_row;
    }
    if (!cellW || !cellH) {
        CellSize cs = queryCellSize();
        if (cs.w > 0 && cs.h > 0) { if (!cellW) cellW = cs.w; if (!cellH) cellH = cs.h; }
    }
    if (!H) H = 24;
    if (!W) W = 80;
    if (!cellW) cellW = 8;
    if (!cellH) cellH = 16;
    if (std::getenv("GMORE_DEBUG"))
        std::fprintf(stderr, "[gmore] geometry W=%d H=%d cell=%dx%d\n", W, H, cellW, cellH);

    // Not a terminal and not --dump/--imginfo: pass through verbatim (pager-as-cat).
    // GMORE_KEYS forces the pager path even off-tty so a scripted session can be
    // captured to a file for inspection.
    if (!isatty(STDOUT_FILENO) && !dump && !imginfo && !std::getenv("GMORE_KEYS")) {
        fwrite(data.data(), 1, data.size(), stdout);
        return 0;
    }

    Emulator em(W, H, cellW, cellH);
    em.feed(data.data(), data.size());
    const size_t total = em.contentRows();

    if (imginfo) {
        for (size_t k = 0; k < em.images.size(); ++k) {
            const Image& I = em.images[k];
            int cols = (I.Ph + cellW - 1) / cellW, rws = (I.Pv + cellH - 1) / cellH;
            std::printf("image %zu @%zu,%d %dx%dpx %dx%dcells\n", k + 1, I.row, I.col, I.Ph, I.Pv, cols, rws);
            if (I.Ph <= 40 && I.Pv <= 40) {       // small enough to show as ASCII
                for (int y = 0; y < I.Pv; ++y) {
                    std::string line;
                    for (int x = 0; x < I.Ph; ++x) line += I.px[(size_t)y * I.Ph + x] ? '#' : '.';
                    std::printf("%s\n", line.c_str());
                }
            }
        }
        return 0;
    }

    // --dump is the text-grid test surface; suppress images so tests stay deterministic.
    // vTop/vBot give the visible row window so an image paints one sixel clipped to it
    // (vBot==0 => no clip: paint the whole image at its anchor, used by --dump-images).
    auto emitRow = [&](size_t r, size_t vTop = 0, size_t vBot = 0) {
        std::string s; em.renderRow(r, s, true, vTop, vBot); fwrite(s.data(), 1, s.size(), stdout); };
    auto emitRowText = [&](size_t r) {
        std::string s; em.renderRow(r, s, false, 0, 0, /*withLinks=*/false);
        fwrite(s.data(), 1, s.size(), stdout); };

    if (dump) {
        for (size_t r = 0; r < total; ++r) {
            if (dumpImages) emitRow(r); else emitRowText(r);
            std::fputc('\n', stdout);
        }
        return 0;
    }

    // --nav-trace is the navigation test surface: replay the GMORE_KEYS script
    // through Nav (no tty, no painting) and print the final view window as plain
    // text — "top=R bottom=B total=T pct=P% END?" — so motion commands can be
    // asserted deterministically. Counts are leading digits before a command.
    // Search is driven here too against `em`'s grid: `/`/`?` read a pattern up to
    // the next newline, `n`/`N` repeat it; `notfound`/`badre` is appended when a
    // search fails so those paths are assertable. Mirrors run()'s search handling.
    if (navTrace) {
        Nav t;
        int H2 = H ? H : 24;
        t.pageH = H2 - 1;
        t.total = total;
        Search search;
        const char* note = "";
        auto doSearch = [&](int dir) {
            if (!search.valid) { note = " notfound"; return; }
            long fromRow = search.curRow >= 0 ? search.curRow : (long)t.viewTop;
            int  fromCol = search.curRow >= 0 ? search.curCol : (dir > 0 ? -1 : INT_MAX);
            Search::Pos p = search.findPos(fromRow, fromCol, dir, (long)total,
                                           [&](size_t r) { return em.matchSpans(r, search.re); });
            if (!p.found) { note = " notfound"; return; }
            search.curRow = p.row; search.curCol = p.col;
            t.gotoLine((size_t)p.row + 1);
        };
        const char* keys = std::getenv("GMORE_KEYS");
        long count = 0;
        for (const char* p = keys; p && *p; ++p) {
            unsigned char c = (unsigned char)*p;
            if (c == '/' || c == '?') {                  // read pattern up to newline
                std::string pat;
                while (*++p && *p != '\n') pat.push_back(*p);
                bool fwd = (c == '/');
                if (search.compile(pat, fwd)) doSearch(fwd ? +1 : -1);
                else note = " badre";
                if (!*p) break;                          // newline consumed by ++p above
                continue;
            }
            if (c == 'n' || c == 'N') {
                int dir = search.forward ? +1 : -1;
                if (c == 'N') dir = -dir;
                doSearch(dir);
                continue;
            }
            if (c >= '0' && c <= '9') {
                if (count <= (long)total) count = count * 10 + (c - '0');
                continue;
            }
            Nav::Action a = t.dispatch(c, count);
            count = 0;
            if (a == Nav::QUIT) break;
        }
        std::printf("top=%zu bottom=%zu total=%zu pct=%d%% %s%s\n",
                    t.viewTop, std::min(t.viewTop + (size_t)t.pageH, t.total),
                    t.total, t.percent(), t.atEnd() ? "END" : "more", note);
        return 0;
    }

    // Scroll-safety for the first paint. gmore models a clean grid from row 0, but the
    // REAL terminal may start with a non-blank screen and the cursor near the bottom.
    // Painting top-down from there makes each tall sixel force the terminal to scroll to
    // make room — and a scroll mid-row invalidates the DECSC/DECRC anchors, so the
    // second and later images in a table row cascade down a row each (the reported bug;
    // a later page-up/down repaints via RIS and fixes it). The cure is the same trick
    // mdcat's emitImageParagraph uses: print `n` newlines first to reserve the band
    // (this scrolls any existing content into scrollback, guaranteeing room below), then
    // move the cursor back up `n` rows (relative, so the scroll doesn't matter) to the
    // top of the freshly-cleared region, and paint there. No row emission then forces a
    // scroll, so every sixel anchors correctly — without clearing the screen (RIS), which
    // would be wrong for short output that doesn't need paging.
    auto reserveRows = [&](size_t n) {
        if (n == 0) return;
        for (size_t i = 0; i < n; ++i) std::fputc('\n', stdout);
        std::fprintf(stdout, "\033[%zuA", n);
    };

    // Search state (the active pattern + the row of the current match). Declared here,
    // before paintWindow, so paintWindow can highlight matches against search.re.
    Search search;

    // Per-row search-match highlight for `absRow`: the cell-column spans matching the
    // active pattern, with the exact current match (search.curRow/curCol) flagged as
    // the "current" one (more saturated blue). Matching by COLUMN, not just row, so the
    // brighter highlight lands on the precise match n/N is on — even with several
    // matches on one line. Returns an empty Highlight when there is no active search,
    // so non-search rendering is byte-for-byte unchanged.
    auto rowHighlight = [&](size_t absRow) -> Highlight {
        Highlight h;
        if (!search.valid) return h;
        h.spans = em.matchSpans(absRow, search.re);
        if ((long)absRow == search.curRow)
            for (size_t k = 0; k < h.spans.size(); ++k)
                if (h.spans[k].first == search.curCol) { h.current = (int)k; break; }
        return h;
    };

    // Emit rows [first, first+count) the safe two-pass way: all text first (committing
    // every line), then images painted into those committed rows (see paintImages).
    // Leaves the cursor on the line just below the window. vBot is the absolute row past
    // the window for bottom-clipping (0 => clip to first+count, the whole window).
    auto paintWindow = [&](size_t first, int count, size_t vBot) {
        std::string s;
        for (int i = 0; i < count; ++i) {
            size_t r = first + (size_t)i;
            if (r < total) {
                Highlight hl = rowHighlight(r);
                em.renderRow(r, s, /*withImages=*/false, 0, 0, /*withLinks=*/true,
                             hl.empty() ? nullptr : &hl);
            }
            s += '\n';
        }
        em.paintImages(s, first, count, vBot ? vBot : first + (size_t)count);
        fwrite(s.data(), 1, s.size(), stdout);
    };

    // Fits on one screen: print and exit, no prompt.
    if (total <= static_cast<size_t>(H - 1)) {
        reserveRows(total);
        paintWindow(0, (int)total, total);
        return 0;
    }

    // GMORE_KEYS replays a scripted key sequence with no tty (output can be a
    // pipe/file) — skip raw mode entirely in that case.
    const bool scripted = std::getenv("GMORE_KEYS") != nullptr;
    if (!scripted) {
        gTtyFd = open("/dev/tty", O_RDWR);
        if (gTtyFd < 0) gTtyFd = STDIN_FILENO;
        if (!enterRaw()) {
            paintWindow(0, (int)total, total);
            return 0;
        }
        std::atexit(restoreTty);
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
    }

    // view = rows [viewTop, viewTop + pageH); pageH = H-1 (bottom row = prompt).
    // Navigation lives in Nav (see above); viewTop/pageH are references into it so
    // the existing paint code reads unchanged.
    Nav nav;
    nav.pageH = H - 1;
    nav.total = total;
    int& pageH = nav.pageH;
    size_t& viewTop = nav.viewTop;

    // Observability: GMORE_DEBUG logs paint operations to stderr (which absolute
    // grid rows we paint onto which screen lines, and where image strips land), so
    // the scroll logic can be reasoned about without a sixel-capable tty.
    const bool dbg = std::getenv("GMORE_DEBUG") != nullptr;
    auto trace = [&](const char* what) {
        if (!dbg) return;
        std::fprintf(stderr, "[gmore] %-10s viewTop=%zu pageH=%d total=%zu\n",
                     what, viewTop, pageH, total);
    };
    // For an image-bearing absolute row, report which screen line it's being
    // painted on and the image's anchor row — a mismatch is the drift bug.
    auto traceRow = [&](size_t absRow, int screenLine) {
        if (!dbg) return;
        for (size_t k = 0; k < em.images.size(); ++k) {
            const Image& I = em.images[k];
            size_t imgEnd = I.row + (size_t)((I.Pv + cellH - 1) / cellH);
            if (absRow >= I.row && absRow < imgEnd)
                std::fprintf(stderr, "[gmore]   row=%zu -> screenLine=%d  img%zu anchorRow=%zu strip=%zu\n",
                             absRow, screenLine, k, I.row, absRow - I.row);
        }
    };

    // A transient status message (e.g. the = position report) shown on the prompt
    // row in place of --More-- until the next keystroke, then cleared.
    std::string message;
    auto showPrompt = [&] {
        if (!message.empty()) std::fprintf(stdout, "\033[7m%s\033[27m", message.c_str());
        else if (nav.atEnd()) std::fputs("\033[7m(END)\033[27m", stdout);
        else std::fprintf(stdout, "\033[7m--More--(%d%%)\033[27m", nav.percent());
        std::fflush(stdout);
    };
    auto clearPrompt = [&] { std::fputs("\r\033[K", stdout); };

    // Help overlay (the `h` command): RIS-clear and list the key bindings, leaving
    // a prompt at the bottom. Any key dismisses it (the caller repaints after).
    auto showHelp = [&] {
        static const char* lines[] = {
            "gmore — key commands",
            "",
            "  space, f       forward one screen (N: N lines)",
            "  b              backward one screen",
            "  Enter, j       forward one line (N: N lines)",
            "  k, y           backward one line",
            "  d, ^D          forward half screen (N sets the step)",
            "  u, ^U          backward half screen",
            "  g / G          go to first / last line (N: line N)",
            "  /pattern       search forward (regex, smart-case)",
            "  ?pattern       search backward",
            "  n / N          repeat search / in reverse",
            "  =, ^G          show position (line / total / %)",
            "  ^L             clear and repaint the screen",
            "  h              this help",
            "  q, Q           quit",
            "",
        };
        std::fputs("\033c", stdout);                      // RIS: clear like repaint()
        for (const char* l : lines) std::fprintf(stdout, "%s\r\n", l);
        std::fputs("\033[7mPress any key to continue\033[27m", stdout);
        std::fflush(stdout);
    };

    // Initial paint: fill the first page. paintWindow commits all text rows first, then
    // paints images into them (a tall sixel painted before its rows exist would scroll
    // the terminal mid-paint and clip the image's top — see paintImages).
    size_t initEnd = std::min(total, (size_t)pageH);
    trace("init");
    reserveRows((size_t)pageH);   // make room below so no sixel forces a mid-paint scroll
    for (size_t r = 0; r < initEnd; ++r) traceRow(r, (int)r);
    paintWindow(0, (int)initEnd, initEnd);
    viewTop = initEnd < (size_t)pageH ? 0 : initEnd - (size_t)pageH;

    // Repaint the whole visible window for the current viewTop. Both scroll
    // directions use this: with whole-sixel image painting (one clipped sixel per
    // image, not a per-row strip stack — see renderRow) an incremental scroll can't
    // cleanly relocate a sixel whose top has moved off-screen, so we full-repaint
    // every move. Each row's image (if any) paints once, clipped to [viewTop, vBot).
    //
    // We wipe with a FULL RESET (ESC c / RIS), not ESC[2J. On VSCode's terminal
    // ESC[2J erases text cells but NOT a relocated sixel raster, so scrolled-off
    // images ghosted at the top; ESC[3J, soft reset (ESC[!p), cell-overwrite and an
    // opaque cover sixel all left artifacts. Only RIS fully clears the raster
    // (verified live). gmore relies on no terminal state RIS disturbs — it re-emits
    // every SGR/color per row, uses no scroll region or custom charset, and raw mode
    // is kernel-side (untouched).
    auto repaint = [&] {
        size_t vBot = std::min(total, viewTop + (size_t)pageH);
        std::fputs("\033c", stdout);
        for (int i = 0; i < pageH; ++i) traceRow(viewTop + (size_t)i, i);
        paintWindow(viewTop, pageH, vBot);
    };

    // Forward-only incremental paint that PRESERVES SCROLLBACK. When the view moves
    // strictly down by `delta` rows (a contiguous forward shift), we don't reset the
    // terminal — we just emit the `delta` newly-revealed rows at the bottom. The prompt
    // was already erased (clearPrompt) leaving the cursor at column 0 of the old prompt
    // row, which sits one line below the old window's last text row; emitting rows there
    // scrolls the terminal up naturally, pushing the rows that leave the top of the view
    // into the terminal's scrollback (exactly how more(1)/cat behave). Because no RIS
    // (\033c) is issued, VSCode keeps the scrollback the user wants to keep.
    //
    // The cost vs repaint(): an image whose top has scrolled above the new view can't be
    // re-clipped (paintImages anchors whole sixels), so forward-append is only used while
    // images aren't being split across the top fold — see the call site, which falls back
    // to repaint() for any non-contiguous or backward move. A clean text/forward page is
    // the common case and now leaves history intact.
    auto advance = [&](size_t prevTop) {
        size_t delta = viewTop - prevTop;             // caller guarantees viewTop > prevTop
        size_t firstNew = prevTop + (size_t)pageH;    // first row not previously visible
        size_t vBot = std::min(total, viewTop + (size_t)pageH);
        for (size_t i = 0; i < delta; ++i) traceRow(firstNew + i, pageH - (int)delta + (int)i);
        paintWindow(firstNew, (int)delta, vBot);
    };

    // Paint after the view moved from `prevTop` to the current viewTop, picking the
    // cheapest correct strategy. advance() (scrollback-preserving incremental append)
    // is ONLY valid for a CONTIGUOUS forward move — delta <= pageH. At delta < pageH
    // the new window overlaps the old; at delta == pageH it abuts it (the normal
    // full-page scroll). Either way advance() appends exactly the newly-revealed rows
    // below the screen and the terminal scrolls the rest into scrollback, leaving every
    // on-screen sixel/text cell valid. A backward move, no move, OR a forward JUMP
    // (delta > pageH: g/G, a big count, search to a distant line) leaves a GAP — the old
    // screen has no row in common with the new one, so its sixels would linger (the
    // reported bug). Those must full-repaint via RIS, which wipes the screen.
    auto paintMove = [&](size_t prevTop) {
        if (viewTop > prevTop && viewTop - prevTop <= (size_t)pageH) advance(prevTop);
        else if (viewTop != prevTop) repaint();
        // viewTop == prevTop: clamped at an edge, nothing moved — leave the screen.
    };

    // GMORE_KEYS scripts the keystroke stream (one char = one key) instead of
    // reading the tty, so a session like "jk" can be replayed non-interactively
    // and its exact output (escapes + sixel strips) captured for inspection.
    // Motion logic lives in Nav::dispatch; this loop only does I/O and accumulates
    // a leading decimal repeat count (more(1)'s "10j", etc.).
    const char* scriptedKeys = std::getenv("GMORE_KEYS");
    size_t keyPos = 0;

    // One keystroke from the scripted stream or the tty; returns false at EOF so
    // both the main loop and the search input editor share one read path.
    auto nextKey = [&](unsigned char& c) -> bool {
        if (scriptedKeys) {
            if (!scriptedKeys[keyPos]) return false;
            c = (unsigned char)scriptedKeys[keyPos++];
            return true;
        }
        return read(gTtyFd, &c, 1) == 1;
    };

    // Read a pattern on the prompt row after the leading `/`, echoing as typed.
    // Returns true with `out` set on Enter; false if cancelled (ESC) or EOF.
    // Backspace edits; an empty Enter returns true with out empty (reuse last).
    auto readLine = [&](char lead, std::string& out) -> bool {
        out.clear();
        auto draw = [&] {
            std::fputs("\r\033[K", stdout);
            std::fputc(lead, stdout);
            fwrite(out.data(), 1, out.size(), stdout);
            std::fflush(stdout);
        };
        draw();
        for (;;) {
            unsigned char c;
            if (!nextKey(c)) return false;
            if (c == '\r' || c == '\n') return true;
            if (c == 0x1b) return false;                 // ESC cancels
            if (c == 0x7f || c == 0x08) {                // backspace
                if (!out.empty()) {
                    // Drop a whole UTF-8 char (its trailing continuation bytes too).
                    do { out.pop_back(); } while (!out.empty() && (out.back() & 0xc0) == 0x80);
                    draw();
                }
                continue;
            }
            if (c >= 0x20 || c >= 0x80) { out.push_back((char)c); draw(); }
        }
    };

    // Run a search: advance to the next match in `dir` (+1 forward / -1 backward) and
    // scroll its row to the top of the view. Sets `message` on not-found / no-prior.
    // Returns true if a match was found (the caller repaints — the current-match
    // highlight moves even when the view itself doesn't, e.g. a later match on the
    // same row). Steps from the current match's exact (row,col); see findPos.
    auto runSearch = [&](int dir) -> bool {
        if (!search.valid) { message = "No previous search"; return false; }
        // Step from the current match's exact (row,col) so n/N visit every match,
        // including several on one line. With no prior match, seed from viewTop's edge
        // (col -1 forward = before the first; INT_MAX backward = after the last) so the
        // first hit is the nearest match from the current view.
        long fromRow = search.curRow >= 0 ? search.curRow : (long)viewTop;
        int  fromCol = search.curRow >= 0 ? search.curCol : (dir > 0 ? -1 : INT_MAX);
        Search::Pos p = search.findPos(fromRow, fromCol, dir, (long)total,
                                       [&](size_t r) { return em.matchSpans(r, search.re); });
        if (!p.found) { message = "Pattern not found"; return false; }
        search.curRow = p.row; search.curCol = p.col;
        nav.gotoLine((size_t)p.row + 1);
        return true;   // a match was found; caller repaints (the current highlight moved
                       // even when the view itself didn't — e.g. next match on the same row)
    };

    long count = 0;
    bool counting = false;   // mid-count: don't reprint the prompt between digits
    for (;;) {
        if (!counting) showPrompt();
        unsigned char c;
        if (!nextKey(c)) break;
        // Search and help are handled here, before Nav: they need the grid text
        // (search) or take over the prompt row (input editor / overlay), which Nav
        // is deliberately ignorant of.
        if (c == '/' || c == '?') {
            std::string pat;
            bool ok = readLine(c, pat);
            clearPrompt();
            message.clear();
            count = 0; counting = false;
            if (!ok) continue;                            // cancelled — leave the view
            bool fwd = (c == '/');
            if (!search.compile(pat, fwd)) { message = search.error; continue; }
            runSearch(fwd ? +1 : -1);
            // Always full-repaint after a search: the highlight must be applied across
            // the WHOLE visible window (including matches already on screen), which the
            // incremental advance() path can't do — it only paints newly-revealed rows.
            repaint();
            continue;
        }
        if (c == 'n' || c == 'N') {
            clearPrompt();
            message.clear();
            count = 0; counting = false;
            // n repeats in the search's direction; N reverses it.
            int dir = search.forward ? +1 : -1;
            if (c == 'N') dir = -dir;
            if (runSearch(dir)) repaint();   // repaint to (re)highlight the visible window
            continue;
        }
        if (c == 'h') {
            clearPrompt();
            showHelp();
            unsigned char k; nextKey(k);                  // any key dismisses
            repaint();
            count = 0; counting = false; message.clear();
            continue;
        }
        if (c >= '0' && c <= '9') {
            // Clamp so a long digit run can't overflow; no motion exceeds `total`.
            if (count <= (long)total) count = count * 10 + (c - '0');
            counting = true;
            continue;
        }
        counting = false;
        clearPrompt();
        message.clear();
        size_t prevTop = viewTop;
        Nav::Action a = nav.dispatch(c, count);
        count = 0;
        if (a == Nav::QUIT) break;
        if (a == Nav::REPAINT) {
            trace("dispatch");
            // A contiguous forward step appends the newly-revealed rows below the screen,
            // scrolling old content into scrollback (preserving history). A forward jump
            // (g/G, a big count), any backward move, or a no-op falls back to RIS full
            // repaint, which relocates images correctly but resets the screen. See
            // paintMove for why a non-overlapping forward move can't use advance().
            paintMove(prevTop);
        }
        if (a == Nav::REDRAW) { trace("redraw"); repaint(); }
        if (a == Nav::MESSAGE) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "line %zu/%zu (%d%%)",
                          nav.bottomLine(), nav.total, nav.percent());
            message = buf;
        }
    }
    clearPrompt();
    std::fflush(stdout);
    return 0;
}

}  // namespace gmore
