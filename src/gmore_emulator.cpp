#include "gmore_emulator.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "gmore_attrs.h"

namespace gmore {

static bool decodeSixel(const std::string& s, Image& img) {
    size_t i = 0;
    while (i < s.size() && s[i] != 'q') ++i;  // skip intro params P1;P2;P3
    if (i >= s.size()) return false;
    ++i;  // past 'q'

    auto readNums = [&](std::vector<int>& v) {
        std::string n;
        while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == ';')) {
            if (s[i] == ';') {
                v.push_back(n.empty() ? 0 : std::atoi(n.c_str()));
                n.clear();
            } else
                n += s[i];
            ++i;
        }
        if (!n.empty()) v.push_back(std::atoi(n.c_str()));
    };

    int Ph = 0, Pv = 0;
    std::unordered_map<int, uint32_t> pal;
    int cur = 0, x = 0, band = 0, maxX = 0;
    std::vector<std::vector<uint32_t>> g;  // g[pixelRow][x]
    auto colorOf = [&](int idx) {
        auto it = pal.find(idx);
        return it != pal.end() ? it->second : 0xFF000000u;
    };
    auto setpx = [&](int px_x, int px_y, uint32_t c) {
        if ((size_t)px_y >= g.size()) g.resize(px_y + 1);
        auto& r = g[px_y];
        if ((size_t)px_x >= r.size()) r.resize(px_x + 1, 0);
        r[px_x] = c;
    };
    auto plot = [&](int bits, int n) {
        for (int r = 0; r < n; ++r) {
            for (int b = 0; b < 6; ++b)
                if (bits & (1 << b)) setpx(x, band * 6 + b, colorOf(cur));
            ++x;
        }
        if (x > maxX) maxX = x;
    };

    while (i < s.size()) {
        char c = s[i];
        if (c == '"') {
            ++i;
            std::vector<int> v;
            readNums(v);
            if (v.size() >= 4) {
                Ph = v[2];
                Pv = v[3];
            }
            continue;
        }
        if (c == '#') {
            ++i;
            std::vector<int> v;
            readNums(v);
            if (v.size() == 1)
                cur = v[0];
            else if (v.size() >= 5) {
                int idx = v[0], pu = v[1];
                uint32_t rgb = (pu == 2) ? (0xFF000000u | ((v[2] * 255 / 100) << 16) |
                                            ((v[3] * 255 / 100) << 8) | (v[4] * 255 / 100))
                                         : 0xFF808080u;  // HLS: approximate (our sources use RGB)
                pal[idx] = rgb;
                cur = idx;
            }
            continue;
        }
        if (c == '!') {
            ++i;
            std::string n;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) n += s[i++];
            int cnt = std::max(1, std::atoi(n.c_str()));
            if (i < s.size() && s[i] >= '?' && s[i] <= '~') plot(s[i++] - '?', cnt);
            continue;
        }
        if (c == '$') {
            x = 0;
            ++i;
            continue;
        }
        if (c == '-') {
            x = 0;
            ++band;
            ++i;
            continue;
        }
        if (c >= '?' && c <= '~') {
            plot(c - '?', 1);
            ++i;
            continue;
        }
        ++i;  // ignore stray bytes (whitespace, etc.)
    }

    if (Ph <= 0) Ph = maxX;
    if (Pv <= 0) Pv = (int)g.size();
    if (Ph <= 0 || Pv <= 0) return false;
    img.Ph = Ph;
    img.Pv = Pv;
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
    out += '"';
    out += "1;1;";
    out += std::to_string(img.Ph);
    out += ';';
    out += std::to_string(h);
    out += '\n';

    // Walk sixel bands (6 px each)
    for (int band = 0; y0 + band * 6 < y1; ++band) {
        int py0 = y0 + band * 6;          // first pixel row of this band
        int py1 = std::min(py0 + 6, y1);  // exclusive

        // Collect unique colours in this band (in encounter order, index 0…)
        std::vector<uint32_t> palette;  // palette[n] = 0xFFrrggbb
        std::unordered_map<uint32_t, int> idx;
        for (int py = py0; py < py1; ++py) {
            for (int x = 0; x < img.Ph; ++x) {
                uint32_t c = img.px[(size_t)py * img.Ph + x];
                if (c && !idx.count(c)) {
                    idx[c] = (int)palette.size();
                    palette.push_back(c);
                }
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
            out += '#';
            out += std::to_string(n);
            out += ";2;";
            out += std::to_string(r);
            out += ';';
            out += std::to_string(g);
            out += ';';
            out += std::to_string(b);

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
            out += '#';
            out += std::to_string(n);  // select colour register
            int x = 0;
            while (x < img.Ph) {
                unsigned char byte = (unsigned char)('?' + bits[x]);
                int run = 1;
                while (x + run < img.Ph && bits[x + run] == bits[x]) ++run;
                if (run >= 5) {
                    out += '!';
                    out += std::to_string(run);
                    out += (char)byte;
                } else {
                    for (int k = 0; k < run; ++k) out += (char)byte;
                }
                x += run;
            }

            if (n + 1 < (int)palette.size()) out += '$';  // CR: more colours on same band
            (void)first_color;
            first_color = false;
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
    int skipBands = (skipPx > 0) ? skipPx / 6 : 0;  // align top clip to a band boundary
    int alignedSkip = skipBands * 6;
    int avail = img.Pv - alignedSkip;
    if (avail <= 0) return std::string();  // nothing visible
    int outPv = (keepPx > 0 && keepPx < avail) ? keepPx : avail;

    if (s.empty())
        return encodeSixel(img, alignedSkip, alignedSkip + outPv);  // no bytes: re-encode

    // Whole image, verbatim — the common case (image fits the window).
    if (skipBands == 0 && outPv >= img.Pv) {
        return "\033P" + s + "\033\\";
    }

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
        if (seen == skipBands) firstKept = i + 1;  // band `skipBands` begins here
        if (seen == skipBands + keepBands) {
            cut = i;
            break;
        }
    }

    std::string out = "\033P";
    out += s.substr(0, quote + 1);  // through the opening '"'
    out += std::to_string(pan);
    out += ';';
    out += std::to_string(pad);
    out += ';';
    out += std::to_string(ph);
    out += ';';
    out += std::to_string(outPv);
    if (skipBands == 0) {
        out += s.substr(p, cut - p);  // colour defs + kept bands (from top)
    } else {
        // Keep the up-front colour defs, then the kept bands. Extract just the
        // `#n;2;R;G;B` defines from the pre-first-band segment (skip band-0 pixels).
        std::string seg = s.substr(p, firstDash - p);
        for (size_t i = 0; i < seg.size();) {
            if (seg[i] == '#') {
                size_t j = i + 1;
                int semis = 0;
                while (j < seg.size() && (std::isdigit((unsigned char)seg[j]) || seg[j] == ';')) {
                    if (seg[j] == ';') ++semis;
                    ++j;
                }
                if (semis >= 3) out += seg.substr(i, j - i);  // a #n;Pu;R;G;B colour define
                i = j;
            } else
                ++i;
        }
        out += s.substr(firstKept, cut - firstKept);  // then the kept bands
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
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
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
    w = u32(16);
    h = u32(20);
    return w > 0 && h > 0;
}

// Parse a Kitty APC transmission `apc` (ESC _ G <controls>;<b64> ESC \ ... chunks) for
// its image id (i=) and pixel size (from the first chunk's PNG IHDR). Returns false if
// it is not a recoverable Kitty image transmission.
// Read the value of a `key=<int>` from a comma-separated Kitty controls run; 0 if absent.
static int kittyCtrlInt(const std::string& controls, const char* key) {
    std::string k = key;
    for (size_t p = 0; (p = controls.find(k, p)) != std::string::npos; p += k.size()) {
        // Match only at a key boundary (start of run or just after a comma) so "c=" does not
        // hit the 'c' inside another value.
        if (p == 0 || controls[p - 1] == ',') return std::atoi(controls.c_str() + p + k.size());
    }
    return 0;
}

// Parse a captured Kitty APC: its image id, the inner PNG's pixel size (from the IHDR), and the
// cell FOOTPRINT (c=/r=) the producer requested. mdcat injects c=/r= matching the cells it laid the
// image out in (e.g. a table column width); gmore must honour that footprint rather than
// re-deriving one from the pixel size, or a width-bound table image paints at its native width and
// overflows its column. footCols/footRows are 0 if the producer set no c=/r= (then the caller falls
// back to pixels).
static bool kittyParse(
    const std::string& apc, uint32_t& id, int& pw, int& ph, int& footCols, int& footRows) {
    size_t g = apc.find("\033_G");
    if (g == std::string::npos) return false;
    size_t semi = apc.find(';', g);
    if (semi == std::string::npos) return false;
    std::string controls = apc.substr(g + 3, semi - (g + 3));
    size_t ip = controls.find("i=");
    id =
        (ip != std::string::npos) ? static_cast<uint32_t>(std::atoi(controls.c_str() + ip + 2)) : 0;
    footCols = kittyCtrlInt(controls, "c=");
    footRows = kittyCtrlInt(controls, "r=");
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
    out += ",q=2,x=0,y=";
    out += std::to_string(skipPx);
    out += ",w=";
    out += std::to_string(pw);
    out += ",h=";
    out += std::to_string(keepPx);
    out += ",c=";
    out += std::to_string(cols);
    out += ",r=";
    out += std::to_string(rows);
    out += "\033\\";
    return out;
}

// ---------------------------------------------------------------------------
// The emulator: a cell grid (rows grow downward = scrollback) with a screen
// viewport [top, top+H) in which the cursor lives, fed bytes by a state machine.
// ---------------------------------------------------------------------------
struct Emulator::Impl {
    int W, H;
    int cellW, cellH;  // pixels per cell (for sizing sixel images)
    std::vector<std::vector<Cell>> rows;
    std::vector<Image> images;
    // Kitty image ids already transmitted to the terminal this session. A Kitty image is sent
    // ONCE (its data chunks), then displayed by id with cheap crop placements on every (re)paint
    // and scroll. Mutable because painting is logically const but must remember what it has sent.
    mutable std::set<uint32_t> kittyTransmitted;
    // RIS (ESC c, the full repaint wipe) makes the terminal forget every transmitted
    // Kitty image, so the next paint must re-transmit the data before placing it again.
    // Callers that issue RIS must call this so paintImageBand re-sends instead of
    // emitting an a=p crop that references an id the terminal no longer has (which
    // renders nothing — the repaint-loses-images bug).
    void forgetKittyTransmissions() const { kittyTransmitted.clear(); }
    size_t top = 0;      // absolute index of screen row 0
    int cr = 0, cc = 0;  // cursor, screen-relative
    Attr pen;            // current pen
    int sr = 0, sc = 0;  // DECSC save
    size_t stop = 0;     // DECSC save of `top` (see decsc/decrc)
    Attr spen;

    // parser state
    enum St { GROUND, ESC, CSI, OSC, DCS, APC } st = GROUND;
    std::string seq;       // accumulated escape payload
    bool escPend = false;  // saw ESC inside OSC/DCS/APC (looking for ST '\')
    char32_t uacc = 0;     // UTF-8 accumulator
    int uneed = 0;
    bool absorbLf = false;  // swallow one LF right after a sixel (cursor is already below it)
    std::string kittyAcc;   // accumulates a Kitty image's chunks (ESC_G..ESC\ × N) until m=0

    // Overstrike (typewriter convention used by `man`/groff via nroff): bold is
    // `X \b X`, underline is `_ \b X` (or `X \b _`). When a backspace lands the
    // cursor back onto the cell `put` just wrote, the next `put` is an overstrike:
    // we merge the two glyphs into one styled cell instead of overwriting. Both
    // fields must match for the merge — any other cursor motion clears the back-up.
    int osRow = -1, osCol = -1;  // cell `put` last wrote (overstrike target)
    bool osBack = false;         // a bs() just backed the cursor onto (osRow,osCol)

    Impl(int w, int h, int cw, int ch)
        : W(w < 1 ? 1 : w), H(h < 1 ? 1 : h), cellW(cw < 1 ? 1 : cw), cellH(ch < 1 ? 1 : ch) {
        ensure();
    }

    void ensure() {
        while (rows.size() < top + static_cast<size_t>(H)) rows.emplace_back(W);
    }
    std::vector<Cell>& screen(int r) { return rows[top + r]; }
    void scrollUp() {
        ++top;
        ensure();
    }
    void blank(std::vector<Cell>& L, int a, int b) {
        for (int i = a; i < b && i < W; ++i) L[i] = Cell{};
    }

    void lfWrap() {
        if (cr + 1 >= H)
            scrollUp();
        else
            ++cr;
    }
    void lf() {
        cc = 0;
        lfWrap();
        haveBase = false;
    }  // \n acts as CR+LF (no tty driver here)
    // The cell that the most recent base code point landed in, so a following zero-width code point
    // (combining mark, VS, ZWJ part) can attach to its grapheme cluster instead of taking a column.
    bool haveBase = false;
    int baseRow = 0, baseCol = 0;
    char32_t lastCp = 0;
    // True only when the cursor still sits immediately after the last base cell we wrote (no
    // motion, CR, or wrap since) — the precondition for attaching a combiner to that grapheme
    // cluster.
    bool atBase() const {
        return haveBase && baseRow == cr && cc == baseCol + screen2(baseRow, baseCol) &&
            baseCol < static_cast<int>(rows[top + baseRow].size());
    }
    int screen2(int r, int c) const { return rows[top + r][c].width ? rows[top + r][c].width : 1; }
    void put(char32_t cp) {
        int w = cellWidthOf(cp);
        // Zero-width follower, or any code point right after a ZWJ: glue it onto the current
        // cluster.
        if ((w == 0 || lastCp == 0x200D) && atBase()) {
            screen(baseRow)[baseCol].combine.push_back(cp);
            lastCp = cp;
            return;
        }
        // A regional-indicator following another forms one flag grapheme: attach as a combiner so
        // the pair occupies the single wide cell already written for the first indicator.
        if (cp >= 0x1F1E6 && cp <= 0x1F1FF && lastCp >= 0x1F1E6 && lastCp <= 0x1F1FF && atBase()) {
            screen(baseRow)[baseCol].combine.push_back(cp);
            lastCp = cp;
            return;
        }
        if (w == 0) w = 1;  // a stray combiner with no base: show it
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
                Attr a = gAttrs[prev.attr];
                a.flags |= bold ? A_BOLD : A_UNDER;
                prev.cp = (cp == U'_' && prev.cp != U'_') ? prev.cp : cp;
                prev.attr = internAttr(a);
                baseRow = cr;
                baseCol = cc;
                haveBase = true;
                lastCp = prev.cp;
                // Candidate stays on this cell so a triple overstrike (X\bX\bX) keeps merging.
                osBack = false;
                ++cc;
                if (prev.width == 2 && cc < W) ++cc;
                return;
            }
            // fall through: plain overwrite
        }
        if (cc + w > W) {
            cc = 0;
            lfWrap();
        }  // a wide char won't straddle the edge
        Cell& c = screen(cr)[cc];
        c = Cell{};
        c.cp = cp;
        c.attr = internAttr(pen);
        c.width = static_cast<uint8_t>(w);
        baseRow = cr;
        baseCol = cc;
        haveBase = true;
        lastCp = cp;
        osRow = cr;
        osCol = cc;
        osBack = false;  // this cell becomes the overstrike candidate
        ++cc;
        if (w == 2 && cc < W) {  // continuation cell holds the 2nd column
            Cell& cont = screen(cr)[cc];
            cont = Cell{};
            cont.cp = 0;
            cont.attr = internAttr(pen);
            cont.width = 0;
            ++cc;
        }
    }
    void tab() { cc = std::min(W - 1, ((cc / 8) + 1) * 8); }
    void bs() {
        if (cc > 0) --cc;
        osBack = (cr == osRow && cc == osCol);
    }
    void up(int n) { cr = std::max(0, cr - n); }
    void down(int n) { cr = std::min(H - 1, cr + n); }
    void right(int n) { cc = std::min(W - 1, cc + n); }
    void left(int n) { cc = std::max(0, cc - n); }
    void col(int c) { cc = std::min(std::max(0, c), W - 1); }
    void row(int r) { cr = std::min(std::max(0, r), H - 1); }
    void cup(int r, int c) {
        row(r);
        col(c);
    }
    // DECSC/DECRC save and restore the ABSOLUTE cursor position (top + cr), not just
    // the screen-relative cr: a sixel between them advances cr via finishSixel and may
    // scrollUp() (bumping `top`), which decrc must undo too — else each DECSC/DECRC-
    // bracketed image (mdcat's table/paragraph placement) leaves `top` one row higher,
    // and every following image drifts down a row. Saving `top` makes the bracket truly
    // position-neutral, matching the producer's intent. (timg doesn't bracket its
    // sixels, so its grid protocol is unaffected.)
    void decsc() {
        sr = cr;
        sc = cc;
        stop = top;
        spen = pen;
    }
    void decrc() {
        cr = sr;
        cc = sc;
        top = stop;
        pen = spen;
        ensure();
    }
    void el(int m) {  // erase in line
        auto& L = screen(cr);
        if (m == 1)
            blank(L, 0, cc + 1);
        else if (m == 2)
            blank(L, 0, W);
        else
            blank(L, cc, W);
    }
    void ed(int m) {  // erase in display
        if (m == 1) {
            for (int r = 0; r < cr; ++r) blank(screen(r), 0, W);
            el(1);
        } else if (m == 2) {
            for (int r = 0; r < H; ++r) blank(screen(r), 0, W);
        } else {
            el(0);
            for (int r = cr + 1; r < H; ++r) blank(screen(r), 0, W);
        }
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
        if (absorbLf) {
            absorbLf = false;
            if (b == '\n') return;
        }
        if (b == 0x1B) {
            st = ESC;
            return;
        }
        if (uneed > 0) {  // UTF-8 continuation
            if ((b & 0xC0) == 0x80) {
                uacc = (uacc << 6) | (b & 0x3F);
                if (--uneed == 0) put(uacc);
            } else {
                uneed = 0;
                put(0xFFFD);
                ground(b);
            }  // malformed: replace, reprocess
            return;
        }
        if (b < 0x20) {
            switch (b) {
            case '\n': lf(); break;
            case '\r': cc = 0; break;
            case '\t': tab(); break;
            case '\b': bs(); break;
            default: break;  // ignore other C0
            }
            return;
        }
        if (b < 0x80) {
            put(b);
            return;
        }
        if ((b & 0xE0) == 0xC0) {
            uacc = b & 0x1F;
            uneed = 1;
        } else if ((b & 0xF0) == 0xE0) {
            uacc = b & 0x0F;
            uneed = 2;
        } else if ((b & 0xF8) == 0xF0) {
            uacc = b & 0x07;
            uneed = 3;
        } else
            put(0xFFFD);
    }

    void esc(unsigned char b) {
        switch (b) {
        case '[':
            st = CSI;
            seq.clear();
            break;
        case ']':
            st = OSC;
            seq.clear();
            escPend = false;
            break;
        case 'P':
            st = DCS;
            seq.clear();
            escPend = false;
            break;
        case '_':
            st = APC;
            seq.clear();
            escPend = false;
            break;  // APC (Kitty graphics)
        case '7':
            decsc();
            st = GROUND;
            break;
        case '8':
            decrc();
            st = GROUND;
            break;
        case 'M':
            up(1);
            st = GROUND;
            break;                    // RI (no scroll-down for now)
        default: st = GROUND; break;  // ignore other ESC x
        }
    }

    void csi(unsigned char b) {
        if (b >= 0x40 && b <= 0x7E) {
            dispatchCsi(static_cast<char>(b));
            st = GROUND;
            return;
        }
        seq.push_back(static_cast<char>(b));
    }
    // OSC: accumulate until BEL or ST, then dispatch. OSC 8 sets/clears pen.link;
    // all other OSC sequences are silently consumed (text not garbled, attrs ignored).
    void osc(unsigned char b) {
        if (b == 0x07) {
            dispatchOsc();
            st = GROUND;
            return;
        }
        if (escPend) {
            if (b == '\\') {
                dispatchOsc();
                st = GROUND;
                return;
            }
            escPend = false;
        }
        if (b == 0x1B) {
            escPend = true;
            return;
        }
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
        if (escPend) {
            if (b == '\\') {
                finishSixel();
                st = GROUND;
                return;
            }
            escPend = false;
        }
        if (b == 0x1B) {
            escPend = true;
            return;
        }
        seq.push_back(static_cast<char>(b));
    }
    // Decode the captured sixel, anchor it at the cursor, and advance the cursor to
    // the row directly below the image, column 0 (the terminal's post-sixel behaviour).
    void finishSixel() {
        Image img;
        if (!decodeSixel(seq, img)) return;
        img.row = top + cr;
        img.col = cc;
        img.sixel = seq;  // keep the producer's exact bytes for verbatim replay
        images.push_back(std::move(img));
        int rowsCells = (images.back().Pv + cellH - 1) / cellH;
        cc = 0;
        for (int k = 0; k < rowsCells; ++k) {
            if (cr + 1 >= H)
                scrollUp();
            else
                ++cr;
        }
        absorbLf = true;  // a single trailing LF from the producer is now redundant
    }

    // APC (Kitty graphics): accumulate one chunk's payload until ST, then assemble it
    // back into a full ESC_G..ESC\ chunk in kittyAcc. A large image arrives as several
    // chunks (m=1 ... m=0); only the chunk whose `m=` is 0 or absent completes the image.
    void apc(unsigned char b) {
        if (escPend) {
            if (b == '\\') {
                finishKittyChunk();
                st = GROUND;
                return;
            }
            escPend = false;
        }
        if (b == 0x1B) {
            escPend = true;
            return;
        }
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
        uint32_t id = 0;
        int pw = 0, ph = 0, footCols = 0, footRows = 0;
        if (!kittyParse(kittyAcc, id, pw, ph, footCols, footRows)) {
            kittyAcc.clear();
            return;
        }
        Image img;
        img.row = top + cr;
        img.col = cc;
        img.Ph = pw;
        img.Pv = ph;
        img.footCols = footCols;
        img.footRows = footRows;
        img.kitty = kittyAcc;
        img.kid = id;
        images.push_back(std::move(img));
        kittyAcc.clear();
        // Reserve the image's height in cells: the producer's r= when it set one (so the band
        // matches the cells mdcat laid out), else the pixel-derived height. Ceil for the pixel
        // fallback so a fractional last row is not clipped.
        int rowsCells = footRows > 0 ? footRows : (ph + cellH - 1) / cellH;
        cc = 0;
        for (int k = 0; k < rowsCells; ++k) {
            if (cr + 1 >= H)
                scrollUp();
            else
                ++cr;
        }
        absorbLf = true;  // a single trailing LF from the producer is now redundant
    }

    void dispatchCsi(char final) {
        std::string s = seq;
        if (!s.empty() && (s[0] == '?' || s[0] == '>' || s[0] == '<' || s[0] == '=')) s.erase(0, 1);
        std::vector<int> ps;
        {
            size_t i = 0;
            while (i <= s.size()) {
                size_t j = s.find(';', i);
                std::string t = s.substr(i, (j == std::string::npos ? s.size() : j) - i);
                ps.push_back(t.empty() ? -1 : std::atoi(t.c_str()));
                if (j == std::string::npos) break;
                i = j + 1;
            }
        }
        auto P = [&](size_t i, int def) { return (i < ps.size() && ps[i] >= 0) ? ps[i] : def; };
        auto P1 = [&](size_t i) { return std::max(1, P(i, 1)); };  // 0/absent -> 1
        switch (final) {
        case 'A': up(P1(0)); break;
        case 'B': down(P1(0)); break;
        case 'C': right(P1(0)); break;
        case 'D': left(P1(0)); break;
        case 'E':
            cc = 0;
            down(P1(0));
            break;
        case 'F':
            cc = 0;
            up(P1(0));
            break;
        case 'G':
        case '`': col(P1(0) - 1); break;
        case 'd': row(P1(0) - 1); break;
        case 'H':
        case 'f': cup(P1(0) - 1, P1(1) - 1); break;
        case 'J': ed(P(0, 0)); break;
        case 'K': el(P(0, 0)); break;
        case 'm': applySgr(ps); break;
        default: break;  // ignore the rest (YAGNI)
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
            case 38:
            case 48: {
                uint32_t& slot = (c == 38) ? pen.fg : pen.bg;
                int mode = (i + 1 < ps.size()) ? ps[i + 1] : -1;
                if (mode == 5) {
                    slot = pal((i + 2 < ps.size() && ps[i + 2] >= 0) ? ps[i + 2] : 0);
                    i += 2;
                } else if (mode == 2) {
                    int r = (i + 2 < ps.size() && ps[i + 2] >= 0) ? ps[i + 2] : 0;
                    int g = (i + 3 < ps.size() && ps[i + 3] >= 0) ? ps[i + 3] : 0;
                    int bb = (i + 4 < ps.size() && ps[i + 4] >= 0) ? ps[i + 4] : 0;
                    slot = tru(r, g, bb);
                    i += 4;
                }
                break;
            }
            default:
                if (c >= 30 && c <= 37)
                    pen.fg = pal(c - 30);
                else if (c >= 90 && c <= 97)
                    pen.fg = pal(c - 90 + 8);
                else if (c >= 40 && c <= 47)
                    pen.bg = pal(c - 40);
                else if (c >= 100 && c <= 107)
                    pen.bg = pal(c - 100 + 8);
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
    // `fold=true` replaces every code point with foldFontVariant(cp) so the
    // returned text (and its cellAt mapping) use the normalised ASCII form.
    // Used by matchSpans for font-independent regex search.
    std::string rowText(size_t absRow,
                        std::vector<int>* cellAt = nullptr,
                        bool fold = false) const {
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
            appendUtf8(out, fold ? foldFontVariant(L[i].cp) : L[i].cp);
            for (char32_t comb : L[i].combine) appendUtf8(out, comb);
            if (cellAt)
                cellAt->resize(out.size(), i);  // bytes [before,out.size()) belong to cell i
            (void)before;
        }
        if (cellAt)
            cellAt->push_back((int)L.size());  // terminator: maps end offset past the last cell
        return out;
    }

    // Cell-column spans [startCol, endCol) of every match of `re` in row `absRow`.
    // Byte ranges from the regex are mapped through the cellAt table so spans land
    // on whole cells (wide chars / combining marks included). Empty matches are
    // skipped (an empty span would highlight nothing and could loop).
    // The row text is folded (foldFontVariant) before matching so that font
    // variants like math-italic '𝑎' are found by a plain 'a' pattern.
    std::vector<std::pair<int, int>> matchSpans(size_t absRow, const std::regex& re) const {
        std::vector<std::pair<int, int>> spans;
        std::vector<int> cellAt;
        std::string text = rowText(absRow, &cellAt, /*fold=*/true);
        if (text.empty()) return spans;
        auto begin = std::sregex_iterator(text.begin(), text.end(), re);
        for (auto it = begin; it != std::sregex_iterator(); ++it) {
            size_t b0 = (size_t)it->position(), len = (size_t)it->length();
            if (len == 0) continue;
            int startCol = cellAt[std::min(b0, cellAt.size() - 1)];
            int endCol = cellAt[std::min(b0 + len, cellAt.size() - 1)];  // exclusive
            if (endCol > startCol) spans.emplace_back(startCol, endCol);
        }
        return spans;
    }

    size_t contentRows() const {
        size_t last = 0;
        for (size_t r = 0; r < rows.size(); ++r) {
            for (const Cell& c : rows[r]) {
                if (!(c.cp == U' ' && c.attr == 0)) {
                    last = r + 1;
                    break;
                }
            }
        }
        for (const Image& img : images) {
            size_t imgEnd = img.row + (size_t)img.heightCells(cellH);
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
    // current position. Sixel: replay the original bytes clipped to that pixel band. Kitty:
    // transmit the image once (rewriting a=T->a=t so the transmit itself draws nothing), then
    // display the matching SOURCE crop scaled into the cells it spans — a cheap, payload-free a=p
    // placement that re-clips correctly on every scroll without re-encoding pixels.
    void paintImageBand(const Image& img, int skipPx, int keepPx, std::string& out) const {
        if (!img.isKitty()) {
            out += replaySixel(img, skipPx, keepPx);
            return;
        }
        if (kittyTransmitted.insert(img.kid).second)
            out += kittyTransmitOnly(img.kitty);  // first paint: send the data once
        // The full-image footprint: the producer's c=/r= when present (so a width-bound table image
        // stays in its column), else derived from the pixel size (ceil so it isn't clipped).
        int fullCols = img.footCols > 0 ? img.footCols : std::max(1, (img.Ph + cellW - 1) / cellW);
        int fullRows = img.footRows > 0 ? img.footRows : std::max(1, (img.Pv + cellH - 1) / cellH);
        // This placement shows the pixel band [skipPx, skipPx+keepPx) of a Pv-tall image. Scale the
        // full row footprint by the visible fraction so a partially-scrolled image keeps its
        // aspect; round to nearest (not ceil) so an unclipped image is exactly fullRows, never one
        // row tall-er.
        int rows = img.Pv > 0 ? std::max(1, (fullRows * keepPx + img.Pv / 2) / img.Pv) : fullRows;
        out += kittyPlace(img.kid, img.Ph, skipPx, keepPx, fullCols, rows);
    }

    // `withImages` inline-paints sixels (the --dump-images path); the interactive pager
    // paints images separately via paintImages() and passes false here. `withLinks`
    // re-emits OSC 8 hyperlinks; it is independent of images — only the plain-text grid
    // (--dump / nav traces) suppresses links so its output stays deterministic.
    void renderRow(size_t absRow,
                   std::string& out,
                   bool withImages = true,
                   size_t viewTop = 0,
                   size_t viewBot = 0,
                   bool withLinks = true,
                   const Highlight* hl = nullptr) const {
        if (withImages) {
            for (const Image& img : images) {
                size_t imgEnd = img.row + (size_t)img.heightCells(cellH);
                if (absRow < img.row || absRow >= imgEnd) continue;  // row not within the image
                size_t firstVisible = std::max(img.row, viewTop);
                if (absRow != firstVisible) continue;  // paint only on the top visible row
                int ppr = img.pxPerRow(cellH);
                int skipPx = (int)(firstVisible - img.row) * ppr;  // off-screen rows above
                int keepPx = img.Pv - skipPx;
                if (viewBot > firstVisible)  // clip to the window bottom
                    keepPx = std::min(keepPx, (int)(viewBot - firstVisible) * ppr);
                out += "\0337";  // DECSC: save cursor
                if (img.col > 0) {
                    out += "\033[";
                    out += std::to_string(img.col + 1);
                    out += 'G';
                }
                paintImageBand(img, skipPx, keepPx, out);
                out += "\0338";  // DECRC: restore cursor
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
            if (!((L[i].cp == U' ' || (L[i].cp == 0 && L[i].width == 0)) && L[i].attr == 0))
                last = i;
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
            const Attr& A = gAttrs[a];
            const Attr& B = gAttrs[b];
            return A.flags == B.flags && A.fg == B.fg && A.bg == B.bg;
        };
        int curHl = 0;  // 0 none, 1 other-match bg, 2 current-match bg
        auto emitHl = [&](int h) {
            if (h == 1)
                out += Highlight::OTHER;
            else if (h == 2)
                out += Highlight::CUR;
            else
                out += "\033[49m";  // back to default background
            curHl = h;
        };
        for (int i = 0; i <= last; ++i) {
            if (L[i].width == 0 && L[i].cp == 0)
                continue;  // wide-char continuation: occupies no byte
            bool needSgr = withImages ? (L[i].attr != cur) : !visualEq(L[i].attr, cur);
            if (needSgr) {
                out += sgrFor(L[i].attr);
                cur = L[i].attr;
                curHl = 0;
            }  // sgrFor reset bg
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
        if (withLinks && curLink != 0) emitOsc8(0);  // close any open hyperlink
        if (curHl != 0) out += "\033[49m";           // drop any open highlight background
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
            size_t imgEnd = img.row + (size_t)img.heightCells(cellH);
            size_t firstVisible = std::max(img.row, winFirst);
            if (firstVisible >= imgEnd || firstVisible >= winFirst + (size_t)winRows) continue;
            int ppr = img.pxPerRow(cellH);
            int skipPx = (int)(firstVisible - img.row) * ppr;  // rows above the window
            int keepPx = img.Pv - skipPx;
            if (winBot > firstVisible)  // clip to the window bottom
                keepPx = std::min(keepPx, (int)(winBot - firstVisible) * ppr);
            int up = winRows - (int)(firstVisible - winFirst);  // lines up from below the window
            out += "\033[";
            out += std::to_string(up);
            out += 'A';  // up to the image's top line
            out += "\033[";
            out += std::to_string(img.col + 1);
            out += 'G';                                // to its column
            out += "\0337";                            // DECSC: save the band-top position
            paintImageBand(img, skipPx, keepPx, out);  // paint (cursor left terminal-dependent)
            out += "\0338";                            // DECRC: back to the band-top position
            out += "\033[";
            out += std::to_string(up);
            out += 'B';
            out += '\r';  // down to below the window
        }
    }
};

// ---------------------------------------------------------------------------
// Search — regex matching over the cell grid's row text, the way more(1)/less(1)
// search. The pattern is an ECMAScript regex; matching is "smart-case" like less:
// case-insensitive unless the pattern contains an uppercase letter. State (the
// compiled pattern and whether it's valid) is held so `n`/`N` can repeat it.
// Search is kept out of Nav: Nav is deliberately grid-free so its motions unit-

Emulator::Emulator(int w, int h, int cw, int ch) : impl_(new Impl(w, h, cw, ch)) {}
Emulator::~Emulator() = default;

void Emulator::forgetKittyTransmissions() const {
    impl_->forgetKittyTransmissions();
}
void Emulator::feed(const char* p, size_t n) {
    impl_->feed(p, n);
}
size_t Emulator::contentRows() const {
    return impl_->contentRows();
}
std::vector<std::pair<int, int>> Emulator::matchSpans(size_t absRow, const std::regex& re) const {
    return impl_->matchSpans(absRow, re);
}
void Emulator::renderRow(size_t absRow,
                         std::string& out,
                         bool withImages,
                         size_t viewTop,
                         size_t viewBot,
                         bool withLinks,
                         const Highlight* hl) const {
    impl_->renderRow(absRow, out, withImages, viewTop, viewBot, withLinks, hl);
}
void Emulator::paintImages(std::string& out, size_t winFirst, int winRows, size_t winBot) const {
    impl_->paintImages(out, winFirst, winRows, winBot);
}
const std::vector<Image>& Emulator::images() const {
    return impl_->images;
}

}  // namespace gmore
