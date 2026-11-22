#include "mdcat_image.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>

#include "mdcat_inline.h"
#include "mdcat_pool.h"
#include "mdcat_protocol.h"
#include "mdcat_remote.h"
#include "mdcat_terminal.h"
#include "mdcat_text.h"

namespace mdcat {

// ---------------------------------------------------------------------------
// Image rendering: <img ...> -> timg
// ---------------------------------------------------------------------------
//
// As an exception to the "inline HTML is literal" rule, a paragraph or table cell whose entire
// content is a single <img ...> tag referring to a local supported image file is rendered as a
// real image. We let timg(1) paint it with sixel graphics. For PNG we read the intrinsic pixel
// size from its fixed header; for other formats (JPG, GIF, SVG) we verify the file exists and
// rely on the width/height attributes (or let timg fill the available width). If anything goes
// wrong (unsupported format, file unreadable, timg missing or failing) we fall back to the tag's
// alt text, or the literal tag.

namespace {

// Supported image extensions (lowercase). timg handles all of these.
bool isSupportedImageExt(const std::string& path) {
    static const char* exts[] = {".png", ".jpg", ".jpeg", ".gif", ".svg", nullptr};
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (int i = 0; exts[i]; ++i)
        if (ext == exts[i]) return true;
    return false;
}

// Verify that path is a supported image format and the file exists.
bool readImageSize(const std::string& path) {
    if (!isSupportedImageExt(path)) return false;
    std::ifstream f(path, std::ios::binary);
    return f.good();
}
}  // namespace

// Pixel width of a PNG file, read from its IHDR chunk (a big-endian uint32 at byte offset 16, right
// after the 8-byte signature, the 4-byte chunk length and the 4-byte "IHDR" type). Returns 0 if the
// file is not a readable PNG. Used to learn a rendered diagram's natural size for scaling.
int pngWidth(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return 0;
    unsigned char h[24];
    f.read(reinterpret_cast<char*>(h), sizeof h);
    if (f.gcount() < static_cast<std::streamsize>(sizeof h)) return 0;
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8; ++i)
        if (h[i] != sig[i]) return 0;
    return (h[16] << 24) | (h[17] << 16) | (h[18] << 8) | h[19];
}


// Scan a markdown image ![alt](src) starting at s[i]. On success, sets alt/src and advances i
// past the closing ')'; returns true. i must point at '!'.
bool scanMdImageAt(const std::string& s, size_t& i, std::string& alt, std::string& src) {
    size_t n = s.size();
    if (i + 4 >= n || s[i] != '!' || s[i + 1] != '[') return false;
    size_t close = i + 2;
    int depth = 1;
    while (close < n) {
        if (s[close] == '\\' && close + 1 < n) {
            close += 2;
            continue;
        }
        if (s[close] == '[')
            ++depth;
        else if (s[close] == ']') {
            if (--depth == 0) break;
        }
        ++close;
    }
    if (close >= n || close + 1 >= n || s[close + 1] != '(') return false;
    alt = s.substr(i + 2, close - (i + 2));
    size_t up = close + 2;
    std::string url;
    int pd = 1;
    while (up < n) {
        char c = s[up];
        if (c == '\\' && up + 1 < n) {
            url += s[up + 1];
            up += 2;
            continue;
        }
        if (c == '(')
            ++pd;
        else if (c == ')') {
            if (--pd == 0) break;
        }
        url += c;
        ++up;
    }
    if (up >= n || s[up] != ')') return false;
    src = trim(url);
    i = up + 1;
    return true;
}

