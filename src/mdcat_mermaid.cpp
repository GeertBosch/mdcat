// Mermaid diagram blocks: the mmdc/timg image path and the built-in renderer (mermaid.h) framing.
#include "mdcat_mermaid.h"

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <ostream>
#include <string>
#include <vector>

#include "mdcat_image.h"
#include "mdcat_inline.h"
#include "mdcat_protocol.h"
#include "mdcat_slot_sink.h"
#include "mdcat_state.h"
#include "mdcat_style.h"
#include "mdcat_terminal.h"
#include "mdcat_text.h"
#include "mermaid.h"
#include "unicode_width.h"

namespace mdcat {

// Render the source of a ```mermaid fenced code block as a diagram, writing the raw sixel bytes to
// `out` and the painted cell footprint to `cellWidth`/`cellHeight`. Returns true on success; on any
// failure (mmdc missing or erroring, timg failing) returns false so the caller can fall back to
// showing the block as ordinary fenced code.
//
// mmdc(1) (the mermaid CLI) renders the diagram to a temporary PNG, which is then handed to the
// existing <img> pipeline (renderImageBlock) as an absolute path — reusing its geometry/footprint
// and sixel-capture logic exactly. We go through a temp file rather than mmdc's `-o -` stdout mode
// so that renderImageBlock can read the PNG's intrinsic size and width-bound it to the terminal.
// mmdc's -w (page width) is set to the available width in device pixels. Since -w is only a maximum
// (a diagram narrower than the page renders at its natural, often tiny, size on a HiDPI terminal),
// we render once to measure the natural width, then re-render at a -s (scale) chosen to enlarge a
// small diagram toward the column budget — this is what actually fixes "too small" on iTerm2.
bool renderMermaidBlock(const std::vector<std::string>& lines,
                        int availWidth,
                        std::string& out,
                        int& cellWidth,
                        int& cellHeight,
                        uint32_t kittyId) {
    if (!mmdcAvailable()) return false;

    // Unique temp paths for the mermaid source and the rendered PNG. mkstemp creates the files; we
    // only need their names (mmdc writes the PNG itself), so the descriptors are closed
    // immediately.
    char srcPath[] = "/tmp/mdcat-mermaid-XXXXXX";
    char pngPath[] = "/tmp/mdcat-mermaid-XXXXXX.png";
    int sfd = mkstemp(srcPath);
    if (sfd < 0) return false;
    int pfd = mkstemps(pngPath, 4);  // keep the ".png" suffix so it is a valid image extension
    if (pfd < 0) {
        close(sfd);
        unlink(srcPath);
        return false;
    }
    close(pfd);

    // Write the diagram source to the temp file.
    {
        std::string body;
        for (const auto& l : lines) {
            body += l;
            body += '\n';
        }
        ssize_t total = 0, len = static_cast<ssize_t>(body.size());
        while (total < len) {
            ssize_t w = write(sfd, body.data() + total, static_cast<size_t>(len - total));
            if (w <= 0) break;
            total += w;
        }
        close(sfd);
    }

    auto cleanup = [&] {
        unlink(srcPath);
        unlink(pngPath);
    };

    auto shquote = [](const char* p) {
        std::string q = "'";
        for (const char* c = p; *c; ++c) {
            if (*c == '\'')
                q += "'\\''";
            else
                q += *c;
        }
        q += "'";
        return q;
    };

    // If PUPPETEER_EXECUTABLE_PATH is not already set, probe standard macOS Chrome locations so
    // mmdc works even when puppeteer's own chrome-headless-shell binary was not downloaded.
    std::string chromeEnv;
    if (!std::getenv("PUPPETEER_EXECUTABLE_PATH")) {
        static const char* kChromePaths[] = {
            "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
            "/Applications/Chromium.app/Contents/MacOS/Chromium",
        };
        for (const char* p : kChromePaths) {
            if (access(p, X_OK) == 0) {
                chromeEnv = "PUPPETEER_EXECUTABLE_PATH=" + shquote(p) + " ";
                break;
            }
        }
    }

    auto runMmdc = [&](const std::string& opts) {
        // -q suppresses mmdc's progress chatter so it never pollutes the rendered output; stderr is
        // discarded as a further guard.
        std::string cmd = chromeEnv + "mmdc -q" + opts + " -i " + shquote(srcPath) + " -e png -o " +
            shquote(pngPath) + " </dev/null >/dev/null 2>&1";
        return std::system(cmd.c_str()) == 0;
    };

    // Available width in device pixels — the column budget times the real cell width. Used both as
    // mmdc's page width (-w) and as the target the diagram is scaled up toward.
    int availPx = availWidth > 0 ? availWidth * cellMetrics().realCellW() : 0;
    std::string widthOpt = availPx > 0 ? " -w " + std::to_string(availPx) : std::string();

    // Pass the terminal background to mmdc so the diagram background matches the terminal theme.
    // Omitted when the terminal didn't answer OSC 11 (mmdc keeps its default white background).
    std::string bg = queryBackgroundColor();
    std::string bgOpt = bg.empty() ? std::string() : " -b '" + bg + "'";
    if (darkBackground()) bgOpt += " --theme dark";

    // First render at scale 1 to learn the diagram's natural pixel width. mmdc's -w is only a
    // maximum page width: a diagram whose layout is narrower than -w renders at its natural size,
    // which on a HiDPI terminal is a tiny sixel. So if the natural width is well under the
    // available width, pick a -s (Puppeteer scale) that enlarges it to roughly fill the column
    // budget and re-render. Scale is capped at 2 so a small diagram is not blown up to fill most of
    // the screen.
    if (!runMmdc(widthOpt + bgOpt)) {
        cleanup();
        return false;
    }
    int naturalW = pngWidth(pngPath);
    if (availPx > 0 && naturalW > 0) {
        int scale = availPx / naturalW;
        if (scale > 2) scale = 2;
        if (scale >= 2) {
            if (!runMmdc(widthOpt + bgOpt + " -s " + std::to_string(scale))) {
                cleanup();
                return false;
            }
        }
    }

    // Hand the rendered PNG to the <img> pipeline as an absolute path (so gFileDir resolution is a
    // no-op), reusing all of its geometry and sixel-capture logic.
    std::string tag = "<img src=\"" + std::string(pngPath) + "\">";
    // pngPath is absolute, so fileDir resolution is a no-op; pass empty. Forward kittyId so a
    // deferred mermaid slot gets a document-order Kitty id (ADR 0003).
    bool ok = renderImageBlock(tag,
                               availWidth,
                               /*fileDir=*/std::string(),
                               out,
                               cellWidth,
                               cellHeight,
                               /*forceWidthBound=*/false,
                               kittyId);
    cleanup();
    return ok && !out.empty() && cellHeight > 0;
}

namespace {

// Frame a diagram: the image first, anchored at the band's top-left with no cursor-up, then its
// text rows (docs/plan-mermaid-flowcharts.md, "Emitting the block"). The image takes no cursor
// movement, so the rows that follow scroll normally and carry it along, however tall the band is.
// Each row is indented by `pad` spaces. Within a row only the non-space runs are written; every
// other cell is skipped with a cursor move, because on VSCode spaces written over an image make it
// disappear (docs/TERMINAL-GRAPHICS.md section 10). The cursor ends at column 1 below the band.
std::string overlayParagraphBytes(const std::vector<std::string>& textRows,
                                  const std::string& apc,
                                  int pad) {
    std::string s;
    if (pad > 0) s += "\033[" + std::to_string(pad) + 'C';
    s += "\0337";
    s += apc;
    s += "\0338\r";
    for (const std::string& row : textRows) {
        size_t i = 0;
        bool wrote = false;
        while (i < row.size()) {
            if (row[i] == ' ') {
                size_t j = i;
                while (j < row.size() && row[j] == ' ') ++j;
                if (j < row.size()) {  // trailing spaces are never written
                    if (!wrote) {
                        s.append(static_cast<size_t>(pad), ' ');
                        wrote = true;
                    }
                    s += "\033[" + std::to_string(j - i) + 'C';
                }
                i = j;
                continue;
            }
            size_t j = i;
            while (j < row.size() && row[j] != ' ') ++j;
            if (!wrote) {
                s.append(static_cast<size_t>(pad), ' ');
                wrote = true;
            }
            s.append(row, i, j - i);
            i = j;
        }
        s += '\n';
    }
    return s;
}

// The labels of a diagram share their rows with each other and with the strokes' text layer, so
// each line of a Markdown label must stand alone: a bold or italic style that is on at the end of
// a line is closed there and reopened at the start of the next. (reflow already does this for code
// spans.)
std::vector<std::string> mermaidMarkdownLines(const std::string& md, int width) {
    std::vector<std::string> lines = splitOnNewlines(reflow(renderInline(md), width));
    bool bold = false, italic = false;
    for (std::string& line : lines) {
        std::string out;
        if (bold) out += kBoldOn;
        if (italic) out += kItalicOn;
        for (size_t i = 0; i < line.size();) {
            size_t len = unitLength(line, i);
            if (line.compare(i, len, kBoldOn) == 0)
                bold = true;
            else if (line.compare(i, len, kBoldOff) == 0)
                bold = false;
            else if (line.compare(i, len, kItalicOn) == 0)
                italic = true;
            else if (line.compare(i, len, kItalicOff) == 0)
                italic = false;
            out.append(line, i, len);
            i += len;
        }
        if (italic) out += kItalicOff;
        if (bold) out += kBoldOff;
        line = out;
    }
    return lines;
}

}  // namespace

// Draw a ```mermaid block with the built-in renderer (mermaid.h) and write it centred. Returns
// false, writing nothing and setting `reason`, when the builtin does not apply: not a Kitty
// terminal or images are off, the block is inside a list or quote (whose per-line prefixing would
// break the framing), the diagram is outside the supported subset, or it is too tall for the
// terminal. `tooComplex` is set when the diagram has more back edges or crossings than its width
// allows, which is better shown as source than drawn by anything else.
bool emitBuiltinMermaid(const std::vector<std::string>& body,
                        std::ostream& out,
                        std::string& reason,
                        bool& tooComplex) {
    tooComplex = false;
    if (!terminalSupportsGraphics() || graphicsBackend() != GraphicsBackend::Kitty) {
        reason = "not kitty";
        return false;
    }
    if (gIndent != 0) {
        reason = "inside list or quote";
        return false;
    }
    CellMetrics cm = cellMetrics();
    mermaid::Env env;
    env.availCols = terminalWidth();
    env.cellW = cm.realCellW();
    env.cellH = cm.realCellH();
    env.darkTheme = darkBackground();
    env.width = displayWidth;
    env.markdownLines = mermaidMarkdownLines;
    mermaid::Diagram d;
    mermaid::Decline why;
    if (!mermaid::render(body, env, d, reason, &why)) {
        tooComplex = why == mermaid::Decline::TooComplex;
        return false;
    }
    // VSCode misplaces the text of a band taller than the screen (field guide section 10, case K).
    if (const char* tp = std::getenv("TERM_PROGRAM"); tp && std::string(tp) == "vscode") {
        for (int fd : {STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO}) {
            struct winsize ws;
            if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
                if (d.rows > ws.ws_row) {
                    reason = "taller than the terminal: " + std::to_string(d.rows) + " > " +
                        std::to_string(ws.ws_row);
                    return false;
                }
                break;
            }
        }
    }
    // Under a slot sink the writer assigns document-order ids (kittyRenumberAll); elsewhere mint
    // one.
    uint32_t id = asSlotSink(out) ? 1 : nextKittyId();
    int pad = std::max(0, (terminalWidth() - d.cols) / 2);
    out << overlayParagraphBytes(
        d.textRows, kittyApcFromPng(d.png, d.cols, d.rows, id, imageXOffset(env.cellW)), pad);
    return true;
}

}  // namespace mdcat
