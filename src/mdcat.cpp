// mdcat - render a GitHub Flavored Markdown subset to an ANSI terminal.
//
// This is a C++17 port of the earlier markfun.sh. Rather than rewriting text with regular
// expressions, it parses the input in two phases that mirror the GFM specification
// (https://github.github.com/gfm):
//
//   1. Block parsing splits the document into a sequence of blocks: ATX headings, thematic
//      breaks, fenced and indented code blocks, GFM tables, and paragraphs (the leaf blocks),
//      plus the container blocks — block quotes and lists — whose stripped contents are rendered
//      recursively (a quote gets a left rule; a list item gets a bullet/number marker and a
//      hanging indent). Blank lines separate blocks; the lines of a paragraph are gathered
//      together so they can be reflowed.
//   2. Inline parsing turns the raw text of headings, paragraphs and table cells into styled
//      runs: emphasis (*x* / _x_), strong emphasis (**x**), code spans (`x`) and links
//      ([text](url)). Code-fence contents are emitted verbatim, not inline-parsed.
//
// Styling is applied with ANSI escape sequences; link targets use OSC 8 hyperlinks. Widths used
// for table layout and paragraph wrapping are measured in display columns, ignoring escapes.
//
// Only the elements the original script dealt with are implemented, made more solid and more
// conformant. As an exception to the "no inline HTML" rule, a paragraph or table cell that
// consists solely of an <img ...> tag pointing at a local PNG is rendered as an actual image by
// shelling out to timg(1) on terminals that support sixel graphics; elsewhere (e.g. Apple's
// Terminal.app) it falls back to the tag's alt text or filename. See renderImageBlock. Supported
// formats: PNG, JPG, GIF, SVG. As a second exception, an inline <br> tag (in any of the <br>,
// <br/>, <br /> spellings) is rendered as a hard line break wherever it appears — see
// scanHardBreak. All other inline HTML is passed through literally. LaTeX math between $...$
// (inline) or $$...$$ (block) is transliterated to Unicode on a best-effort basis — Greek letters,
// common operator and relation symbols, super/subscripts, and \mathrm/\mathbf wrappers; see
// renderMath and scanMath. A ```mermaid fenced code block is rendered as a diagram via the mermaid
// CLI (mmdc) when it and a graphics terminal are available; otherwise it falls back to ordinary
// code. See renderMermaidBlock. On a Kitty-graphics terminal, a flowchart that mermaid.h
// understands is drawn by mdcat itself instead: strokes as one PNG, labels as real text, fitted to
// the width by tighter spacing and wrapped labels, or drawn wider than the terminal if need be.
// --mermaid / $MDCAT_MERMAID picks the chain; $MDCAT_MERMAID_DEBUG=1 reports builtin rejections.
//
// Build:  c++ -std=c++17 -O2 -o mdcat mdcat.cpp
// Usage:  mdcat [--width N] [--img[=kitty|sixel|none]] [--mermaid MODE] [--] [file ...]
//         (reads standard input when given no file arguments)
//         --mermaid auto|builtin|mmdc|source picks how ```mermaid blocks render.
//         --width N forces the render width, overriding $COLUMNS and the terminal size.
//         --img forces image output even when piped; an optional protocol pins the graphics
//         backend.

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gmore_run.h"
#include "highlight.h"
#include "mdcat_blocks.h"
#include "mdcat_image.h"
#include "mdcat_inline.h"
#include "mdcat_math.h"
#include "mdcat_mermaid.h"
#include "mdcat_pool.h"
#include "mdcat_protocol.h"
#include "mdcat_remote.h"
#include "mdcat_render.h"
#include "mdcat_slot_sink.h"
#include "mdcat_state.h"
#include "mdcat_style.h"
#include "mdcat_table.h"
#include "mdcat_terminal.h"
#include "mdcat_text.h"
#include "mermaid.h"
#include "tty_guard.h"
#include "unicode_width.h"
#include "width_probe.h"