// Parse markdown image syntax into an attrs map with "alt", "src" (and optionally "href") keys.
// Handles plain images ![alt](src) and linked images [![alt](src)](href).
// Returns false if `text` is not exactly one such pattern with nothing before or after it.
bool parseMdImage(const std::string& text, std::map<std::string, std::string>& attrs) {
    const std::string& s = text;
    size_t n = s.size();

    // [![alt](src)](href) — linked image
    if (n > 5 && s[0] == '[' && s[1] == '!') {
        size_t i = 1;  // points at '!'
        std::string alt, src;
        if (scanMdImageAt(s, i, alt, src) && i < n && s[i] == ']' && i + 1 < n && s[i + 1] == '(') {
            ++i;  // skip ']'
            size_t up = i + 1;
            std::string href;
            int pd = 1;
            while (up < n) {
                char c = s[up];
                if (c == '\\' && up + 1 < n) {
                    href += s[up + 1];
                    up += 2;
                    continue;
                }
                if (c == '(')
                    ++pd;
                else if (c == ')') {
                    if (--pd == 0) break;
                }
                href += c;
                ++up;
            }
            if (up < n && s[up] == ')' && up + 1 == n) {
                attrs["alt"] = alt;
                attrs["src"] = src;
                attrs["href"] = trim(href);
                return true;
            }
        }
    }

    // ![alt](src) — plain image
    if (n > 4 && s[0] == '!') {
        size_t i = 0;
        std::string alt, src;
        if (scanMdImageAt(s, i, alt, src) && i == n) {
            attrs["alt"] = alt;
            attrs["src"] = src;
            return true;
        }
    }

    return false;
}

// Parse a single HTML start tag of the form <img key="value" key2="value2" ...> into a map of
// attribute names (lowercased) to values. Only the simple quoted-value form described by the task
// is recognized. Returns false if `text` (already trimmed) is not exactly one <img ...> tag with
// nothing before or after it. Values may be single- or double-quoted.
bool parseImgTag(const std::string& text, std::map<std::string, std::string>& attrs) {
    const std::string& s = text;
    size_t i = 0, n = s.size();
    if (n < 5 || s[0] != '<') return false;
    // Tag name must be "img" (case-insensitive), followed by whitespace or '>'.
    if (!(std::tolower(s[1]) == 'i' && std::tolower(s[2]) == 'm' && std::tolower(s[3]) == 'g'))
        return false;
    if (!(isSpaceCh(s[4]) || s[4] == '>' || s[4] == '/')) return false;
    i = 4;
    while (i < n) {
        while (i < n && isSpaceCh(s[i])) ++i;
        if (i < n && s[i] == '/') ++i;  // tolerate a self-closing slash
        while (i < n && isSpaceCh(s[i])) ++i;
        if (i >= n) return false;            // no closing '>'
        if (s[i] == '>') return i == n - 1;  // the tag must end the string
        // attribute name
        size_t ks = i;
        while (i < n &&
               (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '_'))
            ++i;
        if (i == ks) return false;  // expected a name
        std::string key = s.substr(ks, i - ks);
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        while (i < n && isSpaceCh(s[i])) ++i;
        std::string value;
        if (i < n && s[i] == '=') {
            ++i;
            while (i < n && isSpaceCh(s[i])) ++i;
            if (i < n && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i++];
                size_t vs = i;
                while (i < n && s[i] != q) ++i;
                if (i >= n) return false;  // unterminated quote
                value = s.substr(vs, i - vs);
                ++i;  // consume closing quote
            } else {  // unquoted value up to whitespace or '>'
                size_t vs = i;
                while (i < n && !isSpaceCh(s[i]) && s[i] != '>') ++i;
                value = s.substr(vs, i - vs);
            }
        }
        attrs[key] = value;
    }
    return false;  // ran off the end without a closing '>'
}

