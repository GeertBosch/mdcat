// GFM table rendering.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace mdcat {

// Is `line` a GFM table delimiter row, e.g. |---|:--:| ? It must contain only pipes, colons,
// hyphens and spaces, include at least one hyphen, and at least one pipe.
bool isTableDelimiterRow(const std::string& line);

// Render a table: rawRows[0] is the header, rawRows[1] the delimiter, the rest are data rows.
void emitTable(const std::vector<std::string>& rawRows, std::ostream& out);

}  // namespace mdcat
