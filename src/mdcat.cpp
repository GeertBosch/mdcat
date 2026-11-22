// mdcat - render a GitHub Flavored Markdown subset to an ANSI terminal.
//
// This is a C++17 port of the earlier markfun.sh. Rather than rewriting text with regular
// expressions, it parses the input in two phases that mirror the GFM specification
// (https://github.github.com/gfm):
//
//   1. Block parsing splits the document into a sequence of blocks: ATX headings, thematic
//      breaks, fenced and indented code blocks, GFM tables, and paragraphs (the leaf blocks),
//      plus the container blocks — block quotes and lists — whose stripped contents are rendered
//      recursively (a quote gets a left rule; a list item gets a bullet/number marker and a
//      hanging indent). Blank lines separate blocks; the lines of a paragraph are gathered
//      together so they can be reflowed.
//   2. Inline parsing turns the raw text of headings, paragraphs and table cells into styled
//      runs: emphasis (*x* / _x_), strong emphasis (**x**), code spans (`x`) and links
//      ([text](url)). Code-fence contents are emitted verbatim, not inline-parsed.
//
// Styling is applied with ANSI escape sequences; link targets use OSC 8 hyperlinks. Widths used
// for table layout and paragraph wrapping are measured in display columns, ignoring escapes.
//
// Only the elements the original script dealt with are implemented, made more solid and more
// conformant. As an exception to the "no inline HTML" rule, a paragraph or table cell that
// consists solely of an <img ...> tag pointing at a local PNG is rendered as an actual image by
// shelling out to timg(1) on terminals that support sixel graphics; elsewhere (e.g. Apple's
// Terminal.app) it falls back to the tag's alt text or filename. See renderImageBlock. Supported
// formats: PNG, JPG, GIF, SVG. As a second exception, an inline <br> tag (in any of the <br>,
// <br/>, <br /> spellings) is rendered as a hard line break wherever it appears — see
// scanHardBreak. All other inline HTML is passed through literally. LaTeX math between $...$
// (inline) or $$...$$ (block) is transliterated to Unicode on a best-effort basis — Greek letters,
// common operator and relation symbols, super/subscripts, and \mathrm/\mathbf wrappers; see
// renderMath and scanMath. A ```mermaid fenced code block is rendered as a diagram via the mermaid
// CLI (mmdc) when it and a graphics terminal are available; otherwise it falls back to ordinary
// code. See renderMermaidBlock. On a Kitty-graphics terminal, a flowchart that mermaid.h
// understands is drawn by mdcat itself instead: strokes as one PNG, labels as real text, fitted to
// the width by tighter spacing and wrapped labels, or drawn wider than the terminal if need be.
// --mermaid / $MDCAT_MERMAID picks the chain; $MDCAT_MERMAID_DEBUG=1 reports builtin rejections.
//
// Build:  c++ -std=c++17 -O2 -o mdcat mdcat.cpp
// Usage:  mdcat [--width N] [--img[=kitty|sixel|none]] [--mermaid MODE] [--] [file ...]
//         (reads standard input when given no file arguments)
//         --mermaid auto|builtin|mmdc|source picks how ```mermaid blocks render.
//         --width N forces the render width, overriding $COLUMNS and the terminal size.
//         --img forces image output even when piped; an optional protocol pins the graphics
//         backend.

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gmore_run.h"
#include "highlight.h"
#include "mdcat_blocks.h"
#include "mdcat_image.h"
#include "mdcat_inline.h"
#include "mdcat_math.h"
#include "mdcat_mermaid.h"
#include "mdcat_pool.h"
#include "mdcat_protocol.h"
#include "mdcat_remote.h"
#include "mdcat_slot_sink.h"
#include "mdcat_state.h"
#include "mdcat_style.h"
#include "mdcat_table.h"
#include "mdcat_terminal.h"
#include "mdcat_text.h"
#include "mermaid.h"
#include "tty_guard.h"
#include "unicode_width.h"
#include "width_probe.h"