namespace {

// Complete a half-open timg geometry ("Wx" or "xH") into a full "WxH" box by filling the missing
// dimension with a deliberately loose bound that can never become the binding constraint, so the
// requested dimension governs and the image keeps its own aspect ratio. We do this ourselves rather
// than letting timg fill the blank from the terminal: timg >= 1.6.3 reads the terminal size via the
// controlling tty's window-size ioctl, and when that is unavailable (stdout on a pipe with no
// usable controlling tty) it refuses a partial -g with "Failed to read size from terminal" and
// emits no output. Supplying a full box keeps mdcat working regardless of timg version or how it is
// invoked. The loose bound is the terminal extent in that axis with a generous floor; mdcat reads
// the actual painted size back from the sixel afterward, so over-specifying costs nothing.
std::string completeGeom(const std::string& geom) {
    // A loose bound large enough never to clip: the terminal extent floored at 1000 cells so a
    // small or undetectable terminal can't accidentally bind the result.
    int loose = std::max(1000, fullTerminalWidth());
    size_t x = geom.find('x');
    // No box at all: timg still needs a size, and on a pipe with no usable controlling tty it
    // cannot get one from the terminal. Bound the width to the available columns with a loose
    // height; timg fits the image inside, so a smaller image keeps its intrinsic size and a larger
    // one is scaled down to the column budget — the same effect timg's own terminal default would
    // give.
    if (geom.empty() || x == std::string::npos)
        return std::to_string(fullTerminalWidth()) + "x" + std::to_string(loose);
    bool haveW = x > 0;
    bool haveH = x + 1 < geom.size();
    if (haveW && haveH) return geom;                             // already a full "WxH": leave it
    if (haveW) return geom + std::to_string(loose);              // "Wx" -> "WxLOOSE"
    if (haveH) return std::to_string(loose) + geom;              // "xH" -> "LOOSExH"
    return std::to_string(loose) + "x" + std::to_string(loose);  // bare "x": both loose
}

// Run timg to render `path` at the given -g geometry with sixel graphics, returning its stdout.
// `geom` is a timg geometry string in character cells: "WxH" (a full box), "Wx" (width only, height
// derived from the aspect ratio) or "xH" (height only, width derived). Partial geometry lets a
// single requested dimension govern, with the other following the true aspect ratio, instead of our
// own rounding double-constraining the box; completeGeom turns it into a full box before invoking
// timg so a piped timg never has to query the terminal. Returns an empty string if timg cannot be
// launched or exits non-zero.
//
// The image data is captured (rather than letting timg draw straight to the terminal) for two
// reasons: with its stdout on a pipe timg emits plain sixel without doing its own cursor/scroll
// positioning, so we can later replay the bytes at whatever cursor position we choose; and we can
// read the painted pixel size out of the sixel to compute an exact cell footprint.
std::string runTimg(const std::string& path, const std::string& geomIn) {
    std::string geom = completeGeom(geomIn);
    // The -g box arrives in REAL terminal cells, but timg (run headless, stdin detached) sizes it
    // with its own fixed 9x18 px cell, painting pixels that occupy more of the terminal's narrower
    // cells than requested. Rescale each axis from real cells to timg cells so the painted pixels
    // match the columns mdcat reserves. completeGeom always returns a full "WxH", so both fields
    // are present.
    if (size_t x = geom.find('x'); x != std::string::npos) {
        CellMetrics cm = cellMetrics();
        int w = std::atoi(geom.substr(0, x).c_str());
        int h = std::atoi(geom.substr(x + 1).c_str());
        if (w > 0 && h > 0)
            geom =
                std::to_string(cm.colsToTimgCols(w)) + "x" + std::to_string(cm.rowsToTimgRows(h));
    }
    std::ostringstream cmd;
    // -ps : sixel pixelation;  -g <geom> : fit inside the given character-cell box.  The path is
    // passed single-quoted with embedded single quotes escaped, so odd filenames stay safe.
    std::string quoted = "'";
    for (char c : path) {
        if (c == '\'')
            quoted += "'\\''";
        else
            quoted += c;
    }
    quoted += "'";
    // stdin from /dev/null: timg only writes its sixel/Kitty bytes to stdout (the pipe). With stdin
    // left on the controlling tty, timg puts the terminal into raw mode (no ECHO/ICANON) to query
    // it, then restores it on exit — but when several timg processes run concurrently (ADR 0003),
    // their save/restore races and the last one restores a stale "echo off" state, leaving the
    // user's terminal dead. Detaching stdin stops timg touching terminal modes at all; we already
    // pass an explicit -g box and protocol, so it has nothing to auto-detect. -b <bg>: fill the
    // height padding (timg rounds up to a 6px sixel band) and any alpha with the terminal's
    // background colour instead of timg's default black, which would otherwise paint a dark line
    // under a light image. Omitted when the terminal didn't answer OSC 11 (timg keeps its default).
    std::string bg = queryBackgroundColor();
    std::string bgOpt = bg.empty() ? std::string() : " -b '" + bg + "'";
    if (!geom.empty())
        cmd << "timg -ps" << bgOpt << " -g" << geom << " " << quoted << " </dev/null 2>/dev/null";
    else
        cmd << "timg -ps" << bgOpt << " " << quoted << " </dev/null 2>/dev/null";
    FILE* p = popen(cmd.str().c_str(), "r");
    if (!p) return std::string();
    std::string out;
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, got);
    int rc = pclose(p);
    if (rc != 0) return std::string();
    return out;
}

