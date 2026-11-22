// Block parser and document driver.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace mdcat {

// Render a Markdown document, given as its source lines, to `out`.
void render(const std::vector<std::string>& lines, std::ostream& out);

// Read all lines from `in`, tolerating CRLF line endings.
std::vector<std::string> splitLines(std::istream& in);

}  // namespace mdcat