namespace mdcat {

// ---------------------------------------------------------------------------
// ANSI / OSC helpers
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Block model
// ---------------------------------------------------------------------------

void emitCodeBlock(const std::vector<std::string>& lines,
                   const std::string& lang,
                   std::ostream& out) {
    // Fenced code: print with the light-gray background used for inline code, padded to the
    // width of the widest line so the block reads as a solid panel.  Syntax highlighting is
    // applied when a recognized language tag is present.
    //
    // kCodeOff only resets fg/bg; cancel bold+italic explicitly so highlighted attributes
    // cannot leak past the line boundary into subsequent terminal output.
    const std::string kAttrOff = "\033[22;23m";  // cancel bold (22) and italic (23)
    const auto highlighted = highlightCode(lines, lang);
    int maxw = 0;
    for (const auto& l : lines) maxw = std::max(maxw, displayWidth(l));
    // Clamp the panel width so a row (2-space margins + content) never exceeds the terminal width.
    // Code lines are emitted verbatim (never reflowed), so a line longer than this still overflows
    // and wraps — that is unavoidable — but short lines must not be over-padded to the widest
    // line's width, which would push them past the edge and make the pager wrap them into phantom
    // rows with a broken background (gmore renders into a fixed-width grid; the terminal soft-wraps
    // under cat).
    maxw = std::min(maxw, std::max(1, terminalWidth() - 4));
    for (const auto& l : highlighted) {
        out << kCodeOn << "  " << padTo(l, maxw) << "  " << kAttrOff << kCodeOff << '\n';
    }
}

// ---------------------------------------------------------------------------
// Block parser / document driver
// ---------------------------------------------------------------------------

void render(const std::vector<std::string>& lines, std::ostream& out);

// Drop a single trailing '\n' if present. render() terminates every block with one, so the buffered
// output of a recursive render ends in a newline; removing it keeps splitOnNewlines from yielding a
// spurious empty final line (which would draw an extra bare quote rule).
std::string trimTrailingNewline(const std::string& s) {
    if (!s.empty() && s.back() == '\n') return s.substr(0, s.size() - 1);
    return s;
}

// True if `line` opens a block quote: up to three spaces of indentation, then a '>'. (GFM allows
// the block-quote marker to be indented by at most three spaces; four would be a code indent, which
// this renderer does not implement, but checking the bound keeps the detection honest.)
bool isBlockQuote(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ') ++i;
    return i < line.size() && line[i] == '>';
}

// Strip one block-quote marker from a line that isBlockQuote(): drop up to three leading spaces,
// the '>', and one optional space after it. The remainder is the line's content one level in.
std::string stripBlockQuote(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ') ++i;
    if (i < line.size() && line[i] == '>') ++i;  // the marker itself
    if (i < line.size() && line[i] == ' ') ++i;  // one optional space of padding
    return line.substr(i);
}

// A parsed list-item marker. `markerWidth` is the number of leading columns the marker plus its
// trailing spaces occupy on the first line — i.e. the column the item's content begins at, which is
// also the indent every continuation line and nested block must reach to stay in the item.
struct ListMarker {
    bool ordered = false;    // true for "1." / "1)", false for a "-"/"+"/"*" bullet
    char delim = 0;          // bullet char ('-','+','*') or ordered delimiter ('.'/')')
    int start = 0;           // ordered list's starting number (the digits before the delimiter)
    size_t markerWidth = 0;  // columns from line start to the item's content (the content indent)
};

// If `line` begins a list item, fill `m` and return true. A marker is up to three spaces of indent,
// then either a bullet (-, +, *) or an ordered marker (1-9 digits then '.' or ')'), then at least
// one space (or end of line). The trailing run of spaces is folded into markerWidth so the content
// indent is known. An empty item (marker then end of line) gets a content indent of marker + 1, the
// conventional single space. (We don't implement GFM's 4-space-content rule for over-indented item
// bodies; up to a normal run of spaces after the marker is consumed.)
bool parseListMarker(const std::string& line, ListMarker& m) {
    size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ') ++i;
    if (i >= line.size()) return false;
    size_t markerStart = i;
    if (line[i] == '-' || line[i] == '+' || line[i] == '*') {
        m.ordered = false;
        m.delim = line[i];
        ++i;
    } else if (std::isdigit(static_cast<unsigned char>(line[i]))) {
        size_t d = i;
        while (d < line.size() && std::isdigit(static_cast<unsigned char>(line[d]))) ++d;
        if (d - i > 9 || d >= line.size() || (line[d] != '.' && line[d] != ')')) return false;
        m.ordered = true;
        m.start = std::atoi(line.substr(i, d - i).c_str());
        m.delim = line[d];
        i = d + 1;
    } else {
        return false;
    }
    // The marker must be followed by a space (or be the whole line, an empty item).
    if (i < line.size() && line[i] != ' ') return false;
    (void)markerStart;
    size_t afterMarker = i;
    while (i < line.size() && line[i] == ' ') ++i;
    // An empty item, or one indented by many spaces, still uses a single space of content indent.
    if (i >= line.size() || i - afterMarker == 0)
        m.markerWidth = afterMarker + 1;
    else
        m.markerWidth = i;
    return true;
}

// True if `line` begins a list item of any kind.
bool isListItem(const std::string& line) {
    ListMarker m;
    return parseListMarker(line, m);
}

// Two list markers belong to the same list when they are the same type, and for bullets the same
// bullet character, and for ordered lists the same delimiter ('.'/')'). A change of any of these
// (or a blank line followed by a different type) starts a new list. (GFM also treats a change of
// bullet character as a new list; we follow that.)
bool sameListType(const ListMarker& a, const ListMarker& b) {
    if (a.ordered != b.ordered) return false;
    return a.delim == b.delim;
}

// True if the trimmed line is a valid ATX heading marker (1-6 '#' then space or end of line).
bool isAtxHeading(const std::string& t) {
    size_t h = 0;
    while (h < t.size() && t[h] == '#') ++h;
    return h >= 1 && h <= 6 && (h == t.size() || t[h] == ' ');
}

// True if the trimmed line opens or closes a fenced code block (>= 3 backticks or tildes).
bool isCodeFence(const std::string& t) {
    if (t.size() < 3 || (t[0] != '`' && t[0] != '~')) return false;
    size_t r = 0;
    while (r < t.size() && t[r] == t[0]) ++r;
    return r >= 3;
}

// True if `line` is indented enough to be an indented code line: at least four leading columns of
// indentation, counting a leading tab as reaching the four-column stop. A blank line does not count
// (it is handled separately as a possible interior blank). See GFM 4.4.
bool isIndentedCode(const std::string& line) {
    int cols = 0;
    for (char c : line) {
        if (c == ' ')
            ++cols;
        else if (c == '\t')
            cols += 4;
        else
            break;
        if (cols >= 4) return true;
    }
    return false;  // ran out of leading whitespace before reaching column four (or the line is
                   // blank)
}

// Remove the four-column indent that introduces an indented code block, leaving the rest verbatim.
// A leading tab counts as four columns and is consumed whole; otherwise up to four leading spaces
// are dropped. Content beyond the first four columns is preserved exactly.
std::string stripCodeIndent(const std::string& line) {
    size_t i = 0;
    int cols = 0;
    while (i < line.size() && cols < 4) {
        if (line[i] == ' ') {
            ++cols;
            ++i;
        } else if (line[i] == '\t') {
            cols += 4;
            ++i;
        } else
            break;
    }
    return line.substr(i);
}

// True if the trimmed line is a thematic break: >= 3 of the same -, * or _ (spaces allowed).
bool isThematicBreak(const std::string& t) {
    std::string compact;
    for (char c : t)
        if (c != ' ' && c != '\t') compact += c;
    if (compact.size() < 3) return false;
    return compact.find_first_not_of('-') == std::string::npos ||
        compact.find_first_not_of('*') == std::string::npos ||
        compact.find_first_not_of('_') == std::string::npos;
}

// True if line `j` begins a new leaf block, so a paragraph in progress must stop before it.
bool startsBlock(const std::vector<std::string>& lines, size_t j) {
    const std::string t = trim(lines[j]);
    if (t.empty()) return true;
    if (isAtxHeading(t)) return true;
    if (isCodeFence(t)) return true;
    if (isThematicBreak(t)) return true;
    if (isBlockQuote(lines[j])) return true;
    if (isListItem(lines[j])) return true;
    if (lines[j].find('|') != std::string::npos && j + 1 < lines.size() &&
        isTableDelimiterRow(lines[j + 1]))
        return true;  // a GFM table header row
    return false;
}

// Strip up to `n` leading spaces from `line` (fewer if the line has fewer). Used to remove a list
// item's content indent from its continuation lines so the item's body renders at column zero.
std::string stripIndent(const std::string& line, size_t n) {
    size_t i = 0;
    while (i < n && i < line.size() && line[i] == ' ') ++i;
    return line.substr(i);
}

// Count the leading spaces of `line` (its indentation in columns; tabs are not expanded — the
// inputs here use spaces, matching the rest of the parser).
size_t leadingSpaces(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    return i;
}

// One gathered list item: the content lines with the item's content indent already stripped, plus
// whether a blank line appeared anywhere inside it (which, like a blank line between items, makes
// the whole list loose — GFM 5.3).
struct ListItem {
    std::vector<std::string> lines;
    bool hadBlank = false;
};

// If `s` begins with a GFM task-list checkbox — "[ ]", "[x]" or "[X]" followed by a space (or end
// of the content) — set `checked` and strip the checkbox plus its trailing space, returning true.
// The checkbox must be the very start of the item's content (GFM task-list extension).
bool stripTaskCheckbox(std::string& s, bool& checked) {
    if (s.size() < 3 || s[0] != '[' || s[2] != ']') return false;
    char c = s[1];
    if (c == ' ')
        checked = false;
    else if (c == 'x' || c == 'X')
        checked = true;
    else
        return false;
    if (s.size() == 3) {
        s.clear();
        return true;
    }  // checkbox is the whole content
    if (s[3] != ' ') return false;  // must be followed by whitespace
    s.erase(0, 4);
    return true;
}

// Render a gathered list. Each item's content is a full block sequence rendered recursively (one
// container level deeper) into a buffer; the buffer's first line is prefixed with the item marker
// and the rest with equal-width padding so the body hangs under the marker. Bullets use a
// depth-varying glyph (● ○ ▪︎, matching GitHub's disc/circle/square); ordered items keep their
// number with a '.'. An item whose content opens with a task-list checkbox ("[ ]"/"[x]") swaps the
// marker for a ☐/☑ glyph (GFM task-list extension). gListDepth tracks bullet nesting for the glyph;
// gIndent is raised by the marker width so inner content reflows within the narrower column. A
// loose list prints a blank line between items.
void emitList(const std::vector<ListItem>& items,
              const ListMarker& marker,
              bool loose,
              std::ostream& out) {
    static const std::vector<std::string> kBullets = {"●", "○", "▪︎"};
    // Each list level indents its content by four columns (matching most Markdown viewers), so the
    // marker is padded out to at least four columns. A longer ordered marker (e.g. "10. ") keeps
    // its natural width.
    const int kListIndent = 4;
    // The outermost list block is set off from surrounding text by a two-space left lead; nested
    // lists already step in through their parent item's marker, so they add no further lead.
    const std::string lead = (gListNesting == 0) ? "  " : "";
    gIndent += static_cast<int>(lead.size());
    ++gListNesting;
    for (size_t idx = 0; idx < items.size(); ++idx) {
        if (idx && loose) out << '\n';

        // A task-list item: strip the leading checkbox from its content and remember its state so
        // the marker becomes a checkbox glyph below.
        std::vector<std::string> itemLines = items[idx].lines;
        bool isTask = false, checked = false;
        if (!itemLines.empty() && stripTaskCheckbox(itemLines[0], checked)) isTask = true;

        // Build the marker text and the matching blank padding for continuation lines.
        std::string mark;
        if (isTask) {
            mark = (checked ? "☑" : "☐") + std::string(" ");  // ☑ / ☐
        } else if (marker.ordered) {
            mark = std::to_string(marker.start + static_cast<int>(idx)) + marker.delim + ' ';
        } else {
            mark = kBullets[std::min<size_t>(gListDepth, kBullets.size() - 1)] + std::string(" ");
        }
        // Pad the marker out to the four-column indent so the body hangs at the indented column.
        if (displayWidth(mark) < kListIndent)
            mark += std::string(static_cast<size_t>(kListIndent - displayWidth(mark)), ' ');
        int markWidth = displayWidth(mark);
        std::string pad(static_cast<size_t>(markWidth), ' ');

        std::ostringstream buf;
        gIndent += markWidth;
        if (!marker.ordered) ++gListDepth;
        if (!loose) gTightItem = true;  // suppress lead-paragraph/sublist spacing in a tight item
        render(itemLines, buf);
        if (!marker.ordered) --gListDepth;
        gIndent -= markWidth;

        std::vector<std::string> body = splitOnNewlines(trimTrailingNewline(buf.str()));
        for (size_t k = 0; k < body.size(); ++k)
            out << lead << (k == 0 ? mark : pad) << body[k] << '\n';
    }
    --gListNesting;
    gIndent -= static_cast<int>(lead.size());
}

// Render the gathered lines of a block quote (markers already stripped) one container level deeper.
// The contents are a full sequence of blocks, so they go through render() recursively; the rendered
// output is then prefixed line by line with the quote rule. gIndent is raised by the rule's width
// while the contents render, so paragraphs reflow and rules fill within the narrower inner column.
void emitBlockQuote(const std::vector<std::string>& inner, std::ostream& out) {
    const int prefixWidth = 2;  // displayWidth(kQuoteBar): the bar glyph plus one space
    std::ostringstream buf;
    gIndent += prefixWidth;
    render(inner, buf);
    gIndent -= prefixWidth;
    // Prefix every line of the rendered contents with the quote rule. Blank separator lines between
    // inner blocks get the bar too, so the rule is continuous down the whole quote.
    for (const std::string& line : splitOnNewlines(trimTrailingNewline(buf.str())))
        out << kQuoteBar << line << '\n';
}

void render(const std::vector<std::string>& lines, std::ostream& out) {
    size_t n = lines.size();
    bool emitted = false;  // has any block been printed yet?
    size_t curLine = 0;    // source-line index of the block currently being emitted (for mark)
    // A tight list item suppresses the blank line between its own top-level blocks (its lead
    // paragraph and a nested list). Snapshot the flag and clear it immediately, so this applies
    // only to this render's top level — blocks rendered deeper, and recursive renders, separate
    // normally.
    bool tight = gTightItem;
    gTightItem = false;
    // Print a blank line before every block except the first, so blocks are visually separated
    // without a leading or trailing blank line in the output. A tight item skips that blank.
    // After the separator (so the cursor sits on the block's first row) emit the provenance
    // marker tagging that row with the block's markdown source line: gEmitProvenance gates it
    // to the pager path and gIndent == 0 restricts it to top-level blocks, whose curLine is a
    // real source line the pager can re-render from. See srcAtRow in gmore_emulator.cpp.
    auto separate = [&] {
        if (emitted && !tight) out << '\n';
        emitted = true;
        if (gEmitProvenance && gIndent == 0) out << "\033]9999;" << curLine << "\033\\";
    };
    for (size_t i = 0; i < n;) {
        const std::string& line = lines[i];
        std::string t = trim(line);

        if (t.empty()) {
            ++i;
            continue;
        }  // blank lines separate blocks
        curLine = i;  // the source line separate()'s provenance marker will tag
        if (gIndent == 0) gBlockLine = i + 1;

        // Stream incrementally: flush the output sink before starting the next block, so each block
        // (and any subprocess-rendered image it contains) reaches the terminal as soon as it is
        // ready rather than the whole document being withheld until the slowest image finishes. The
        // previous block is fully written by now and the cursor sits at a line boundary, so a flush
        // here never splits a Kitty APC chunk mid-stream. At the top level `out` is the SlotSink,
        // whose flush commits the finished block as an ordered slot for the writer thread; in
        // recursive renders it is a local ostringstream and the flush is a no-op. A possibly-empty
        // first flush is harmless.
        if (emitted) out.flush();

        // ATX heading: 1-6 '#' followed by a space (or end of line).
        if (isAtxHeading(t)) {
            separate();
            size_t h = 0;
            while (h < t.size() && t[h] == '#') ++h;
            std::string text = trim(t.substr(h));
            // A heading may have an optional closing run of '#'.
            size_t e = text.size();
            while (e > 0 && text[e - 1] == '#') --e;
            if (e < text.size() && (e == 0 || text[e - 1] == ' ')) text = trim(text.substr(0, e));
            emitHeading(static_cast<int>(h), text, out);
            ++i;
            continue;
        }

        // Fenced code block: a run of >= 3 backticks or tildes; closed by a matching fence.
        if (isCodeFence(t)) {
            separate();
            char fence = t[0];
            size_t run = 0;
            while (run < t.size() && t[run] == fence) ++run;
            std::string lang = trim(t.substr(run));  // info string, e.g. "cpp" or "python"
            std::vector<std::string> body;
            size_t j = i + 1;
            for (; j < n; ++j) {
                std::string tj = trim(lines[j]);
                size_t r = 0;
                while (r < tj.size() && tj[r] == fence) ++r;
                if (r >= run && tj.find_first_not_of(fence) == std::string::npos)
                    break;  // closing fence
                body.push_back(lines[j]);
            }
            // A ```mermaid block renders per gMermaidMode: the built-in renderer, then a diagram
            // via the mermaid CLI when it (and a graphics terminal) are available, then ordinary
            // code.
            std::string mlang = lang;
            for (char& c : mlang)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            int mAvailW = terminalWidth();
            if (mlang == "mermaid" && gMermaidMode == MermaidMode::Source) {
                emitCodeBlock(body, lang, out);
            } else if (mlang == "mermaid") {
                if (gMermaidMode != MermaidMode::Mmdc) {
                    std::string why;
                    bool tooComplex = false;
                    if (emitBuiltinMermaid(body, out, why, tooComplex)) {
                        i = (j < n) ? j + 1 : j;  // skip the closing fence if present
                        continue;
                    }
                    // Inside a list or quote the lines are re-based, so the location is that of
                    // the enclosing top-level block.
                    if (std::getenv("MDCAT_MERMAID_DEBUG"))
                        std::cerr << "mdcat: " << (gSourceName.empty() ? "<stdin>" : gSourceName)
                                  << ":" << gBlockLine << ": mermaid: fallback: " << why << "\n";
                    if (gMermaidMode == MermaidMode::Builtin || tooComplex) {
                        emitCodeBlock(body, lang, out);
                        i = (j < n) ? j + 1 : j;
                        continue;
                    }
                }
                // The conversion produces the final bytes: the framed diagram on success, else the
                // ordinary code block (the fallback). Computing both in the worker lets the whole
                // block be a single deferred slot (ADR 0003). emitCodeBlock is pure here (top-level
                // gIndent==0, terminalWidth memoized), so it is safe to run off-thread into a local
                // buffer.
                auto convert = [body, lang, mAvailW](uint32_t kid) {
                    std::string mimg;
                    int miw = 0, mih = 0;
                    if (renderMermaidBlock(body, mAvailW, mimg, miw, mih, kid) &&
                        isImageBlock(mimg))
                        return imageParagraphBytes(mimg, mih);
                    std::ostringstream cb;
                    emitCodeBlock(body, lang, cb);
                    return cb.str();
                };
                if (SlotSink* sink = asSlotSink(out)) {
                    // Defer the Kitty id (kKittyIdDefer): the writer assigns a document-order id
                    // when it drains the slot, so out-of-order completion stays serial-identical.
                    slotDeferImage(
                        sink, imagePool().submit([convert] { return convert(kKittyIdDefer); }));
                } else {
                    out << imagePool().submit([convert] { return convert(0); }).get();
                }
            } else {
                emitCodeBlock(body, lang, out);
            }
            i = (j < n) ? j + 1 : j;  // skip the closing fence if present
            continue;
        }

        // Thematic break: a line of three or more -, * or _ (optionally spaced).
        if (isThematicBreak(t)) {
            separate();
            emitThematicBreak(out);
            ++i;
            continue;
        }

        // Block quote: one or more lines starting with '>'. Gather the run, including lazy
        // continuation lines — a non-blank line without its own '>' that continues a paragraph
        // inside the quote (GFM 5.1). A blank line, or an unquoted line that itself starts a new
        // block, ends the quote. The markers are stripped and the contents rendered recursively.
        if (isBlockQuote(line)) {
            separate();
            std::vector<std::string> inner;
            size_t j = i;
            for (; j < n; ++j) {
                if (isBlockQuote(lines[j])) {
                    inner.push_back(stripBlockQuote(lines[j]));
                } else if (!trim(lines[j]).empty() && !startsBlock(lines, j)) {
                    inner.push_back(lines[j]);  // lazy paragraph continuation
                } else {
                    break;
                }
            }
            emitBlockQuote(inner, out);
            i = j;
            continue;
        }

        // List: a run of items sharing a marker type (GFM 5.3). Reached only after the
        // thematic-break check above, so a line like "- - -" or "***" is a rule, not a one-item
        // list. Each item is gathered as the content lines belonging to it (its content indent
        // stripped); a line at the list's own indent that opens a same-type marker starts the next
        // item; one that opens a different-type marker, a non-indented block start, or follows a
        // blank line at a lower indent ends the list. The list is loose if any blank line falls
        // between items or inside an item.
        if (isListItem(line)) {
            separate();
            ListMarker listMarker;
            parseListMarker(line, listMarker);  // defines the list's type for the whole run

            std::vector<ListItem> items;
            bool loose = false;
            size_t j = i;
            while (j < n) {
                ListMarker m;
                // A new item: a marker line at this list's indent level and matching type.
                if (parseListMarker(lines[j], m) &&
                    leadingSpaces(lines[j]) < listMarker.markerWidth) {
                    if (!sameListType(m, listMarker))
                        break;  // different marker type: a separate list
                    ListItem item;
                    // The first line keeps everything after the marker (drop the marker itself, not
                    // just leading spaces — there are none before it at this point).
                    item.lines.push_back(lines[j].substr(std::min(m.markerWidth, lines[j].size())));
                    size_t k = j + 1;
                    bool pendingBlank =
                        false;  // a blank line seen but not yet committed to the item
                    for (; k < n; ++k) {
                        if (trim(lines[k]).empty()) {
                            pendingBlank = true;
                            continue;
                        }
                        size_t ind = leadingSpaces(lines[k]);
                        if (ind >= m.markerWidth) {
                            // Indented to (or past) the content column: part of this item's body. A
                            // committed blank line before it is preserved and makes the list loose.
                            if (pendingBlank) {
                                item.hadBlank = true;
                                item.lines.push_back("");
                                pendingBlank = false;
                            }
                            item.lines.push_back(stripIndent(lines[k], m.markerWidth));
                        } else if (!pendingBlank && !startsBlock(lines, k)) {
                            // Lazy continuation: an unindented paragraph line directly following
                            // the item's text (no blank between) still belongs to its paragraph.
                            item.lines.push_back(lines[k]);
                        } else {
                            break;  // unindented block start, marker, or post-blank line: item
                                    // ends.
                        }
                    }
                    if (item.hadBlank) loose = true;
                    // A blank line separating this item from a following same-type item at the
                    // list's own indent makes the whole list loose (GFM 5.3).
                    if (pendingBlank && k < n) {
                        ListMarker next;
                        if (parseListMarker(lines[k], next) &&
                            leadingSpaces(lines[k]) < listMarker.markerWidth &&
                            sameListType(next, listMarker))
                            loose = true;
                    }
                    items.push_back(std::move(item));
                    j = k;
                } else {
                    break;
                }
            }
            emitList(items, listMarker, loose, out);
            i = j;
            continue;
        }

        // GFM table: a header row whose next line is a delimiter row. Gather contiguous rows.
        if (line.find('|') != std::string::npos && i + 1 < n && isTableDelimiterRow(lines[i + 1])) {
            separate();
            std::vector<std::string> rows;
            rows.push_back(line);          // header
            rows.push_back(lines[i + 1]);  // delimiter
            size_t j = i + 2;
            for (; j < n; ++j) {
                if (trim(lines[j]).empty() || lines[j].find('|') == std::string::npos) break;
                rows.push_back(lines[j]);
            }
            emitTable(rows, out);
            i = j;
            continue;
        }

        // Indented code block: lines indented four or more columns, rendered verbatim (GFM 4.4).
        // Reached only at a fresh block position — an indented line right after a paragraph line is
        // gathered as that paragraph's lazy continuation instead, because isIndentedCode is
        // deliberately NOT part of startsBlock(), so a code block cannot interrupt a paragraph.
        // Interior blank lines are kept; trailing blank lines are dropped. The four-column indent
        // is stripped from each line and the rest emitted exactly, reusing the fenced-code panel
        // style.
        if (isIndentedCode(line)) {
            separate();
            std::vector<std::string> body;
            size_t j = i;
            for (; j < n; ++j) {
                if (trim(lines[j]).empty()) {
                    body.push_back(std::string());
                    continue;
                }
                if (!isIndentedCode(lines[j])) break;
                body.push_back(stripCodeIndent(lines[j]));
            }
            while (!body.empty() && body.back().empty()) body.pop_back();  // drop trailing blanks
            emitCodeBlock(body, "", out);
            // Resume right after the lines that became code; any trailing blank lines we gathered
            // but popped are left for the main loop, which treats blank lines as block separators.
            i += body.size();
            continue;
        }

        // Paragraph: line i is not a block start, so consume it and any following lines until a
        // blank line or the start of another block. The same startsBlock() predicate that the
        // dispatcher relies on is reused here, so the two can never disagree (which previously
        // produced an empty paragraph and an infinite loop on lines like "#620](...)").
        separate();
        std::vector<std::string> para;
        size_t j = i;
        do {
            para.push_back(lines[j]);
            ++j;
        } while (j < n && !startsBlock(lines, j));
        emitParagraph(para, out);
        i = j;
    }
}

std::vector<std::string> splitLines(std::istream& in) {
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // tolerate CRLF
        lines.push_back(line);
    }
    return lines;
}