namespace mdcat {

void usage() {
    std::cerr << "usage: mdcat [-o OUTFILE] [--width N] [--img[=kitty|sixel|none]]\n"
                 "             [--mermaid auto|builtin|mmdc|source] [--] [file ...]\n";
}

[[noreturn]] void error(int code, const std::string& msg) {
    std::cerr << "mdcat: " << msg << "\n";
    usage();
    exit(code);
}

// Map a --mermaid / MDCAT_MERMAID mode name; false for an unrecognized value.
MermaidMode parseMermaidMode(const std::string& s) {
    if (s == "auto") return MermaidMode::Auto;
    if (s == "builtin") return MermaidMode::Builtin;
    if (s == "mmdc") return MermaidMode::Mmdc;
    if (s == "source") return MermaidMode::Source;
    error(2, std::string("invalid mermaid mode: ") + s);
}


int parseOptions(int argc, char** argv) {
    // Supported: --width N / -w N / --width=N to force the render width (overriding $COLUMNS
    // and the terminal size), and -- to end options so a filename may begin with a dash. Option
    // parsing stops at the first non-option operand.

    // Map a --img protocol argument to a backend, or return -2 for an unrecognized value.
    auto parseBackend = [](const std::string& s) -> int {
        if (s == "kitty") return static_cast<int>(GraphicsBackend::Kitty);
        if (s == "sixel") return static_cast<int>(GraphicsBackend::Sixel);
        if (s == "none") return static_cast<int>(GraphicsBackend::None);
        return -2;
    };

    int a = 1;
    for (; a < argc; ++a) {
        std::string arg = argv[a];
        if (arg == "--") {
            ++a;
            break;
        }
        if (arg.size() < 2 || arg[0] != '-') break;  // first operand: stop option parsing

        if (arg == "-o") {
            if (a + 1 >= argc) error(2, "-o requires a filename");
            gOutputFile = argv[++a];
            continue;
        }

        if (arg == "--img") {  // force image output even when piped
            gForceGraphics = true;
            // Optional protocol argument: only consume the next token if it names a backend,
            // so `--img file.md` still treats file.md as the operand.
            if (a + 1 < argc) {
                int b = parseBackend(argv[a + 1]);
                if (b >= 0) {
                    gForcedBackend = b;
                    ++a;
                }
            }
            continue;
        }
        if (arg.rfind("--img=", 0) == 0) {  // inline protocol: --img=kitty|sixel|none
            int b = parseBackend(arg.substr(6));
            if (b < 0) error(2, "invalid --img protocol: " + arg.substr(6));
            gForceGraphics = true;
            gForcedBackend = b;
            continue;
        }

        if (arg == "--mermaid" || arg.rfind("--mermaid=", 0) == 0) {  // flag wins over the env
            std::string mode;
            if (arg == "--mermaid") {
                if (a + 1 >= argc) error(2, "--mermaid requires a mode");
                mode = argv[++a];
            } else {
                mode = arg.substr(10);
            }
            gMermaidMode = parseMermaidMode(mode);

            continue;
        }

        std::string val;
        if (arg == "-w" || arg == "--width") {  // value is the next argument
            if (a + 1 >= argc) error(2, arg + " requires a value");
            val = argv[++a];
        } else if (arg.rfind("--width=", 0) == 0) {  // value is inline after '='
            val = arg.substr(8);
        } else
            error(2, "unknown option: " + arg);
        if (!val.empty()) {
            int w = std::atoi(val.c_str());
            if (w <= 0) error(2, "invalid width: " + val);
            gWidthOverride = w;
        }
    }
    return a;
}

int renderPaged(std::vector<std::string> files) {
    // On a TTY, page through gmore by default. A render THREAD (not a forked child, so the
    // source and the renderer stay resident) streams bytes into a pipe while gmore parses the
    // read end and paints the first page before the whole document is ready. gmore holds the
    // source via the relayout callback: on a window resize it re-renders at the new width and
    // restores the reading position by source-line provenance (see gEmitProvenance / gmore).
    // Read the source ONCE into re-renderable segments (one per file, so `mdcat a b` keeps
    // rendering each independently — never merging a's last block with b's first). Held for
    // the life of the pager so every re-layout renders from the same source.
    struct Segment {
        std::string fileDir;
        std::vector<std::string> lines;
        std::string name;
    };
    std::vector<Segment> segments;
    if (!files.empty()) {
        for (const auto& path : files) {
            std::ifstream f(path);
            if (!f) {
                std::cerr << "mdcat: " << path << ": cannot open file\n";
                return 1;
            }
            size_t slash = path.rfind('/');
            segments.push_back({slash != std::string::npos ? path.substr(0, slash) : std::string(),
                                splitLines(f),
                                path});
        }
    } else {
        segments.push_back({std::string(), splitLines(std::cin), std::string()});
    }

    // Page one segment (file) at a time: each gets its own render thread/pipe/gmore::run(),
    // so advancing past the end of a file only ever needed that file's bytes, never the rest
    // of the argument list. Like less(1) with multiple operands, the prompt shows "(file x of
    // y)" and 'q' quits the whole session while reaching a file's end just advances to the
    // next one.
    for (size_t idx = 0; idx < segments.size(); ++idx) {
        Segment& seg = segments[idx];
        std::string promptSuffix;
        if (segments.size() > 1) {
            promptSuffix = " (file " + std::to_string(idx + 1) + " of " +
                std::to_string(segments.size()) + ")";
        }

        // Render `seg` at `width` into a fresh pipe on a background thread; return the read
        // end for gmore to ingest. Provenance markers are emitted (paging path) so the grid
        // rows carry their source line. Only one render thread runs at a time — the caller
        // joins the previous one before starting the next (see joinRender), so gRenderWidth
        // and the other render globals are never touched concurrently.
        std::thread renderThread;
        auto joinRender = [&] {
            if (renderThread.joinable()) renderThread.join();
        };
        auto startRender = [&](int width) -> int {
            joinRender();
            int pfd[2];
            if (pipe(pfd) != 0) {
                std::perror("mdcat: pipe");
                return -1;
            }
            // SIGWINCH must reach gmore's main thread (it drives re-layout), never the render
            // thread. A newly created thread inherits the creator's signal mask, so block
            // SIGWINCH on THIS (main) thread across the spawn — the render thread inherits
            // the block — then restore the main thread's mask below. Masking from inside the
            // render thread instead is both racy (it runs asynchronously) and, on this
            // platform, was observed to leave SIGWINCH blocked on the main thread, silently
            // killing resize handling. Blocking here and unblocking here keeps the mask
            // change on one thread and deterministic.
            sigset_t block, prev;
            sigemptyset(&block);
            sigaddset(&block, SIGWINCH);
            pthread_sigmask(SIG_BLOCK, &block, &prev);
            renderThread = std::thread([&seg, width, wfd = pfd[1]] {
                gRenderWidth = width;    // re-layout width for this pass (0 elsewhere)
                gEmitProvenance = true;  // tag rows with their source line for resize anchoring
                SlotSink sink(wfd);
                std::ostream out(&sink);
                gFileDir = seg.fileDir;
                gSourceName = seg.name;
                render(seg.lines, out);
                out.flush();
                gFileDir.clear();
                gSourceName.clear();
                sink.finish();
                close(wfd);
            });
            pthread_sigmask(SIG_SETMASK, &prev, nullptr);  // main handles SIGWINCH again
            return pfd[0];
        };

        int rfd = startRender(fullTerminalWidth());
        if (rfd < 0) return 1;
        gmore::Relayout relayout = [&](int width) { return startRender(width); };
        bool quitRequested = false;
        int pagerRc = gmore::run(
            std::string(), false, false, false, false, rfd, relayout, promptSuffix, &quitRequested);
        close(rfd);
        joinRender();
        if (pagerRc != 0 || quitRequested) return pagerRc;
    }
    return 0;
}

int renderAll(std::vector<std::string> files, std::ostream& out) {
    if (files.empty()) return render(splitLines(std::cin), out), 0;
    for (const auto& path : files) {
        std::ifstream f(path);
        if (!f) error(1, path + ": cannot open file");
        size_t slash = path.rfind('/');
        gFileDir = (slash != std::string::npos) ? path.substr(0, slash) : std::string();
        gSourceName = path;
        render(splitLines(f), out);
    }
    gFileDir.clear();
    gSourceName.clear();
    return 0;
}

}  // namespace mdcat

