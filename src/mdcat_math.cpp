#include "mdcat_math.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

#include "mdcat_state.h"
#include "mdcat_style.h"
#include "mdcat_text.h"

namespace mdcat {

namespace {

// ---------------------------------------------------------------------------
// LaTeX math -> Unicode
// ---------------------------------------------------------------------------
//
// GitHub renders LaTeX between $...$ (inline) and $$...$$ (block). We don't ship a TeX engine;
// instead renderMath() does a best-effort transliteration of the common, simple expressions that
// map cleanly onto Unicode: Greek letters, a handful of operator/relation symbols, super/subscripts
// of digits and a few characters, \mathrm/\mathbf wrappers, and \mathbb blackboard-bold letters.
// Bare Latin letters render in math italic by default (as in LaTeX math mode); \mathrm/\text keep
// them upright and \mathbf makes them bold.
// Anything it can't represent is left as-is — and a `\cmd{...}` group that can't be fully rendered
// is kept verbatim INCLUDING its braces (so a LaTeX reader still sees `\mathbb{R}`, not
// `\mathbbR`), so the output is never worse than the raw source. The function is total: never
// throws.

// Greek letters and named symbols, keyed by the LaTeX command without its backslash.
const std::map<std::string, std::string>& mathSymbols() {
    static const std::map<std::string, std::string> m = {
        // Lowercase Greek
        {"alpha", "α"},
        {"beta", "β"},
        {"gamma", "γ"},
        {"delta", "δ"},
        {"epsilon", "ε"},
        {"varepsilon", "ε"},
        {"zeta", "ζ"},
        {"eta", "η"},
        {"theta", "θ"},
        {"vartheta", "ϑ"},
        {"iota", "ι"},
        {"kappa", "κ"},
        {"lambda", "λ"},
        {"mu", "μ"},
        {"nu", "ν"},
        {"xi", "ξ"},
        {"omicron", "ο"},
        {"pi", "π"},
        {"varpi", "ϖ"},
        {"rho", "ρ"},
        {"varrho", "ϱ"},
        {"sigma", "σ"},
        {"varsigma", "ς"},
        {"tau", "τ"},
        {"upsilon", "υ"},
        {"phi", "φ"},
        {"varphi", "ϕ"},
        {"chi", "χ"},
        {"psi", "ψ"},
        {"omega", "ω"},
        // Uppercase Greek
        {"Alpha", "Α"},
        {"Beta", "Β"},
        {"Gamma", "Γ"},
        {"Delta", "Δ"},
        {"Epsilon", "Ε"},
        {"Zeta", "Ζ"},
        {"Eta", "Η"},
        {"Theta", "Θ"},
        {"Iota", "Ι"},
        {"Kappa", "Κ"},
        {"Lambda", "Λ"},
        {"Mu", "Μ"},
        {"Nu", "Ν"},
        {"Xi", "Ξ"},
        {"Omicron", "Ο"},
        {"Pi", "Π"},
        {"Rho", "Ρ"},
        {"Sigma", "Σ"},
        {"Tau", "Τ"},
        {"Upsilon", "Υ"},
        {"Phi", "Φ"},
        {"Chi", "Χ"},
        {"Psi", "Ψ"},
        {"Omega", "Ω"},
        // Operators / relations / misc
        {"exists", "∃"},
        {"in", "∈"},
        {"int", "∫"},
        {"sum", "∑"},
        {"prod", "∏"},
        {"partial", "∂"},
        {"infty", "∞"},
        {"perp", "⊥"},
        {"parallel", "∥"},
        {"therefore", "∴"},
        {"because", "∵"},
        {"subset", "⊂"},
        {"supset", "⊃"},
        {"subseteq", "⊆"},
        {"supseteq", "⊇"},
        {"to", "→"},
        {"rightarrow", "→"},
        {"longrightarrow", "⟶"},
        {"leftarrow", "←"},
        {"Rightarrow", "⇒"},
        {"Leftarrow", "⇐"},
        {"times", "×"},
        {"div", "÷"},
        {"pm", "±"},
        {"mp", "∓"},
        {"simeq", "≃"},
        {"approx", "≈"},
        {"cong", "≅"},
        {"equiv", "≡"},
        {"neq", "≠"},
        {"leq", "≤"},
        {"geq", "≥"},
        {"ll", "≪"},
        {"gg", "≫"},
        {"cdot", "⋅"},
        {"cdots", "⋯"},
        {"ldots", "…"},
        {"dots", "…"},
        {"nabla", "∇"},
        {"forall", "∀"},
        {"notin", "∉"},
        {"emptyset", "∅"},
        {"cup", "∪"},
        {"cap", "∩"},
        {"wedge", "∧"},
        {"vee", "∨"},
        {"neg", "¬"},
        {"oplus", "⊕"},
        {"otimes", "⊗"},
        {"sqrt", "√"},
        {"angle", "∠"},
        {"prime", "′"},
        {"circ", "∘"},
        {"star", "⋆"},
        {"langle", "⟨"},
        {"rangle", "⟩"},
        {"propto", "∝"},
        {"mapsto", "↦"},
    };
    return m;
}

// Super/subscript glyphs for the characters that have dedicated Unicode forms. Characters with no
// form are left unconverted (the whole script group is then rendered literally with ^ or _).
const std::map<char, std::string>& superscripts() {
    static const std::map<char, std::string> m = {
        {'0', "⁰"},
        {'1', "¹"},
        {'2', "²"},
        {'3', "³"},
        {'4', "⁴"},
        {'5', "⁵"},
        {'6', "⁶"},
        {'7', "⁷"},
        {'8', "⁸"},
        {'9', "⁹"},
        {'+', "⁺"},
        {'-', "⁻"},
        {'=', "⁼"},
        {'(', "⁽"},
        {')', "⁾"},
        {'n', "ⁿ"},
        {'i', "ⁱ"},
        {'.', "·"},
    };
    return m;
}
const std::map<char, std::string>& subscripts() {
    static const std::map<char, std::string> m = {
        {'0', "₀"},
        {'1', "₁"},
        {'2', "₂"},
        {'3', "₃"},
        {'4', "₄"},
        {'5', "₅"},
        {'6', "₆"},
        {'7', "₇"},
        {'8', "₈"},
        {'9', "₉"},
        {'+', "₊"},
        {'-', "₋"},
        {'=', "₌"},
        {'(', "₍"},
        {')', "₎"},
    };
    return m;
}

// Blackboard-bold (\mathbb) and calligraphic (\mathcal) letters that have dedicated Unicode forms.
// Returns the mapped string, or empty if the letter has no form (so the caller can keep \mathbb{X}
// literal rather than dropping the styling).
std::string mathbbLetter(char c) {
    switch (c) {
    case 'A': return "𝔸";
    case 'B': return "𝔹";
    case 'C': return "ℂ";
    case 'D': return "𝔻";
    case 'E': return "𝔼";
    case 'F': return "𝔽";
    case 'G': return "𝔾";
    case 'H': return "ℍ";
    case 'I': return "𝕀";
    case 'J': return "𝕁";
    case 'K': return "𝕂";
    case 'L': return "𝕃";
    case 'M': return "𝕄";
    case 'N': return "ℕ";
    case 'O': return "𝕆";
    case 'P': return "ℙ";
    case 'Q': return "ℚ";
    case 'R': return "ℝ";
    case 'S': return "𝕊";
    case 'T': return "𝕋";
    case 'U': return "𝕌";
    case 'V': return "𝕍";
    case 'W': return "𝕎";
    case 'X': return "𝕏";
    case 'Y': return "𝕐";
    case 'Z': return "ℤ";
    }
    return "";
}


// Map an ASCII letter to its styled Mathematical-Alphanumeric code point, or return the letter
// unchanged for Upright (which is plain ASCII). The Mathematical Italic and Bold blocks are
// contiguous A-Z then a-z, except that italic lowercase 'h' is unassigned (its glyph is the Planck
// constant U+210E). Non-letters pass through unchanged.
std::string mathStyledLetter(char c, MathStyle style) {
    bool upper = (c >= 'A' && c <= 'Z');
    bool lower = (c >= 'a' && c <= 'z');
    if (!upper && !lower) return std::string(1, c);
    unsigned idx = upper ? (c - 'A') : 26 + (c - 'a');
    std::string out;
    switch (style) {
    case MathStyle::Upright: return std::string(1, c);
    case MathStyle::Italic:
        if (c == 'h') {
            appendUtf8(out, 0x210E);
            return out;
        }  // Planck constant ℎ
        appendUtf8(out, 0x1D434 + idx);  // 𝐴..𝑧 (italic)
        return out;
    case MathStyle::Bold:
        appendUtf8(out, 0x1D400 + idx);  // 𝐀..𝐳 (bold)
        return out;
    }
    return std::string(1, c);
}

// Try to convert a super/subscript group at s[i] (the '^' or '_'). On success, appends the Unicode
// to out, advances i past the consumed text, and returns true. On failure returns false and leaves
// i/out unchanged so the caller can emit the marker literally.
bool convertScript(const std::string& s, size_t& i, std::string& out, bool sup) {
    const auto& table = sup ? superscripts() : subscripts();
    std::string body;  // the raw characters being scripted
    size_t j = i + 1;
    if (j < s.size() && s[j] == '{') {
        size_t k = j + 1;
        while (k < s.size() && s[k] != '}') body += s[k++];
        if (k >= s.size()) return false;  // unbalanced brace: give up
        j = k + 1;
    } else if (j < s.size()) {
        body = s[j];  // single-character script: x^2, a_i
        j += 1;
    } else {
        return false;
    }
    std::string conv;
    for (char c : body) {
        auto it = table.find(c);
        if (it == table.end()) return false;  // a char with no glyph: render the group literally
        conv += it->second;
    }
    out += conv;
    i = j;
    return true;
}

// Whether a \sqrt / \frac argument needs grouping parentheses when flattened inline against the
// fraction bar `/` we emit. The rule depends on which side of the bar the argument is on:
//
//   - Additive operators ALWAYS force parens (both sides): a top-level `+`, a `\pm`/`\mp` (`±`/`∓`,
//     which bind like `+`), or a non-leading `-` (a leading sign is unary — `-b` is one term).
//     Without them `a + b/c` and `a/b + c` misgroup.
//   - Explicit MULTIPLICATIVE operators (`*`, `/`, `\times`, `\cdot`, `\div`) force parens only in
//   a
//     DENOMINATOR. They are same-strength as `/`, so on the numerator side left-to-right evaluation
//     is already correct (`5*6/7` = `(5*6)/7` ✓), but in a denominator `5/6*7` would wrongly mean
//     `(5/6)*7`, so `\frac{5}{6*7}` must render `5/(6*7)`.
//   - IMPLICIT multiplication (juxtaposition, `2a`) binds tighter than `/` and never needs parens,
//     even in a denominator: `x/2a` conventionally means `x/(2a)`, so `\frac{x}{2a}` stays `x/2a`.
//
// "Top level" means brace-depth 0 — operators inside a nested {…} or \sqrt/\frac subgroup don't
// count. Scans the RAW LaTeX (before transliteration) so it can see \pm, \times, \cdot and braces.
bool mathArgNeedsParens(const std::string& tex, bool denominator) {
    int depth = 0;
    bool sawTerm = false;  // have we passed any non-space content yet? (distinguishes unary '-')
    for (size_t i = 0; i < tex.size(); ++i) {
        char c = tex[i];
        if (c == '{') {
            ++depth;
            continue;
        }
        if (c == '}') {
            if (depth > 0) --depth;
            continue;
        }
        if (depth != 0) continue;
        if (c == '\\') {
            size_t j = i + 1;
            while (j < tex.size() && std::isalpha(static_cast<unsigned char>(tex[j]))) ++j;
            std::string cmd = tex.substr(i + 1, j - (i + 1));
            if (cmd == "pm" || cmd == "mp") return true;  // ± / ∓ bind like +
            if (denominator && (cmd == "times" || cmd == "cdot" || cmd == "div")) return true;
            sawTerm = true;
            i = j - 1;  // skip the command name
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        if (c == '+') return true;
        if (c == '-' && sawTerm) return true;  // a non-leading '-' is a binary subtraction
        if (denominator && (c == '*' || c == '/')) return true;  // explicit mul/div misgroups here
        sawTerm = true;
    }
    return false;
}


// Render a \sqrt / \frac argument, wrapping it in faint grouping parens ONLY when it needs them
// (mathArgNeedsParens). The parens are drawn in kMathGroupOn/Off; the argument itself is rendered
// in the ambient style. `denominator` picks the stricter rule for a \frac denominator (see
// mathArgNeedsParens). A single term or product comes back bare (`2a`, `7`, `x`); a sum, or an
// explicit product in a denominator, comes back parenthesized (`(a + b)`, `(6*7)`).
std::string groupArg(const std::string& arg, MathStyle style, bool denominator = false) {
    std::string inner = renderMath(arg, style);
    if (!mathArgNeedsParens(arg, denominator)) return inner;
    return kMathGroupOn + "(" + kMathGroupOff + inner + kMathGroupOn + ")" + kMathGroupOff;
}

}  // namespace

std::string renderMath(const std::string& tex, MathStyle style) {
    std::string out;
    size_t n = tex.size();
    for (size_t i = 0; i < n;) {
        char c = tex[i];
        if (c == '\\') {
            // Read the command name (letters), or a single-character escape like \{ or \,.
            size_t j = i + 1;
            while (j < n && std::isalpha(static_cast<unsigned char>(tex[j]))) ++j;
            if (j == i + 1) {
                // Non-letter after backslash: \, \; \! are spacing (drop); \{ \} \$ are literals.
                char e = (j < n) ? tex[j] : '\0';
                if (e == ',' || e == ';' || e == ':' || e == '!' || e == ' ') {
                    i = j + 1;
                    continue;
                }
                if (e == '{' || e == '}' || e == '$' || e == '%' || e == '&' || e == '#') {
                    out += e;
                    i = j + 1;
                    continue;
                }
                out += c;
                ++i;
                continue;  // unknown: keep the backslash literally
            }
            std::string cmd = tex.substr(i + 1, j - (i + 1));
            // \quad (1em) and \qquad (2em) are the wide spaces that set separate expressions apart;
            // approximate them with 2 and 4 columns. Plain spaces are fine: the whole expression is
            // a math atom (kMathAtomStart..End) that reflow treats as one indivisible unit, so
            // interior spaces are copied verbatim, never collapsed. A single space would be
            // indistinguishable from the space around an operator, making two expressions read as
            // one.
            if (cmd == "quad") {
                out += "  ";
                i = j;
                continue;
            }
            if (cmd == "qquad") {
                out += "    ";
                i = j;
                continue;
            }
            if (cmd == "left" || cmd == "right") {
                i = j;
                continue;
            }  // delimiter sizing: drop

            // If the command takes a braced argument, read the whole balanced group now. `arg` is
            // the argument contents; `groupEnd` is the position just past the closing '}'. If there
            // is no balanced group, argEnd stays == j (no argument) and we keep the bare command
            // logic.
            std::string arg;
            size_t groupEnd = j;
            bool hasArg = false;
            if (j < n && tex[j] == '{') {
                size_t k = j + 1;
                int depth = 1;
                while (k < n && depth > 0) {
                    if (tex[k] == '{')
                        ++depth;
                    else if (tex[k] == '}') {
                        if (--depth == 0) break;
                    }
                    if (depth > 0) arg += tex[k];
                    ++k;
                }
                if (k < n) {
                    hasArg = true;
                    groupEnd = k + 1;
                }  // balanced; else leave hasArg false
            }

            // \mathbb / \mathcal: each letter must have a Unicode form, or the whole group is kept.
            if (hasArg && (cmd == "mathbb" || cmd == "mathcal")) {
                std::string conv;
                bool ok = !arg.empty();
                for (char ch : arg) {
                    std::string g = mathbbLetter(ch);
                    if (g.empty()) {
                        ok = false;
                        break;
                    }
                    conv += g;
                }
                if (ok) {
                    out += conv;
                    i = groupEnd;
                    continue;
                }
                out += tex.substr(i, groupEnd - i);  // keep \mathbb{...} verbatim, braces and all
                i = groupEnd;
                continue;
            }

            // \mathrm / \mathbf / ...: styling wrappers. Recurse on the argument with the wrapper's
            // font, but only accept the result if it is fully representable (no leftover backslash
            // from an unmapped command); otherwise keep the entire \cmd{...} so a LaTeX reader
            // still sees the intent.
            if (hasArg &&
                (cmd == "mathrm" || cmd == "mathbf" || cmd == "mathit" || cmd == "mathsf" ||
                 cmd == "text" || cmd == "operatorname")) {
                MathStyle inner = cmd == "mathbf" ? MathStyle::Bold
                    : cmd == "mathit"             ? MathStyle::Italic
                                      : MathStyle::Upright;  // mathrm, text, mathsf, operatorname
                std::string conv = renderMath(arg, inner);
                if (conv.find('\\') == std::string::npos) {
                    out += conv;
                    i = groupEnd;
                    continue;
                }
                out += tex.substr(i, groupEnd - i);  // keep \cmd{...} verbatim
                i = groupEnd;
                continue;
            }

            // \sqrt{...}: a radical sign followed by its argument in faint grouping parens. We
            // can't draw an extended radical vinculum over the argument in a terminal, so the
            // parens carry the grouping that LaTeX's overbar would ("√(a² + b²)" is unambiguous,
            // "√a² + b²" is not). The argument is rendered recursively so nested markup still
            // works. Bare \sqrt (no braces) falls through to the symbol table's "√".
            if (hasArg && cmd == "sqrt") {
                out += "√" + groupArg(arg, style);
                i = groupEnd;
                continue;
            }

            // \frac{num}{den}: a terminal has no stacked fraction, so render it inline as
            // "(num)/(den)" with the grouping parens faint. Both arguments are read as balanced
            // groups (the first via the generic arg scan above; the second scanned here) and
            // rendered recursively. If the second argument is missing, fall through to keep the
            // source readable.
            if (hasArg && (cmd == "frac" || cmd == "dfrac" || cmd == "tfrac")) {
                size_t p = groupEnd;
                while (p < n && std::isspace(static_cast<unsigned char>(tex[p]))) ++p;
                if (p < n && tex[p] == '{') {
                    std::string den;
                    size_t k = p + 1;
                    int depth = 1;
                    while (k < n && depth > 0) {
                        if (tex[k] == '{')
                            ++depth;
                        else if (tex[k] == '}') {
                            if (--depth == 0) break;
                        }
                        if (depth > 0) den += tex[k];
                        ++k;
                    }
                    if (k < n) {  // balanced denominator
                        // numerator "/" denominator, each parenthesized only if it contains a top-
                        // level additive operator. The "/" itself is normal foreground (it is the
                        // operator, not grouping); only the parens are faint. The bar is spaced on
                        // both sides so it reads as a same-strength operator alongside \cdot etc.,
                        // rather than binding tighter than them (which unspaced "2a" vs "6 ⋅ 7"
                        // wrongly suggested).
                        out += groupArg(arg, style) + " / " + groupArg(den, style, /*denom=*/true);
                        i = k + 1;
                        continue;
                    }
                }
                // No second argument: keep \frac{...} verbatim so the intent survives.
                out += tex.substr(i, groupEnd - i);
                i = groupEnd;
                continue;
            }

            auto it = mathSymbols().find(cmd);
            if (it != mathSymbols().end()) {
                out += it->second;
                i = j;
                continue;
            }

            // Unknown command: leave it as written, INCLUDING any braced argument, so the source
            // stays readable (e.g. \mathbb{R} -> "\mathbb{R}", not "\mathbbR").
            if (hasArg) {
                out += tex.substr(i, groupEnd - i);
                i = groupEnd;
                continue;
            }
            out += "\\" + cmd;
            i = j;
            continue;
        }
        if (c == '^' || c == '_') {
            if (convertScript(tex, i, out, c == '^')) continue;
            out += c;
            ++i;
            continue;
        }
        if (c == '{' || c == '}') {
            ++i;
            continue;
        }  // bare grouping braces have no visual effect
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            out += mathStyledLetter(c, style);  // Latin letters take the current math font
            ++i;
            continue;
        }
        int len = utf8SequenceLength(static_cast<unsigned char>(c));
        out += tex.substr(i, len);
        i += len;
    }
    return out;
}

namespace {
// ---------------------------------------------------------------------------
// Stacked (multi-line) math layout
// ---------------------------------------------------------------------------
//

// A single inline string as a one-row box whose only row is the baseline.
MathBox inlineBox(const std::string& s) {
    MathBox b;
    b.rows.push_back(s);
    b.width = displayWidth(s);
    b.mid = 0;
    return b;
}

// Build the stacked box for one \frac. The numerator and denominator are rendered inline (no
// grouping parens: the over/under layout already carries the grouping the faint parens stood in
// for, so `\frac{-b \pm ...}{2a}` shows a bare numerator, not "(-b ± ...)"). Each is centered over
// a bar as wide as the wider side.
MathBox fracBox(const std::string& num, const std::string& den, MathStyle style) {
    std::string top = renderMath(num, style);
    std::string bot = renderMath(den, style);
    int w = std::max({displayWidth(top), displayWidth(bot), 1});
    MathBox b;
    b.rows.push_back(padAligned(top, w, Align::Center));
    b.rows.push_back(horizontalLine(w));
    b.rows.push_back(padAligned(bot, w, Align::Center));
    b.width = w;
    b.mid = 1;
    return b;
}

// Concatenate two boxes side by side, aligning their baseline (mid) rows. The taller extent above
// and below the shared baseline is taken from whichever box reaches further; the shorter box's
// missing rows are blank padding of its own width, so columns stay aligned.
MathBox hcat(const MathBox& a, const MathBox& b) {
    int above = std::max(a.mid, b.mid);
    int belowA = static_cast<int>(a.rows.size()) - 1 - a.mid;
    int belowB = static_cast<int>(b.rows.size()) - 1 - b.mid;
    int below = std::max(belowA, belowB);
    MathBox out;
    out.mid = above;
    out.width = a.width + b.width;
    for (int r = -above; r <= below; ++r) {
        auto rowOf = [](const MathBox& box, int rel) -> std::string {
            int idx = box.mid + rel;
            if (idx < 0 || idx >= static_cast<int>(box.rows.size()))
                return std::string(static_cast<size_t>(box.width), ' ');
            return box.rows[static_cast<size_t>(idx)];
        };
        out.rows.push_back(rowOf(a, r) + rowOf(b, r));
    }
    return out;
}

// Read a balanced {...} group starting at tex[i]=='{'. Returns the contents and advances i past the
// closing '}'. If unbalanced, returns false and leaves i unchanged.
bool readBraceGroup(const std::string& tex, size_t& i, std::string& out) {
    if (i >= tex.size() || tex[i] != '{') return false;
    size_t k = i + 1;
    int depth = 1;
    std::string g;
    while (k < tex.size() && depth > 0) {
        if (tex[k] == '{')
            ++depth;
        else if (tex[k] == '}') {
            if (--depth == 0) break;
        }
        if (depth > 0) g += tex[k];
        ++k;
    }
    if (k >= tex.size()) return false;  // unbalanced
    out = g;
    i = k + 1;
    return true;
}

}  // namespace

bool renderMathStacked(const std::string& tex, MathBox& result, MathStyle style) {
    std::vector<MathBox> boxes;
    std::string pending;  // inline text accumulated since the last box
    bool sawFrac = false;
    auto flush = [&]() {
        if (!pending.empty()) {
            boxes.push_back(inlineBox(renderMath(pending, style)));
            pending.clear();
        }
    };
    size_t n = tex.size();
    for (size_t i = 0; i < n;) {
        if (tex[i] == '\\') {
            size_t j = i + 1;
            while (j < n && std::isalpha(static_cast<unsigned char>(tex[j]))) ++j;
            std::string cmd = tex.substr(i + 1, j - (i + 1));
            if (cmd == "frac" || cmd == "dfrac" || cmd == "tfrac") {
                size_t p = j;
                while (p < n && std::isspace(static_cast<unsigned char>(tex[p]))) ++p;
                std::string num;
                if (p < n && tex[p] == '{' && readBraceGroup(tex, p, num)) {
                    while (p < n && std::isspace(static_cast<unsigned char>(tex[p]))) ++p;
                    std::string den;
                    if (p < n && tex[p] == '{' && readBraceGroup(tex, p, den)) {
                        flush();
                        boxes.push_back(fracBox(num, den, style));
                        sawFrac = true;
                        i = p;
                        continue;
                    }
                }
                // Malformed \frac: fall through and let it accumulate as inline text.
            }
            pending += tex.substr(i, j - i);
            i = j;
            continue;
        }
        // Skip a balanced brace group verbatim into `pending` so a top-level {...} does not hide an
        // interior '\frac' we must not stack (only depth-0 fracs stack).
        if (tex[i] == '{') {
            std::string g;
            size_t p = i;
            if (readBraceGroup(tex, p, g)) {
                pending += tex.substr(i, p - i);
                i = p;
                continue;
            }
        }
        pending += tex[i];
        ++i;
    }
    flush();
    if (!sawFrac || boxes.empty()) return false;
    MathBox acc = boxes.front();
    for (size_t k = 1; k < boxes.size(); ++k) acc = hcat(acc, boxes[k]);
    result = acc;
    return true;
}

}  // namespace mdcat
