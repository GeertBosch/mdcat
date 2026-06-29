#pragma once
#include <string>
#include <vector>

/**
 * Syntax-highlight a block of code lines for the given language tag (e.g. "cpp", "python").
 * Returns lines with embedded ANSI color sequences. The caller is responsible for the
 * surrounding kCodeOn/kCodeOff block background; this function only emits fg-color spans
 * inside that background. If the language is unknown or empty the lines are returned
 * unchanged.
 */
std::vector<std::string> highlightCode(const std::vector<std::string>& lines,
                                       const std::string& lang);

/**
 * Select the syntax-highlight colour palette for the terminal theme. Call once, before any
 * highlightCode, after the background is known. dark=true uses lightened hues + a light-gray
 * code fg; dark=false (the default) keeps the historical palette for a light background.
 */
void setHighlightTheme(bool dark);
