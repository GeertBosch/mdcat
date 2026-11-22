#include "mdcat_inline.h"

#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "mdcat_math.h"
#include "mdcat_state.h"
#include "mdcat_style.h"
#include "mdcat_text.h"

namespace mdcat {

namespace {

// ---------------------------------------------------------------------------
// Inline parsing
// ---------------------------------------------------------------------------

class InlineRenderer {
public:
    explicit InlineRenderer(const std::string& text) : s_(text) {}

    std::string render() {
        tokenize();
        resolveEmphasis();
        return emit();
    }

private:
    const std::string& s_;

    enum class Kind { Text, Code, Link, Image, Delim };
    struct Token {
        Kind kind;
        std::string text;  // Text: literal; Code: content; Link: rendered link text
        std::string url;   // Link: target URL
        // Delim fields:
        char delim = 0;  // '*' or '_'
        int count = 0;   // remaining usable delimiter characters
        bool canOpen = false;
        bool canClose = false;
        // Emphasis resolution result, applied at emit time:
        int openStrong = 0, openEm = 0;    // styles opened just before this token's text
        int closeStrong = 0, closeEm = 0;  // styles closed just after
    };
    std::vector<Token> toks_;

    static bool isAsciiPunct(char c) {
        return std::string("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~").find(c) != std::string::npos;
    }
    static bool isWordChar(unsigned char c) { return std::isalnum(c) || c == '_'; }
    static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n'; }

    char charAt(size_t i) const { return i < s_.size() ? s_[i] : '\n'; }
    char prevAt(size_t i) const { return i == 0 ? '\n' : s_[i - 1]; }

    void addText(const std::string& t) {
        if (!toks_.empty() && toks_.back().kind == Kind::Text)
            toks_.back().text += t;
        else {
            Token tk;
            tk.kind = Kind::Text;
            tk.text = t;
            toks_.push_back(std::move(tk));
        }
    }

    // --- Pass 1 -------------------------------------------------------------
    void tokenize() {
        size_t n = s_.size();
        for (size_t i = 0; i < n;) {
            char c = s_[i];
            if (c == '\\' && i + 1 < n && isAsciiPunct(s_[i + 1])) {
                addText(std::string(1, s_[i + 1]));
                i += 2;
                continue;
            }
            if (c == '`') {
                if (scanCode(i)) continue;
            }
            if (c == '$') {
                if (scanMath(i)) continue;
            }
            if (c == '<') {
                if (scanHardBreak(i)) continue;
            }
            if (c == '!' && i + 1 < n && s_[i + 1] == '[') {
                if (scanImage(i)) continue;
            }
            if (c == '[') {
                if (scanLink(i)) continue;
            }
            if (c == '*' || c == '_') {
                scanDelim(i);
                continue;
            }
            int len = utf8SequenceLength(static_cast<unsigned char>(c));
            addText(s_.substr(i, len));
            i += len;
        }
    }

    // `code`: backtick run closed by an equal-length run; content emitted verbatim. Advances i.
    bool scanCode(size_t& i) {
        size_t open = 0;
        while (i + open < s_.size() && s_[i + open] == '`') ++open;
        size_t j = i + open;
        while (j < s_.size()) {
            if (s_[j] == '`') {
                size_t run = 0;
                while (j + run < s_.size() && s_[j + run] == '`') ++run;
                if (run == open) {
                    Token tk;
                    tk.kind = Kind::Code;
                    tk.text = s_.substr(i + open, j - (i + open));
                    toks_.push_back(std::move(tk));
                    i = j + run;
                    return true;
                }
                j += run;
            } else
                ++j;
        }
        return false;  // unmatched: fall through and treat the backticks as text
    }