using namespace mdcat;

int main(int argc, char** argv) {
    if (auto e = std::getenv("MDCAT_MERMAID")) gMermaidMode = parseMermaidMode(e);

    // Collect the file operands, parsing options first.
    auto optind = parseOptions(argc, argv);

    std::vector<std::string> files;
    for (auto a = optind; a < argc; ++a) files.emplace_back(argv[a]);

    // Eagerly initialize terminal-query state before rendering starts. This avoids deferred
    // probe traffic while the pager is active and keeps worker threads off terminal I/O.
    (void)graphicsBackend();
    (void)cellMetrics();
    (void)queryBackgroundColor();
    initTheme();

    bool usePager = isatty(STDOUT_FILENO) && gOutputFile.empty();
    // Measure text the way this terminal does (grapheme clusters or code point by code point),
    // so a line laid out to fit never wraps there. The built-in pager shares the choice.
    uw::initMode(usePager);

    if (usePager) return renderPaged(files);

    // Non-tty: stream blocks out as they are rendered. Each file is rendered independently and
    // the results concatenated — the last block of one file can never merge with the first of
    // the next — which makes rendering distribute over the argument list (`mdcat a b` == `mdcat
    // a` then `mdcat b`), as checked by tests/property-concat.sh. SlotSink reserves an ordered
    // slot per block and drains them on a dedicated writer thread, so a deferred image
    // (standalone <img>, mermaid) returns immediately while its conversion runs on the pool and
    // a stalled write() blocks only the writer (ADR 0003). Output stays in document order and
    // byte-identical to the old serial StreamingSink; only timing changes.
    int fd = gOutputFile.empty() ? STDOUT_FILENO
                                 : open(gOutputFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) error(errno, "cannot open output file " + gOutputFile);
    SlotSink sink(fd);
    std::ostream out(&sink);
    if (renderAll(std::move(files), out) != 0) return 1;
    out.flush();    // commit the final block's text as a slot
    sink.finish();  // flush trailing text, drain all slots, join the writer thread
    return 0;
}