// Run timg to render `path` into the given -g cell box with the KITTY graphics protocol (-pk),
// returning its stdout: a Kitty APC (ESC _ G a=T,...,f=100,m=...; <base64 PNG> ESC \), chunked
// for large images. Unlike the sixel path, timg here DOWNSCALES the image to the -g box with proper
// interpolation (so the terminal does no quality-losing scaling — VSCode's terminal in particular
// has no anti-aliasing), with correct m= chunking. The exact header keys vary by timg version:
// newer timg (1.6.3) sets i= and q=2, but older timg (Debian 1.4.3) emits neither, so the caller
// must INSERT a unique i= and a q=2 rather than assume they are present (see kittyRewriteId /
// kittyQ2). timg requires a -g box when headless (piped), so completeGeom fills any missing
// dimension. Returns empty on failure.
std::string runTimgKitty(const std::string& path, const std::string& geomIn) {
    std::string geom = completeGeom(geomIn);
    if (geom.empty()) geom = "100x100";  // timg -pk needs a box headless; a loose default
    std::string quoted = "'";
    for (char c : path) {
        if (c == '\'')
            quoted += "'\\''";
        else
            quoted += c;
    }
    quoted += "'";
    std::ostringstream cmd;
    // stdin from /dev/null so concurrent timg processes never race on terminal raw-mode
    // save/restore and leave the tty with echo disabled — see runTimg's note.
    cmd << "timg -pk -g" << geom << " " << quoted << " </dev/null 2>/dev/null";
    FILE* p = popen(cmd.str().c_str(), "r");
    if (!p) return std::string();
    std::string out;
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, got);
    int rc = pclose(p);
    if (rc != 0) return std::string();
    // timg wraps its Kitty output with its own cursor management — a leading "ESC[?25l" (hide
    // cursor) and a trailing newline + "ESC[?25h" (show cursor). We strip these so the captured
    // bytes are the bare Kitty APC sequence: mdcat positions and reserves space for the image
    // itself (emitImageParagraph with DECSC/DECRC), and leaving timg's stray newline in would
    // advance the cursor mid-band and corrupt the placement of tall, multi-chunk images.
    auto strip = [&](const std::string& lead) {
        if (out.compare(0, lead.size(), lead) == 0) out.erase(0, lead.size());
    };
    strip("\033[?25l");
    auto stripTail = [&](const std::string& tail) {
        if (out.size() >= tail.size() &&
            out.compare(out.size() - tail.size(), tail.size(), tail) == 0)
            out.erase(out.size() - tail.size());
    };
    stripTail("\033[?25h");
    stripTail("\n");
    return out;
}
}  // namespace

// The process-wide image pool, created on first use. Sized to hardware concurrency (at least one
// worker). Lazy + leaked-on-exit via a function-local static so terminal probes (graphicsBackend,
// cellMetrics) have run on the main thread before any worker touches them.
ThreadPool& imagePool() {
    static ThreadPool pool(std::max(1u, std::thread::hardware_concurrency()));
    return pool;
}