void usage() {
    std::cerr << "usage: mdcat [-o OUTFILE] [--width N] [--img[=kitty|sixel|none]]\n"
                 "             [--mermaid auto|builtin|mmdc|source] [--] [file ...]\n";
}

[[noreturn]] void error(int code, const std::string& msg) {
    std::cerr << "mdcat: " << msg << "\n";
    usage();
    exit(code);
}

// Map a --mermaid / MDCAT_MERMAID mode name; false for an unrecognized value.
MermaidMode parseMermaidMode(const std::string& s) {
    if (s == "auto") return MermaidMode::Auto;
    if (s == "builtin") return MermaidMode::Builtin;
    if (s == "mmdc") return MermaidMode::Mmdc;
    if (s == "source") return MermaidMode::Source;
    error(2, std::string("invalid mermaid mode: ") + s);
}


int parseOptions(int argc, char** argv) {
    // Supported: --width N / -w N / --width=N to force the render width (overriding $COLUMNS
    // and the terminal size), and -- to end options so a filename may begin with a dash. Option
    // parsing stops at the first non-option operand.

    // Map a --img protocol argument to a backend, or return -2 for an unrecognized value.
    auto parseBackend = [](const std::string& s) -> int {
        if (s == "kitty") return static_cast<int>(GraphicsBackend::Kitty);
        if (s == "sixel") return static_cast<int>(GraphicsBackend::Sixel);
        if (s == "none") return static_cast<int>(GraphicsBackend::None);
        return -2;
    };

    int a = 1;
    for (; a < argc; ++a) {
        std::string arg = argv[a];
        if (arg == "--") {
            ++a;
            break;
        }
        if (arg.size() < 2 || arg[0] != '-') break;  // first operand: stop option parsing

        if (arg == "-o") {
            if (a + 1 >= argc) error(2, "-o requires a filename");
            gOutputFile = argv[++a];
            continue;
        }

        if (arg == "--img") {  // force image output even when piped
            gForceGraphics = true;
            // Optional protocol argument: only consume the next token if it names a backend,
            // so `--img file.md` still treats file.md as the operand.
            if (a + 1 < argc) {
                int b = parseBackend(argv[a + 1]);
                if (b >= 0) {
                    gForcedBackend = b;
                    ++a;
                }
            }
            continue;
        }
        if (arg.rfind("--img=", 0) == 0) {  // inline protocol: --img=kitty|sixel|none
            int b = parseBackend(arg.substr(6));
            if (b < 0) error(2, "invalid --img protocol: " + arg.substr(6));
            gForceGraphics = true;
            gForcedBackend = b;
            continue;
        }

        if (arg == "--mermaid" || arg.rfind("--mermaid=", 0) == 0) {  // flag wins over the env
            std::string mode;
            if (arg == "--mermaid") {
                if (a + 1 >= argc) error(2, "--mermaid requires a mode");
                mode = argv[++a];
            } else {
                mode = arg.substr(10);
            }
            gMermaidMode = parseMermaidMode(mode);

            continue;
        }

        std::string val;
        if (arg == "-w" || arg == "--width") {  // value is the next argument
            if (a + 1 >= argc) error(2, arg + " requires a value");
            val = argv[++a];
        } else if (arg.rfind("--width=", 0) == 0) {  // value is inline after '='
            val = arg.substr(8);
        } else
            error(2, "unknown option: " + arg);
        if (!val.empty()) {
            int w = std::atoi(val.c_str());
            if (w <= 0) error(2, "invalid width: " + val);
            gWidthOverride = w;
        }
    }
    return a;
}

