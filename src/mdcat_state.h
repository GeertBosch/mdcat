// Option and render-context globals shared by the mdcat modules (set by main(), read while
// rendering). Defined in mdcat_state.cpp.
#pragma once

#include <cstddef>
#include <string>

namespace mdcat {

// An explicit width from the --width/-w command-line flag, or <= 0 if none was given. Set by main()
// before any rendering, and so before terminalWidth() is first called and caches its result.
extern int gWidthOverride;

// Force inline image output even when stdout is not a terminal (--img). Lets mdcat be used as an
// image source when piped, e.g. `mdcat --img doc.md | gmore` for visual pager testing. Set by
// main() before rendering, so before terminalSupportsGraphics() caches its result. Cell-size
// detection still works over /dev/tty, and the terminal width is read from $COLUMNS / stderr's
// TIOCGWINSZ.
extern bool gForceGraphics;

// Backend explicitly chosen on the command line via `--img <kitty|sixel|none>`. When set, this is
// authoritative (same force as MDCAT_GRAPHICS), so `mdcat --img sixel doc.md | gmore` pins the
// protocol regardless of terminal or env. Unset (-1) means a bare `--img` with no protocol
// argument: the historical "force output, default to Kitty when piped" behaviour. Set by main().
extern int gForcedBackend;  // -1 = unset; else a GraphicsBackend value

// How a ```mermaid block is rendered (--mermaid, MDCAT_MERMAID): Auto tries the built-in renderer,
// then mmdc, then the source; Builtin skips mmdc; Mmdc skips the built-in renderer; Source always
// shows the source. Set by main() before rendering.
enum class MermaidMode { Auto, Builtin, Mmdc, Source };
extern MermaidMode gMermaidMode;

// Directory of the file currently being rendered, used to resolve relative image paths.
// Empty string means the current working directory. Set by main() before each render() call.
extern std::string gFileDir;

// Name of the file being rendered ("<stdin>" when empty) and the 1-based source line of the
// top-level block being emitted, so a diagnostic can say where its diagram starts.
extern std::string gSourceName;
extern size_t gBlockLine;

extern std::string gOutputFile;

// Columns currently consumed by container-block decorations (the block-quote left rule, a list
// item's marker indent). Every nested container raises it by the width of its prefix while its
// content is being rendered, so the width that paragraphs reflow to and that tables and rules fill
// shrinks to match the space the prefix leaves. Restored on the way out. See emitBlockQuote /
// emitList / terminalWidth.
extern int gIndent;

// Current bullet-list nesting depth, used to pick the bullet glyph (● ○ ▪︎ by depth, like GitHub's
// disc/circle/square). Raised while a bullet list's items render and restored after. Ordered lists
// don't change it — their marker is the number, not a depth-varying glyph. See emitList.
extern int gListDepth;

// Nesting depth across *all* lists (bullet and ordered). Used so only the outermost list block gets
// the two-space left lead that sets a list off from surrounding text; nested lists already step in
// via their parent item's marker. Raised around any list's items and restored after. See emitList.
extern int gListNesting;

// Set just before rendering the content of a *tight* list item. A tight list draws no blank line
// between its items, and likewise its item bodies should not put a blank line between the item's
// lead paragraph and an immediately-following nested list (GFM renders a tight item's paragraph
// without <p> spacing). render() reads this once at entry to suppress its own top-level block
// separators, then clears it so blocks nested deeper (and recursive renders) separate normally. See
// emitList.
extern bool gTightItem;

// The full terminal width, before any container indentation is taken out. Determined once and
// cached (see the comment in terminalWidth). Callers want terminalWidth(), which subtracts the
// indent.
// Re-layout width override for the integrated pager. When gmore re-renders the held source
// on a window resize it sets this to the new terminal width; 0 (the default) means "detect
// normally". Consulted before the memoized detection so a resize can drive a fresh width
// without the once-per-process cache getting in the way. Not thread-safe by itself, but the
// render thread that reads it is (re)started by the resize path after the value is set, so
// the write happens-before the read.
extern int gRenderWidth;

// When set, render() emits a private provenance OSC (see emitProvenance) before each
// top-level block, tagging the following grid rows with their markdown source line so the
// integrated pager can restore the reading position after a resize re-layout. Enabled ONLY
// on the pager path; left off for --dump, --imginfo, and non-tty output so those stay
// byte-identical (the golden --dump tests and property-concat.sh depend on it).
extern bool gEmitProvenance;

}  // namespace mdcat
