// Block-level renderers for the leaf blocks: paragraphs, ATX headings and thematic breaks.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace mdcat {

// Render a paragraph (its source lines, joined and reflowed to the terminal width).
void emitParagraph(const std::vector<std::string>& lines, std::ostream& out);

// A block (heading or paragraph) whose text begins with a Markdown image, e.g.
// "![Logo](logo.png) MongoDB" or "# ![Logo](logo.png) MongoDB", is rendered as a two-column,
// one-row layout: the image on the left, the rest of the text vertically centered to its right.
// This reuses the same reserve-then-overlay technique as emitTable's image cells (see
// replaySixel): text is laid out first with the image column reserved as blank padding, then the
// sixel is painted into that reserved space with relative cursor moves, so it works the same
// regardless of how the terminal advances the cursor after a sixel. `rule`, if non-empty, is
// appended as a trailing horizontal line (for headings). Returns "" if `text` does not start with
// an image tag, or the image could not be rendered as a real graphic, so the caller falls back to
// plain text.
std::string renderLeadingImageBlock(const std::string& text,
                                    int availW,
                                    const std::string& fileDir,
                                    bool bold,
                                    const std::string& rule,
                                    uint32_t kittyId = 0);

// Render an ATX heading of the given level: bold text plus an underline rule.
void emitHeading(int level, const std::string& text, std::ostream& out);

// Render a thematic break as a full-width horizontal line.
void emitThematicBreak(std::ostream& out);

}  // namespace mdcat
