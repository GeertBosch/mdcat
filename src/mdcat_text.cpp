#include "mdcat_text.h"

#include <cctype>

#include "mdcat_style.h"
#include "unicode_width.h"

namespace mdcat {

std::string toLowerCopy(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

int utf8SequenceLength(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x06) return 2;
    if ((c >> 4) == 0x0E) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

int decodeUtf8(const std::string& s, size_t i, uint32_t& cp) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    int len = utf8SequenceLength(c);
    if (len == 1 || i + static_cast<size_t>(len) > s.size()) {
        cp = c;
        return 1;
    }
    static const unsigned char kLeadMask[5] = {0, 0x7F, 0x1F, 0x0F, 0x07};
    cp = c & kLeadMask[len];
    for (int k = 1; k < len; ++k) {
        unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) {
            cp = c;
            return 1;
        }  // not a continuation byte: invalid
        cp = (cp << 6) | (cc & 0x3F);
    }
    return len;
}

using uw::codePointWidth;

int displayWidth(const std::string& s) {
    int width = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == kEsc && i + 1 < s.size() && s[i + 1] == '[') {
            // CSI sequence: ESC [ ... final-byte in @..~
            i += 2;
            while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) ++i;
            if (i < s.size()) ++i;  // consume the final byte
            continue;
        }
        if (c == kEsc && i + 1 < s.size() && s[i + 1] == ']') {
            // OSC sequence: ESC ] ... terminated by BEL or ST (ESC backslash)
            i += 2;
            while (i < s.size()) {
                if (static_cast<unsigned char>(s[i]) == 0x07) {
                    ++i;
                    break;
                }
                if (s[i] == kEsc && i + 1 < s.size() && s[i + 1] == '\\') {
                    i += 2;
                    break;
                }
                ++i;
            }
            continue;
        }
        uint32_t cp;
        i += static_cast<size_t>(decodeUtf8(s, i, cp));
        int w = codePointWidth(cp);
        // Absorb the rest of the grapheme cluster: it is drawn in one glyph, not per code point.
        while (i < s.size()) {
            uint32_t next;
            size_t adv = static_cast<size_t>(decodeUtf8(s, i, next));
            if (!uw::continuesCluster(cp, next)) break;
            w = uw::widthWithFollower(w, next);
            cp = next;
            i += adv;
        }
        width += w;
    }
    return width;
}

std::string horizontalLine(int n, const std::string& bar) {
    std::string out;
    for (int i = 0; i < n; ++i) out += bar;
    return out;
}

std::string padTo(const std::string& s, int width) {
    int w = displayWidth(s);
    std::string out = s;
    for (int i = w; i < width; ++i) out += ' ';
    return out;
}

std::string padAligned(const std::string& s, int width, Align a) {
    int w = displayWidth(s);
    if (w >= width) return s;
    int pad = width - w;
    switch (a) {
    case Align::Right: return std::string(pad, ' ') + s;
    case Align::Center: {
        int left = pad / 2;
        return std::string(left, ' ') + s + std::string(pad - left, ' ');
    }
    case Align::Left:
    default: return s + std::string(pad, ' ');
    }
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

void appendUtf8(std::string& out, unsigned cp) {
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

size_t unitLength(const std::string& s, size_t i) {
    char c = s[i];
    if (c == kEsc && i + 1 < s.size() && s[i + 1] == '[') {  // CSI: ESC [ ... final @..~
        size_t j = i + 2;
        while (j < s.size() && !(s[j] >= '@' && s[j] <= '~')) ++j;
        if (j < s.size()) ++j;
        return j - i;
    }
    if (c == kEsc && i + 1 < s.size() && s[i + 1] == ']') {  // OSC: ESC ] ... BEL or ST
        size_t j = i + 2;
        while (j < s.size()) {
            if (static_cast<unsigned char>(s[j]) == 0x07) {
                ++j;
                break;
            }
            if (s[j] == kEsc && j + 1 < s.size() && s[j + 1] == '\\') {
                j += 2;
                break;
            }
            ++j;
        }
        return j - i;
    }
    // A math atom (kMathAtomStart .. kMathAtomEnd) is one indivisible unit: reflow must never break
    // inside it or collapse its interior spaces. Swallow everything through the end marker
    // (inclusive) so the whole expression travels as a single "word".
    if (s.compare(i, kMathAtomStart.size(), kMathAtomStart) == 0) {
        size_t e = s.find(kMathAtomEnd, i + kMathAtomStart.size());
        if (e != std::string::npos) return e + kMathAtomEnd.size() - i;
        return s.size() - i;  // unterminated (shouldn't happen): take the rest
    }
    // Base code point, then absorb following zero-width code points and ZWJ-joined sequences so the
    // whole grapheme cluster travels together.
    uint32_t cp;
    size_t j = i + static_cast<size_t>(decodeUtf8(s, i, cp));
    while (j < s.size()) {
        uint32_t next;
        size_t adv = static_cast<size_t>(decodeUtf8(s, j, next));
        if (!uw::continuesCluster(cp, next)) break;
        j += adv;
        cp = next;
    }
    return j - i;
}

std::string closeStylesAtLineBreaks(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool inCode = false;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\n') {
            if (inCode) out += kCodeOff;  // close the span at end of line
            out += '\n';
            if (inCode) out += kCodeOn;  // reopen it at the start of the next line
            ++i;
            continue;
        }
        size_t len = unitLength(s, i);
        if (s.compare(i, kCodeOn.size(), kCodeOn) == 0)
            inCode = true;
        else if (s.compare(i, kCodeOff.size(), kCodeOff) == 0)
            inCode = false;
        out.append(s, i, len);
        i += len;
    }
    return out;
}