    // $...$ (inline) or $$...$$ (block) LaTeX math. The content is transliterated to Unicode by
    // renderMath and emitted as plain text. We follow GitHub's heuristics to avoid mistaking prose
    // dollar signs (prices) for math: the opener must be followed by a non-space, the closer must
    // be preceded by a non-space, and a single-$ closer may not be immediately followed by a digit.
    // The '$' is at position i. On failure, falls through and the '$' is treated as literal text.
    bool scanMath(size_t& i) {
        size_t open = (i + 1 < s_.size() && s_[i + 1] == '$') ? 2 : 1;  // $ or $$
        size_t start = i + open;
        if (start >= s_.size() || isSpace(s_[start])) return false;  // no space right after opener
        size_t j = start;
        while (j < s_.size()) {
            if (s_[j] == '\\' && j + 1 < s_.size()) {
                j += 2;
                continue;
            }  // skip escaped char
            if (s_[j] == '$') {
                size_t run = 0;
                while (j + run < s_.size() && s_[j + run] == '$') ++run;
                if (run >= open) {
                    if (j == start) return false;          // empty: "$$" is not math
                    if (isSpace(s_[j - 1])) return false;  // no space right before closer
                    size_t after = j + open;
                    // For inline ($) math, a digit right after the closer means it's likely a
                    // price.
                    if (open == 1 && after < s_.size() &&
                        std::isdigit(static_cast<unsigned char>(s_[after])))
                        return false;
                    // Bracket the rendered math with the atom sentinels so reflow treats it as one
                    // indivisible unit (its interior spacing is exact, not prose to be collapsed).
                    addText(kMathAtomStart + renderMath(s_.substr(start, j - start)) +
                            kMathAtomEnd);
                    i = j + open;
                    return true;
                }
                j += run;
            } else
                ++j;
        }
        return false;  // unterminated: treat the opening '$' as literal text
    }

    // <br>, <br/>, <br />: an HTML hard line break (GFM treats it as a literal line break wherever
    // it appears in inline content). Emitted as a '\n' Text token; downstream reflow and table-cell
    // layout split on '\n'. Tag/attribute matching is case-insensitive but otherwise strict: a
    // malformed run like "<brx" or "<br foo" falls through and is rendered as literal text,
    // matching how mdcat passes all other inline HTML through unchanged. The '<' is at position i.
    bool scanHardBreak(size_t& i) {
        size_t j = i + 1;  // just past '<'
        auto lower = [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        };
        if (j + 1 >= s_.size() || lower(s_[j]) != 'b' || lower(s_[j + 1]) != 'r') return false;
        j += 2;
        while (j < s_.size() && (s_[j] == ' ' || s_[j] == '\t')) ++j;  // optional whitespace
        if (j < s_.size() && s_[j] == '/') ++j;                        // optional self-close slash
        if (j >= s_.size() || s_[j] != '>') return false;              // must end at '>'
        addText("\n");
        i = j + 1;
        return true;
    }

    // ![alt](url): markdown image. Captures alt text and url; the '!' is at position i.
    bool scanImage(size_t& i) {
        size_t j = i + 1;  // j points at '['
        size_t close = j + 1;
        int depth = 1;
        while (close < s_.size()) {
            if (s_[close] == '\\' && close + 1 < s_.size()) {
                close += 2;
                continue;
            }
            if (s_[close] == '[')
                ++depth;
            else if (s_[close] == ']') {
                if (--depth == 0) break;
            }
            ++close;
        }
        if (close >= s_.size() || close + 1 >= s_.size() || s_[close + 1] != '(') return false;
        size_t up = close + 2;
        std::string url;
        int pd = 1;
        while (up < s_.size()) {
            char c = s_[up];
            if (c == '\\' && up + 1 < s_.size()) {
                url += s_[up + 1];
                up += 2;
                continue;
            }
            if (c == '(')
                ++pd;
            else if (c == ')') {
                if (--pd == 0) break;
            }
            url += c;
            ++up;
        }
        if (up >= s_.size() || s_[up] != ')') return false;
        Token tk;
        tk.kind = Kind::Image;
        tk.text = s_.substr(j + 1, close - (j + 1));  // raw alt text
        tk.url = trim(url);
        toks_.push_back(std::move(tk));
        i = up + 1;
        return true;
    }

