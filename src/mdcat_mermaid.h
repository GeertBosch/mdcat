// Mermaid diagram blocks: the mmdc image path and the built-in renderer.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace mdcat {

// Render the source of a ```mermaid fenced code block as a diagram via mmdc(1) and the <img>
// pipeline, writing the raw image bytes to `out` and the painted cell footprint to
// `cellWidth`/`cellHeight`. Returns false (so the caller can fall back to ordinary fenced code)
// when mmdc is missing or erroring or timg fails. `kittyId` is as for renderImageBlock.
bool renderMermaidBlock(const std::vector<std::string>& lines,
                        int availWidth,
                        std::string& out,
                        int& cellWidth,
                        int& cellHeight,
                        uint32_t kittyId = 0);

// Draw a ```mermaid block with the built-in renderer (mermaid.h) and write it centred. Returns
// false, writing nothing and setting `reason`, when the builtin does not apply. `tooComplex` is
// set when the diagram has more back edges or crossings than its width allows.
bool emitBuiltinMermaid(const std::vector<std::string>& body,
                        std::ostream& out,
                        std::string& reason,
                        bool& tooComplex);

}  // namespace mdcat
