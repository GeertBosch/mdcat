// ANSI style constants shared by the mdcat modules. kCodeOn and kLightGray are mutable (set by
// initTheme); their definitions live in mdcat_state.cpp for now.
#pragma once

#include <string>

namespace mdcat {

inline constexpr char kEsc = '\033';

// Math-atom sentinels. A rendered math expression is a self-contained unit that must NOT be broken
// or respaced by reflow(): it is laid out like an inline image formed of characters (and will grow
// a height dimension once stacked fractions/matrices arrive), so its internal spacing is exact and
// meaningful, not prose whitespace to be collapsed. renderInline brackets each math span with these
// two Private-Use markers; reflow's unitLength treats everything between them as ONE indivisible
// unit (so interior spaces survive), displayWidth counts the interior but skips the markers, and a
// final stripMathAtoms() pass removes the markers just before output. Zero-width by codePointWidth
// so they never affect measurement. U+E000/U+E001 (UTF-8: EE 80 80 / EE 80 81).
inline const std::string kMathAtomStart = "\xEE\x80\x80";  // U+E000
inline const std::string kMathAtomEnd = "\xEE\x80\x81";    // U+E001

inline const std::string kBoldOn = "\033[1m";
inline const std::string kBoldOff = "\033[22m";
inline const std::string kItalicOn = "\033[3m";
inline const std::string kItalicOff = "\033[23m";
// Code-block colours depend on the terminal theme (see initTheme, called from main once the OSC 11
// background is known). Light theme: dark-gray text on light-gray. Dark theme: light-gray text on
// dark-gray. Both are set before any rendering, so reads during rendering see the chosen palette.
extern std::string kCodeOn;  // default: light-gray bg, dark-gray fg (light theme)
inline const std::string kCodeOff = "\033[39;49m";
inline const std::string kReset = "\033[0m";
extern std::string
    kLightGray;  // table row separators; lightened on a dark theme (default "\033[38;5;250m")
inline const std::string kQuoteBar =
    "\033[38;5;244m▎\033[0m ";  // the left rule drawn before quoted lines
// Grouping parentheses that renderMath adds around \sqrt / \frac arguments (our Unicode transliter-
// ation can't draw an extended radical or a stacked fraction, so braces become parens instead).
// Faint so the grouping reads as structure, not content, but they MUST NOT vanish: they carry the
// grouping. Earlier theme-specific grays (254 near-white on light, 235/242 near-black on dark) were
// each invisible on the OPPOSITE background — and theme detection (OSC 11) fails silently over SSH,
// under pipes, and on some terminals, falling back to the light value on a dark screen (which is
// where the parens disappeared, iTerm). So we use one theme-independent mid-gray (245) with real
// contrast against both a light and a dark background, and no SGR dim (which only pushed the glyph
// further toward the background). No initTheme override.
inline const std::string kMathGroupOn =
    "\033[38;5;245m";                                 // faint but theme-independent grouping parens
inline const std::string kMathGroupOff = "\033[39m";  // restore default fg (keeps other SGR)

}  // namespace mdcat