// If `text` (already trimmed) is a single <img> tag for a local PNG, render it and return true,
// writing the raw sixel bytes to `out` and the image's exact cell footprint to `cellWidth` /
// `cellHeight` (columns / rows). Otherwise return false and leave the outputs untouched.
// `availWidth` caps the width in columns. On any failure to produce an image, the alt text (or
// literal tag) is returned as a one-line text block (cellHeight == 1) so callers can treat the
// result uniformly.
//
// `out` is the plain sixel as timg produced it — it does NOT position itself. The caller is
// responsible for placing the cursor before replaying it: timg leaves the cursor at column 1 on the
// row just below the image, so a sixel painted at the top of a reserved block lands within it. The
// footprint is computed from the painted pixel size read back out of the sixel (which reflects
// timg's aspect-preserving fit), divided by the real cell size — not from the requested -g box.
// `kittyId` controls the Kitty image id stamped on a successful Kitty render: zero mints one now
// from nextKittyId() (the table and synchronous paths, which run in encounter order); a nonzero
// value is used verbatim; the sentinel kKittyIdDefer leaves timg's default id untouched, so a
// deferred slot (ADR 0003) can stamp a document-order id on the writer thread after parallel —
// possibly out-of-order — conversion completes, keeping output byte-identical to the serial
// renderer.
bool renderImageBlock(const std::string& text,
                      int availWidth,
                      const std::string& fileDir,
                      std::string& out,
                      int& cellWidth,
                      int& cellHeight,
                      bool forceWidthBound,
                      uint32_t kittyId) {
    std::map<std::string, std::string> attrs;
    if (!parseImgTag(text, attrs) && !parseMdImage(text, attrs)) return false;

    auto srcIt = attrs.find("src");

    // Fall back to a one-line text block when an image can't (or shouldn't) be drawn: the alt text
    // if present, else the source filename, else the literal tag. Used both for error cases and for
    // terminals that cannot display graphics at all.
    auto fallback = [&](const std::string& reason) {
        (void)reason;
        auto altIt = attrs.find("alt");
        std::string label;
        if (altIt != attrs.end() && !altIt->second.empty())
            label = altIt->second;
        else if (srcIt != attrs.end() && !srcIt->second.empty())
            label = srcIt->second;
        else
            label = text;
        auto hrefIt = attrs.find("href");
        if (hrefIt != attrs.end() && !hrefIt->second.empty())
            out = "\033]8;;" + hrefIt->second + "\033\\" + renderInline(label) + "\033]8;;\033\\";
        else
            out = renderInline(label);
        cellWidth = displayWidth(out);
        cellHeight = 1;
        return true;
    };

    // On a terminal that can't render sixel graphics, show the alt text or filename instead.
    if (!terminalSupportsGraphics()) return fallback("no graphics support");

    if (srcIt == attrs.end() || srcIt->second.empty()) return fallback("no src");
    std::string src = srcIt->second;

    // A remote image (e.g. a CI badge): fetch it ourselves, but only when the URL is https and its
    // host is trusted (git remotes + the user config file). The extension is checked on the path
    // component (query string stripped) so a badge URL like ".../badge.svg?branch=main" still
    // validates. An untrusted/non-https URL, an unsupported extension, or a failed fetch falls back
    // to alt text — exactly as a non-graphics terminal does. On success the bytes land in a temp
    // file that, from here on, is treated as an ordinary local image; tmpGuard unlinks it on every
    // return path.
    std::string tmpFile;
    struct TmpGuard {
        std::string& p;
        ~TmpGuard() {
            if (!p.empty()) unlink(p.c_str());
        }
    } tmpGuard{tmpFile};

    if (!src.empty() && src[0] != '/' && src.find("://") != std::string::npos) {
        std::string url = curateRemoteImageUrl(src, fileDir);
        if (url.empty()) return fallback("untrusted or unsupported remote image");
        std::string urlPath = url;
        if (size_t q = urlPath.find_first_of("?#"); q != std::string::npos)
            urlPath = urlPath.substr(0, q);
        if (!isSupportedImageExt(urlPath)) return fallback("not a supported image");
        tmpFile = fetchRemoteImage(url);
        if (tmpFile.empty()) return fallback("remote fetch failed");
        src = tmpFile;
    } else {
        // Resolve relative paths against the directory of the file being rendered. fileDir is
        // passed in (a snapshot of gFileDir) rather than read from the global, so image workers
        // stay global-free.
        if (!fileDir.empty() && src[0] != '/') src = fileDir + "/" + src;
    }

    if (!readImageSize(src)) return fallback("not a supported image");

    // Target pixel size: an explicit width/height overrides the intrinsic size. With only one of
    // the two given, the other is derived to preserve the aspect ratio; with both, the ratio is
    // free. When the intrinsic size is unknown (non-PNG formats return 0×0), only attribute
    // dimensions are available; if neither is given we fall back to a width-bound geometry below.
    auto attrInt = [&](const char* key, int& dst) {
        auto it = attrs.find(key);
        if (it == attrs.end()) return false;
        int v = std::atoi(it->second.c_str());
        if (v <= 0) return false;
        dst = v;
        return true;
    };
    int tw = 0, th = 0;
    int aw = 0, ah = 0;
    bool haveW = attrInt("width", aw), haveH = attrInt("height", ah);
    if (haveW) tw = aw;
    if (haveH) th = ah;

    // Decide the -g geometry to pass to timg. Rules, in priority order:
    //   - forceWidthBound: caller (table layout) demands an exact column width.
    //   - Image is wider than available columns: clamp to the column budget.
    //   - Explicit height only: height-bound, width follows aspect ratio.
    //   - Explicit width or both dimensions: full box.
    //   - No constraint at all (no attrs, intrinsic size fits or is unknown): omit -g entirely
    //     and let timg choose its own size; the footprint is read back from the sixel header.
    CellMetrics cell = cellMetrics();
    int wCells = cell.pxToColsNominal(tw);
    int hCells = cell.pxToRowsNominal(th);
    std::string geom;
    if (forceWidthBound && availWidth > 0)
        geom = std::to_string(availWidth) + "x";  // caller demands an exact column width
    else if (availWidth > 0 && wCells > availWidth)
        geom = std::to_string(availWidth) + "x";  // width-bound: intrinsic size too wide
    else if (haveH && !haveW)
        geom = "x" + std::to_string(hCells);  // height-bound: width follows aspect
    else if (haveW || haveH)
        geom = std::to_string(wCells) + "x" + std::to_string(hCells);  // explicit box
    // else: geom stays empty — no -g, timg picks its own size

    std::string img;
    if (graphicsBackend() == GraphicsBackend::Kitty) {
        // Kitty path: timg downscales into the -g box and emits a chunked Kitty APC; we only
        // rewrite its image id to a unique value. The footprint is the painted pixel size (read
        // from the embedded PNG's IHDR) converted with the precise area ratio, falling back to
        // requested cells.
        img = runTimgKitty(src, geom);
        if (img.empty()) return fallback("timg failed");
        if (kittyId != kKittyIdDefer) img = kittyRewriteId(img, kittyId ? kittyId : nextKittyId());
        int paintedW = 0, paintedH = 0;
        if (kittyImageSize(img, paintedW, paintedH)) {
            // Width is the binding axis (it is what a table column reserves, and the axis the
            // original overflow was on): the column count is the painted width in cells, but never
            // more than the caller's cap — timg sizes the -g box with a fixed ~9px cell, so the
            // painted pixels can map to a column or two more than reserved (the 38-vs-33 case in
            // table layout). Capping here, then pinning c=/r= below, makes the terminal scale into
            // exactly that box.
            cellWidth = cell.pxToCols(paintedW);
            if (availWidth > 0 && cellWidth > availWidth) cellWidth = availWidth;
            // Derive rows from the columns and the painted image's true aspect, rounded to NEAREST
            // (not ceil): ceil inflates one axis and, when the other axis can't follow, visibly
            // distorts a small image (a 3-column square would become c=3,r=2). Round-nearest keeps
            // c:r as close to the source aspect as the cell grid allows. realCellW/H give the
            // terminal's actual cell shape, so the c:r ratio matches the pixels the terminal will
            // blit into.
            double aspect = paintedW > 0 ? static_cast<double>(paintedH) / paintedW : 1.0;
            double rExact =
                static_cast<double>(cellWidth) * cell.realCellW() * aspect / cell.realCellH();
            cellHeight = std::max(1, static_cast<int>(rExact + 0.5));
        } else {
            cellWidth = wCells;
            cellHeight = hCells;
        }
        // Pin the terminal's cell footprint to the one mdcat reserves, so the image never overflows
        // its column or row band regardless of the local terminal's real cell size. See
        // kittyRewriteFootprint.
        img = kittyRewriteFootprint(img, cellWidth, cellHeight);
    } else {
        // Sixel path (unchanged): timg paints sixel; the footprint comes from the sixel raster
        // header.
        img = runTimg(src, geom);
        if (img.empty()) return fallback("timg failed");
        int paintedW = 0, paintedH = 0;
        if (sixelPixelSize(img, paintedW, paintedH)) {
            cellWidth = cell.pxToCols(paintedW);
            cellHeight = cell.pxToRows(paintedH);
        } else {
            cellWidth = wCells;
            cellHeight = hCells;
        }
    }
    auto hrefIt = attrs.find("href");
    if (hrefIt != attrs.end() && !hrefIt->second.empty())
        out = "\033]8;;" + hrefIt->second + "\033\\" + img + "\033]8;;\033\\";
    else
        out = img;
    return true;
}