int renderPaged(std::vector<std::string> files) {
    // On a TTY, page through gmore by default. A render THREAD (not a forked child, so the
    // source and the renderer stay resident) streams bytes into a pipe while gmore parses the
    // read end and paints the first page before the whole document is ready. gmore holds the
    // source via the relayout callback: on a window resize it re-renders at the new width and
    // restores the reading position by source-line provenance (see gEmitProvenance / gmore).
    // Read the source ONCE into re-renderable segments (one per file, so `mdcat a b` keeps
    // rendering each independently — never merging a's last block with b's first). Held for
    // the life of the pager so every re-layout renders from the same source.
    struct Segment {
        std::string fileDir;
        std::vector<std::string> lines;
        std::string name;
    };
    std::vector<Segment> segments;
    if (!files.empty()) {
        for (const auto& path : files) {
            std::ifstream f(path);
            if (!f) {
                std::cerr << "mdcat: " << path << ": cannot open file\n";
                return 1;
            }
            size_t slash = path.rfind('/');
            segments.push_back({slash != std::string::npos ? path.substr(0, slash) : std::string(),
                                splitLines(f),
                                path});
        }
    } else {
        segments.push_back({std::string(), splitLines(std::cin), std::string()});
    }

    // Page one segment (file) at a time: each gets its own render thread/pipe/gmore::run(),
    // so advancing past the end of a file only ever needed that file's bytes, never the rest
    // of the argument list. Like less(1) with multiple operands, the prompt shows "(file x of
    // y)" and 'q' quits the whole session while reaching a file's end just advances to the
    // next one.
    for (size_t idx = 0; idx < segments.size(); ++idx) {
        Segment& seg = segments[idx];
        std::string promptSuffix;
        if (segments.size() > 1) {
            promptSuffix = " (file " + std::to_string(idx + 1) + " of " +
                std::to_string(segments.size()) + ")";
        }

        // Render `seg` at `width` into a fresh pipe on a background thread; return the read
        // end for gmore to ingest. Provenance markers are emitted (paging path) so the grid
        // rows carry their source line. Only one render thread runs at a time — the caller
        // joins the previous one before starting the next (see joinRender), so gRenderWidth
        // and the other render globals are never touched concurrently.
        std::thread renderThread;
        auto joinRender = [&] {
            if (renderThread.joinable()) renderThread.join();
        };
        auto startRender = [&](int width) -> int {
            joinRender();
            int pfd[2];
            if (pipe(pfd) != 0) {
                std::perror("mdcat: pipe");
                return -1;
            }
            // SIGWINCH must reach gmore's main thread (it drives re-layout), never the render
            // thread. A newly created thread inherits the creator's signal mask, so block
            // SIGWINCH on THIS (main) thread across the spawn — the render thread inherits
            // the block — then restore the main thread's mask below. Masking from inside the
            // render thread instead is both racy (it runs asynchronously) and, on this
            // platform, was observed to leave SIGWINCH blocked on the main thread, silently
            // killing resize handling. Blocking here and unblocking here keeps the mask
            // change on one thread and deterministic.
            sigset_t block, prev;
            sigemptyset(&block);
            sigaddset(&block, SIGWINCH);
            pthread_sigmask(SIG_BLOCK, &block, &prev);
            renderThread = std::thread([&seg, width, wfd = pfd[1]] {
                gRenderWidth = width;    // re-layout width for this pass (0 elsewhere)
                gEmitProvenance = true;  // tag rows with their source line for resize anchoring
                SlotSink sink(wfd);
                std::ostream out(&sink);
                gFileDir = seg.fileDir;
                gSourceName = seg.name;
                render(seg.lines, out);
                out.flush();
                gFileDir.clear();
                gSourceName.clear();
                sink.finish();
                close(wfd);
            });
            pthread_sigmask(SIG_SETMASK, &prev, nullptr);  // main handles SIGWINCH again
            return pfd[0];
        };

        int rfd = startRender(fullTerminalWidth());
        if (rfd < 0) return 1;
        gmore::Relayout relayout = [&](int width) { return startRender(width); };
        bool quitRequested = false;
        int pagerRc = gmore::run(
            std::string(), false, false, false, false, rfd, relayout, promptSuffix, &quitRequested);
        close(rfd);
        joinRender();
        if (pagerRc != 0 || quitRequested) return pagerRc;
    }
    return 0;
}

