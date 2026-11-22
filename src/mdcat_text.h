// UTF-8 / display-width utilities and the paragraph reflow machinery.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mdcat {

// Lowercase a string in place-returning copy form.
std::string toLowerCopy(const std::string& s);

// Number of bytes in the UTF-8 sequence whose lead byte is c (1 for invalid lead bytes).
int utf8SequenceLength(unsigned char c);

// Decode the UTF-8 code point beginning at s[i] into cp, returning the byte length consumed (>=1).
// An invalid/truncated sequence yields the single lead byte as cp with length 1 (best-effort).
int decodeUtf8(const std::string& s, size_t i, uint32_t& cp);

// Display width of a string in terminal columns, skipping ANSI escape (CSI) sequences and OSC 8
// hyperlink sequences so that styled text measures by what is actually shown. Each code point is
// measured by codePointWidth, so fullwidth emoji / CJK count as two columns and combining marks,
// joiners and variation selectors as zero — keeping tables and reflow aligned with what the
// terminal actually draws.
int displayWidth(const std::string& s);

// Append n copies of the box-drawing horizontal line character to out.
std::string horizontalLine(int n, const std::string& bar = "─");

std::string padTo(const std::string& s, int width);

// Per-column text alignment derived from a GFM delimiter row (:--, --:, :-:).
enum class Align { Left, Right, Center };

// Pad `s` to `width` display columns, placing the content per `a`. Spaces are added before and/or
// after; padding never appears inside the styled content, so ANSI escapes are unaffected. A cell
// wider than `width` is returned unchanged (layout sizes columns to fit, so this is the rare
// re-render-rounding case, where overflowing is preferable to truncating).
std::string padAligned(const std::string& s, int width, Align a);

// Trim ASCII spaces and tabs from both ends.
std::string trim(const std::string& s);

// Encode a Unicode code point as UTF-8 and append it to `out`.
void appendUtf8(std::string& out, unsigned cp);

// Whitespace test usable on signed char without the locale surprises of std::isspace.
inline bool isSpaceCh(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Length in bytes of the next display "unit" at offset i: a whole ANSI CSI escape, a whole OSC 8
// hyperlink escape, or a single grapheme — a base UTF-8 code point plus any trailing zero-width
// followers (combining marks, variation selectors) and ZWJ-joined code points (e.g. a flag or a
// family emoji). Keeping the grapheme intact means reflow never splits a base from its marks or
// breaks a ZWJ emoji sequence across a line, and the unit's display width stays correct.
size_t unitLength(const std::string& s, size_t i);

// Keep ANSI styling from bleeding across the line breaks that reflow (or table layout) introduces.
// A code span carries a background colour (kCodeOn .. kCodeOff); if a span wraps, the line would
// end with the background still on and paint gray to the right edge of the terminal. Walk the text
// line by line, tracking whether a code span is open at each '\n': close it before the break and
// reopen it after, so each line is self-contained and no background extends past its text.
std::string closeStylesAtLineBreaks(const std::string& s);

// Reflow a rendered (ANSI-styled) string so that no output line exceeds `width` display columns,
// breaking on spaces between words. A word longer than `width` on its own is hard-cut at exactly
// `width` columns and continued on the next line. ANSI escapes don't count toward the width and
// are never split. Line breaks in the result are '\n'; the result has no trailing newline.
std::string reflow(const std::string& s, int width);

// Remove the math-atom sentinels (kMathAtomStart / kMathAtomEnd) from a fully laid-out string, just
// before it is written to the terminal. They exist only to keep reflow from breaking a math
// expression apart; once line breaking is done they have no purpose and must not reach the terminal
// (some fonts render Private-Use code points as a visible box). Zero-width, so removing them never
// changes any measured width.
std::string stripMathAtoms(const std::string& s);

// Split a string on '\n' into its constituent lines (no trailing empty element added for a final
// newline; an empty string yields a single empty line).
std::vector<std::string> splitOnNewlines(const std::string& s);

}  // namespace mdcat
