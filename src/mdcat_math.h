// LaTeX math -> Unicode transliteration and stacked (multi-line) display-math layout.
#pragma once

#include <string>
#include <vector>

namespace mdcat {

// Math styling applied to bare Latin letters. In LaTeX math mode the default is italic; \mathrm /
// \text / \mathsf / \operatorname keep letters upright; \mathbf is bold; \mathit is italic.
enum class MathStyle { Italic, Upright, Bold };

// Best-effort transliteration of a LaTeX math fragment to Unicode. `tex` is the content between the
// delimiters (without the $'s). `style` is the font applied to bare Latin letters (italic by
// default, as in LaTeX math mode; \mathrm and friends recurse with a different style). Always
// returns a renderable string.
std::string renderMath(const std::string& tex, MathStyle style = MathStyle::Italic);

// A display expression ($$...$$) can afford real two-dimensional layout, so a \frac renders as a
// numerator ATOP a denominator, separated by a horizontal bar as wide as the wider of the two —
// the way TeX sets display math — rather than the inline "num / den" flattening. The result is a
// small rectangle of rows (a "MathBox"): rows[mid] is the baseline that aligns with surrounding
// inline text, so a fraction contributes rows above and below that baseline while a plain term
// occupies only the baseline row itself. This lives ONLY on the block-math path; inline $...$ in
// running prose keeps flattening \frac, since a paragraph line has no room to grow vertically.
//
// The scope is deliberately one level: the numerator and denominator are rendered INLINE via
// renderMath (a nested \frac inside them flattens to "a / b"), and only the outermost \frac of a
// display expression stacks. Segments other than \frac (operators, other atoms, \quad spacing)
// render inline on the baseline row and sit beside the stacked fractions.
struct MathBox {
    std::vector<std::string> rows;  // each row padded to `width` display columns
    int width = 0;                  // common display width of every row
    int mid = 0;                    // index of the baseline row (aligns with inline text)
};

// Render a display-math expression into a MathBox, stacking each top-level \frac and rendering
// everything else inline on the baseline. Scans `tex` at brace-depth 0: a `\frac{..}{..}` (also
// dfrac/tfrac) becomes a fracBox; every other run of characters accumulates into an inline segment
// rendered by renderMath. Returns false if the expression has no top-level \frac (the caller then
// keeps the plain single-line path).
bool renderMathStacked(const std::string& tex,
                       MathBox& result,
                       MathStyle style = MathStyle::Italic);

}  // namespace mdcat