// Replay a captured image (sixel or Kitty APC) at the cursor's current position, then leave the
// cursor exactly where it started. We bracket the bytes with DECSC/DECRC (ESC 7 / ESC 8): save the
// position, paint, restore. This is terminal-independent — where an image leaves the cursor differs
// between terminals (VSCode advances to the row below; iTerm does not), but the saved position is
// restored either way — so callers can move deterministically afterward without depending on that
// behaviour. Works for both protocols: neither moves the cursor predictably, so save/restore is the
// right tool.
void replaySixel(const std::string& image, std::ostream& out) {
    out << "\0337" << image << "\0338";
}

// Emit a captured sixel `image` (exactly `rows` tall) as a standalone block at the left margin,
// leaving the cursor at column 1 on the line just below it. Scroll-safe and needs no cursor report:
// print `rows` newlines to reserve the band (this scrolls if we are near the screen bottom, making
// room first), move the cursor back up `rows` lines (relative, so unaffected by any scroll), replay
// the sixel (which restores the cursor to the band top), then step down `rows` to the band bottom —
// ready for the next block, just like a text paragraph's trailing newline.
// Build the framed bytes for an image paragraph (the same sequence emitImageParagraph writes) as a
// string, so a deferred slot (ADR 0003) can hold the finished image without an ostream on hand.
std::string imageParagraphBytes(const std::string& image, int rows) {
    std::string s;
    s += std::string(static_cast<size_t>(rows), '\n');  // reserve the band (may scroll)
    s += "\033[" + std::to_string(rows) + 'A';          // back to the band's top row, column 1
    s += "\0337";
    s += image;
    s += "\0338";                                      // paint; cursor restored to band top
    s += "\033[" + std::to_string(rows) + 'B' + '\r';  // down to the band bottom, column 1
    return s;
}

void emitImageParagraph(const std::string& image, int rows, std::ostream& out) {
    out << imageParagraphBytes(image, rows);
}

}  // namespace mdcat
