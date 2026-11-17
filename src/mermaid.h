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
    int availCols = 80;         // columns the diagram may occupy
    int cellW = 8, cellH = 16;  // real pixels per character cell
    bool darkTheme = false;     // stroke colour follows the theme
    std::function<int(const std::string&)>
        width;  // display width of a label (mdcat's displayWidth)
};

struct Diagram {
    // `rows` entries, each exactly `cols` columns wide. A diagram title is wrapped in SGR bold
    // escapes, which take no columns.
    std::vector<std::string> textRows;
    int cols = 0, rows = 0;
    std::string png;  // cols*cellW x rows*cellH pixels, transparent background
};

// Render the body of a ```mermaid block. Returns false, leaving `out` unspecified, when the
// diagram is outside the supported subset, wider than env.availCols, or fails validation; `reason`
// then says why.
bool render(const std::vector<std::string>& body,
            const Env& env,
            Diagram& out,
            std::string& reason);

}  // namespace mermaid