int renderAll(std::vector<std::string> files, std::ostream& out) {
    if (files.empty()) return render(splitLines(std::cin), out), 0;
    for (const auto& path : files) {
        std::ifstream f(path);
        if (!f) error(1, path + ": cannot open file");
        size_t slash = path.rfind('/');
        gFileDir = (slash != std::string::npos) ? path.substr(0, slash) : std::string();
        gSourceName = path;
        render(splitLines(f), out);
    }
    gFileDir.clear();
    gSourceName.clear();
    return 0;
}

}  // namespace mdcat

using namespace mdcat;

int main(int argc, char** argv) {
    if (auto e = std::getenv("MDCAT_MERMAID")) gMermaidMode = parseMermaidMode(e);

    // Collect the file operands, parsing options first.
    auto optind = parseOptions(argc, argv);

    std::vector<std::string> files;
    for (auto a = optind; a < argc; ++a) files.emplace_back(argv[a]);

    // Eagerly initialize terminal-query state before rendering starts. This avoids deferred
    // probe traffic while the pager is active and keeps worker threads off terminal I/O.
    (void)graphicsBackend();
    (void)cellMetrics();
    (void)queryBackgroundColor();
    initTheme();

    bool usePager = isatty(STDOUT_FILENO) && gOutputFile.empty();
    // Measure text the way this terminal does (grapheme clusters or code point by code point),
    // so a line laid out to fit never wraps there. The built-in pager shares the choice.
    uw::initMode(usePager);

    if (usePager) return renderPaged(files);

    // Non-tty: stream blocks out as they are rendered. Each file is rendered independently and
    // the results concatenated — the last block of one file can never merge with the first of
    // the next — which makes rendering distribute over the argument list (`mdcat a b` == `mdcat
    // a` then `mdcat b`), as checked by tests/property-concat.sh. SlotSink reserves an ordered
    // slot per block and drains them on a dedicated writer thread, so a deferred image
    // (standalone <img>, mermaid) returns immediately while its conversion runs on the pool and
    // a stalled write() blocks only the writer (ADR 0003). Output stays in document order and
    // byte-identical to the old serial StreamingSink; only timing changes.
    int fd = gOutputFile.empty() ? STDOUT_FILENO
                                 : open(gOutputFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) error(errno, "cannot open output file " + gOutputFile);
    SlotSink sink(fd);
    std::ostream out(&sink);
    if (renderAll(std::move(files), out) != 0) return 1;
    out.flush();    // commit the final block's text as a slot
    sink.finish();  // flush trailing text, drain all slots, join the writer thread
    return 0;
}
