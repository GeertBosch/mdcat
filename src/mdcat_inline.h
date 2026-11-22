// Inline Markdown parsing: emphasis, code spans, links, escapes -> ANSI-styled text.
#pragma once

#include <string>

namespace mdcat {

// Render markdown inline content from raw text into ANSI-styled output.
//
// The renderer runs in two linear passes so that its cost is O(n) in the input length, with no
// recursion or whole-buffer copying:
//
//   Pass 1 (tokenize): scan left to right producing a flat list of tokens. Backslash escapes,
//   code spans and links are resolved immediately (they don't nest emphasis the way emphasis
//   nests them, and their bodies are short). Every run of '*' or '_' becomes a Delim token whose
//   open/close capability is decided from the characters flanking it.
//
//   Pass 2 (resolve emphasis): walk the tokens with a stack of open delimiters, matching each
//   closer to the nearest compatible opener (the standard GFM algorithm). Matched pairs are
//   marked as strong or emphasis; unmatched delimiters stay literal. A final pass emits text.
//
// Emphasis uses a practical subset of the GFM flanking rules: a delimiter run can open if it is
// not followed by whitespace and can close if it is not preceded by whitespace; '_' additionally
// may not open/close inside a word, so snake_case is left intact. '**' is strong, '*'/'_' is
// emphasis; a run of length >= 2 can serve as either by consuming two or one of its characters.
std::string renderInline(const std::string& text);

}  // namespace mdcat