    // [text](url): link text is inline-rendered by a *separate* renderer over just that slice,
    // which is cheap because link text is short and contains no further links to recurse on here.
    bool scanLink(size_t& i) {
        // Find the matching ']' for the opening '[' (no nested brackets expected in our inputs).
        size_t close = i + 1;
        int depth = 1;
        while (close < s_.size()) {
            if (s_[close] == '\\' && close + 1 < s_.size()) {
                close += 2;
                continue;
            }
            if (s_[close] == '[')
                ++depth;
            else if (s_[close] == ']') {
                if (--depth == 0) break;
            }
            ++close;
        }
        if (close >= s_.size() || close + 1 >= s_.size() || s_[close + 1] != '(') return false;
        size_t up = close + 2;
        std::string url;
        int pd = 1;
        while (up < s_.size()) {
            char c = s_[up];
            if (c == '\\' && up + 1 < s_.size()) {
                url += s_[up + 1];
                up += 2;
                continue;
            }
            if (c == '(')
                ++pd;
            else if (c == ')') {
                if (--pd == 0) break;
            }
            url += c;
            ++up;
        }
        if (up >= s_.size() || s_[up] != ')') return false;
        Token tk;
        tk.kind = Kind::Link;
        tk.text = InlineRenderer(s_.substr(i + 1, close - (i + 1))).render();
        tk.url = trim(url);
        toks_.push_back(std::move(tk));
        i = up + 1;
        return true;
    }

    // A maximal run of identical '*' or '_'. Decide open/close capability from flanking chars.
    void scanDelim(size_t& i) {
        char d = s_[i];
        size_t run = 0;
        while (i + run < s_.size() && s_[i + run] == d) ++run;
        char before = prevAt(i);
        char after = charAt(i + run);
        bool spaceBefore = isSpace(before), spaceAfter = isSpace(after);
        Token tk;
        tk.kind = Kind::Delim;
        tk.delim = d;
        tk.count = static_cast<int>(run);
        tk.canOpen = !spaceAfter;
        tk.canClose = !spaceBefore;
        if (d == '_') {
            // Intraword underscores neither open nor close (keeps snake_case intact).
            if (isWordChar(static_cast<unsigned char>(before))) tk.canOpen = false;
            if (isWordChar(static_cast<unsigned char>(after))) tk.canClose = false;
        }
        toks_.push_back(std::move(tk));
        i += run;
    }

    // --- Pass 2 -------------------------------------------------------------
    void resolveEmphasis() {
        std::vector<size_t> stack;  // indices of Delim tokens that can still open
        for (size_t idx = 0; idx < toks_.size(); ++idx) {
            Token& t = toks_[idx];
            if (t.kind != Kind::Delim) continue;
            if (t.canClose) {
                // Try to match against the nearest compatible opener on the stack.
                while (t.count > 0 && !stack.empty()) {
                    bool matched = false;
                    for (size_t k = stack.size(); k-- > 0;) {
                        Token& o = toks_[stack[k]];
                        if (o.delim == t.delim && o.canOpen && o.count > 0) {
                            int use = (t.count >= 2 && o.count >= 2) ? 2 : 1;
                            if (use == 2) {
                                o.openStrong++;
                                t.closeStrong++;
                            } else {
                                o.openEm++;
                                t.closeEm++;
                            }
                            o.count -= use;
                            t.count -= use;
                            if (o.count == 0)
                                stack.resize(k);  // opener spent; drop it and any above
                            matched = true;
                            break;
                        }
                    }
                    if (!matched) break;
                }
            }
            if (t.canOpen && t.count > 0) stack.push_back(idx);
        }
    }

    // --- Emit ---------------------------------------------------------------
    std::string emit() {
        std::string out;
        for (const Token& t : toks_) {
            switch (t.kind) {
            case Kind::Text: out += t.text; break;
            case Kind::Code: out += kCodeOn + t.text + kCodeOff; break;
            case Kind::Link:
                out += "\033]8;;" + t.url + "\033\\" + t.text + "\033]8;;\033\\";
                break;
            case Kind::Image:
                // Inline image: show alt text (we can't fetch remote URLs inline).
                out += t.text;
                break;
            case Kind::Delim: {
                // Closers emit their off-codes first, then any opens, then leftover literals.
                for (int k = 0; k < t.closeEm; ++k) out += kItalicOff;
                for (int k = 0; k < t.closeStrong; ++k) out += kBoldOff;
                for (int k = 0; k < t.openStrong; ++k) out += kBoldOn;
                for (int k = 0; k < t.openEm; ++k) out += kItalicOn;
                // Any delimiter characters not consumed by a pair stay as literal text.
                out.append(static_cast<size_t>(t.count), t.delim);
                break;
            }
            }
        }
        return out;
    }
};

}  // namespace

std::string renderInline(const std::string& text) {
    return InlineRenderer(text).render();
}

}  // namespace mdcat
