// Built-in rendering of Mermaid flowcharts: shapes and arrows become one transparent RGBA raster
// (a PNG) for a Kitty-protocol terminal, and labels become ordinary text in the character cells
// over it. Only a small subset of Mermaid is understood; anything else is declined so the caller
// can fall back to an image or to showing the source. See docs/plan-mermaid-flowcharts.md and ADR
// 0004.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace mermaid {

struct Env {
    int availCols = 80;         // columns the diagram should fit in
    int cellW = 8, cellH = 16;  // real pixels per character cell
    bool darkTheme = false;     // stroke colour follows the theme
    std::function<int(const std::string&)>
        width;  // display width of a label (mdcat's displayWidth)
    // A Markdown string as styled lines no wider than the given width, each with its styles closed
    // at its end and reopened at the start of the next (mdcat's reflow of renderInline).
    std::function<std::vector<std::string>(const std::string&, int)> markdownLines;
};

struct Diagram {
    // `rows` entries, each exactly `cols` columns wide. A diagram title is wrapped in SGR bold
    // escapes, which take no columns.
    std::vector<std::string> textRows;
    int cols = 0, rows = 0;
    std::string png;  // cols*cellW x rows*cellH pixels, transparent background
};

// Render the body of a ```mermaid block. Returns false, leaving `out` unspecified, when the
// diagram is outside the supported subset or fails validation; `reason` then says why. A diagram
// is fitted to env.availCols with tighter spacing and wrapped labels; if that fails it is drawn
// wider than env.availCols. `why` says what kind of decline it is: Unsupported ones are left to an
// image from mmdc, TooComplex ones (too many back edges or crossings for the width) are better
// shown as source than redrawn by anything else.
enum class Decline { Unsupported, TooComplex };
bool render(const std::vector<std::string>& body,
            const Env& env,
            Diagram& out,
            std::string& reason,
            Decline* why = nullptr);

}  // namespace mermaid