std::string reflow(const std::string& s, int width) {
    if (width < 1) width = 1;

    // Split into whitespace-separated words; escapes travel with the word they are attached to. A
    // '\n' (from a <br> hard break) is kept as a standalone "\n" word that forces a line break
    // below.
    std::vector<std::string> words;
    std::string cur;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == ' ' || s[i] == '\n') {
            if (!cur.empty()) {
                words.push_back(cur);
                cur.clear();
            }
            if (s[i] == '\n') words.push_back("\n");
            ++i;
        } else {
            size_t len = unitLength(s, i);
            cur.append(s, i, len);
            i += len;
        }
    }
    if (!cur.empty()) words.push_back(cur);

    std::string out;
    int lineWidth = 0;      // display columns already on the current line
    bool lineEmpty = true;  // nothing placed on the current line yet
    for (std::string w : words) {
        if (w == "\n") {  // forced hard break from a <br>
            out += '\n';
            lineWidth = 0;
            lineEmpty = true;
            continue;
        }
        // A word that cannot fit on a line by itself is hard-cut into width-sized pieces.
        while (displayWidth(w) > width) {
            if (!lineEmpty) {
                out += '\n';
                lineWidth = 0;
                lineEmpty = true;
            }
            std::string piece;
            int pieceWidth = 0;
            size_t i = 0;
            for (; i < w.size();) {
                size_t len = unitLength(w, i);
                int cw = displayWidth(w.substr(i, len));  // 0 for escape, 1 normal, 2 wide
                if (pieceWidth + cw > width) break;
                piece.append(w, i, len);
                pieceWidth += cw;
                i += len;
            }
            // A single unit wider than the whole line (a width-2 grapheme when width==1) fits in no
            // piece. Place it anyway — overflowing one column beats looping forever (w never
            // shrinks).
            if (i == 0) {
                size_t len = unitLength(w, 0);
                piece.append(w, 0, len);
                i = len;
            }
            out += piece;
            out += '\n';
            w = w.substr(i);
        }
        int ww = displayWidth(w);
        if (!lineEmpty && lineWidth + 1 + ww > width) {  // start a new line for this word
            out += '\n';
            lineWidth = 0;
            lineEmpty = true;
        }
        if (!lineEmpty) {
            out += ' ';
            ++lineWidth;
        }
        out += w;
        lineWidth += ww;
        lineEmpty = false;
    }
    return stripMathAtoms(closeStylesAtLineBreaks(out));
}

std::string stripMathAtoms(const std::string& s) {
    if (s.find(kMathAtomStart[0]) == std::string::npos) return s;  // fast path: no U+E00x lead byte
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s.compare(i, kMathAtomStart.size(), kMathAtomStart) == 0) {
            i += kMathAtomStart.size();
        } else if (s.compare(i, kMathAtomEnd.size(), kMathAtomEnd) == 0) {
            i += kMathAtomEnd.size();
        } else {
            out += s[i++];
        }
    }
    return out;
}

std::vector<std::string> splitOnNewlines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '\n') {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

}  // namespace mdcat
