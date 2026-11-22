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
#include "mdcat_inline.h"
#include "mdcat_math.h"
#include "mdcat_state.h"
#include "mdcat_style.h"
#include "mdcat_terminal.h"
#include "mdcat_text.h"
#include "mermaid.h"
#include "tty_guard.h"
#include "unicode_width.h"
#include "width_probe.h"

namespace mdcat {

// ---------------------------------------------------------------------------
// ANSI / OSC helpers
// ---------------------------------------------------------------------------

// --- Remote image trust ----------------------------------------------------------------------
//
// Viewing a Markdown file should not fetch arbitrary network resources. mdcat will fetch a remote
// image (e.g. a CI badge) ONLY when the URL is https and its host is on a trust list built from two
// sources, with no built-in default hosts:
//   1. The git remotes of the repository containing the file being rendered: if you push/pull to a
//      host you already trust it. Both SSH (git@github.com:owner/repo.git) and HTTPS
//      (https://github.com/owner/repo.git) remote forms are parsed for their host.
//   2. A user-maintained file at $XDG_CONFIG_HOME/mdcat/trusted-hosts (or ~/.config/mdcat/
//      trusted-hosts): one host per line, '#' starts a comment. A bare host (github.com) trusts
//      exactly that host; a line starting with '.' or '*.' (.github.com / *.github.com) also trusts
//      its subdomains.
// Anything not trusted falls back to the image's alt text, exactly as a non-graphics terminal does.
// mdcat fetches with curl (https-only, redirect-following, with a timeout and size cap) into a temp
// file and then hands that LOCAL file to the normal timg pipeline. The trust check is on the
// INITIAL url's host only; curl follows redirects to other hosts (GitHub's badge redirects to a
// pipelines host), an accepted trade-off so badges work.


// Extract the host from a git remote URL in either SSH or URL form. Returns "" if none can be
// found. Handles: scp-like SSH (git@host:path, host:path), ssh:// URLs, and https?:// URLs. Strips
// any userinfo (user@) and port (:NNN).
std::string gitRemoteHost(const std::string& remote) {
    std::string s = remote;
    // scheme://[userinfo@]host[:port]/...
    size_t scheme = s.find("://");
    if (scheme != std::string::npos) {
        std::string rest = s.substr(scheme + 3);
        size_t slash = rest.find('/');
        std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
        size_t at = authority.rfind('@');
        if (at != std::string::npos) authority = authority.substr(at + 1);
        size_t colon = authority.find(':');
        if (colon != std::string::npos) authority = authority.substr(0, colon);
        return toLowerCopy(authority);
    }
    // scp-like: [user@]host:path  (the first ':' that is NOT part of a path separates host and
    // path)
    size_t colon = s.find(':');
    if (colon != std::string::npos) {
        std::string authority = s.substr(0, colon);
        size_t at = authority.rfind('@');
        if (at != std::string::npos) authority = authority.substr(at + 1);
        return toLowerCopy(authority);
    }
    return std::string();
}

// Trusted-host entry: an exact host, optionally also matching subdomains.
struct TrustEntry {
    std::string host;        // already lowercased, no leading dot
    bool includeSubdomains;  // true for ".github.com" / "*.github.com" entries
};

bool trustEntryMatches(const TrustEntry& e, const std::string& host) {
    if (host == e.host) return true;
    if (e.includeSubdomains && host.size() > e.host.size() + 1 &&
        host.compare(host.size() - e.host.size(), e.host.size(), e.host) == 0 &&
        host[host.size() - e.host.size() - 1] == '.')
        return true;
    return false;
}

// Build (once) and return the trust list. fileDir scopes the git-remote lookup to the repository of
// the file being rendered.
const std::vector<TrustEntry>& trustedHosts(const std::string& fileDir) {
    static std::vector<TrustEntry> entries;
    static bool built = false;
    if (built) return entries;
    built = true;

    auto add = [&](std::string line) {
        // Strip comment and surrounding whitespace.
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return;
        size_t e = line.find_last_not_of(" \t\r\n");
        line = line.substr(b, e - b + 1);
        if (line.empty()) return;
        bool sub = false;
        if (line.rfind("*.", 0) == 0) {
            sub = true;
            line = line.substr(2);
        } else if (line[0] == '.') {
            sub = true;
            line = line.substr(1);
        }
        if (!line.empty()) entries.push_back({toLowerCopy(line), sub});
    };

    // 1. Git remotes of the file's repository. Run git in fileDir (or CWD if empty) so the lookup
    //    reflects that repo. Failure (not a repo, no git) just yields no entries.
    {
        std::string dir = fileDir.empty() ? "." : fileDir;
        std::string q = "'";
        for (char c : dir) q += (c == '\'') ? std::string("'\\''") : std::string(1, c);
        q += "'";
        std::string cmd = "git -C " + q + " config --get-regexp '^remote\\..*\\.url' 2>/dev/null";
        if (FILE* p = popen(cmd.c_str(), "r")) {
            char buf[4096];
            std::string acc;
            size_t got;
            while ((got = fread(buf, 1, sizeof buf, p)) > 0) acc.append(buf, got);
            pclose(p);
            size_t start = 0;
            while (start < acc.size()) {
                size_t nl = acc.find('\n', start);
                std::string row =
                    acc.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
                start = nl == std::string::npos ? acc.size() : nl + 1;
                // row is "remote.origin.url <value>"; take the value after the first space.
                size_t sp = row.find(' ');
                if (sp == std::string::npos) continue;
                std::string host = gitRemoteHost(row.substr(sp + 1));
                if (!host.empty()) entries.push_back({host, false});
            }
        }
    }

    // 2. User config file.
    {
        std::string base;
        if (const char* xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg)
            base = xdg;
        else if (const char* home = getenv("HOME"); home && *home)
            base = std::string(home) + "/.config";
        if (!base.empty()) {
            std::ifstream f(base + "/mdcat/trusted-hosts");
            std::string line;
            while (std::getline(f, line)) add(line);
        }
    }
    return entries;
}

// If src is a remote image URL we are permitted to fetch, return the (validated) URL; otherwise
// return "". Only https is allowed; the URL must have a clean host (no userinfo, no control/space
// characters) that is on the trust list for fileDir.
std::string curateRemoteImageUrl(const std::string& src, const std::string& fileDir) {
    static const std::string kHttps = "https://";
    if (src.size() <= kHttps.size() || toLowerCopy(src.substr(0, kHttps.size())) != kHttps)
        return std::string();
    for (unsigned char c : src)
        if (c < 0x20 || c == 0x7f || c == ' ') return std::string();
    std::string rest = src.substr(kHttps.size());
    size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    if (authority.find('@') != std::string::npos) return std::string();  // reject embedded creds
    std::string host = authority;
    if (size_t colon = host.find(':'); colon != std::string::npos) host = host.substr(0, colon);
    host = toLowerCopy(host);
    if (host.empty()) return std::string();
    for (const TrustEntry& e : trustedHosts(fileDir))
        if (trustEntryMatches(e, host)) return src;
    return std::string();
}

// Fetch a (already trusted, already validated) https image URL into a temp file and return its
// path, or "" on failure. The temp file keeps the URL's file extension (query string stripped) so
// the format-by-extension checks and timg both see e.g. ".svg". curl is invoked with a hard
// https-only proto restriction, redirect following (-L), a connect+total timeout, and a size cap so
// a viewing session can never block indefinitely or pull an unbounded body. The caller must unlink
// the returned path when done.
std::string fetchRemoteImage(const std::string& url) {
    // Determine the extension from the path component (drop ?query / #fragment).
    std::string path = url;
    if (size_t q = path.find_first_of("?#"); q != std::string::npos) path = path.substr(0, q);
    std::string ext;
    if (size_t dot = path.rfind('.'); dot != std::string::npos && dot > path.rfind('/'))
        ext = path.substr(dot);

    // Build "<tmpdir>/mdcat-XXXXXX<ext>" via mkstemps so the suffix is preserved.
    const char* tmpdir = getenv("TMPDIR");
    std::string tmpl = (tmpdir && *tmpdir ? std::string(tmpdir) : std::string("/tmp"));
    if (!tmpl.empty() && tmpl.back() != '/') tmpl += '/';
    tmpl += "mdcat-XXXXXX" + ext;
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = mkstemps(buf.data(), static_cast<int>(ext.size()));
    if (fd < 0) return std::string();
    close(fd);
    std::string out(buf.data());

    // Single-quote both the output path and URL for the shell.
    auto sh = [](const std::string& s) {
        std::string q = "'";
        for (char c : s) q += (c == '\'') ? std::string("'\\''") : std::string(1, c);
        q += "'";
        return q;
    };
    std::string cmd =
        "curl -fsSL --proto '=https' --max-time 15 --connect-timeout 8 "
        "--max-filesize 16000000 -o " +
        sh(out) + " " + sh(url) + " >/dev/null 2>&1";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        unlink(out.c_str());
        return std::string();
    }
    // Reject an empty body (curl can write a 0-byte file on some odd success paths).
    std::ifstream f(out, std::ios::binary | std::ios::ate);
    if (!f.good() || f.tellg() == 0) {
        unlink(out.c_str());
        return std::string();
    }
    return out;
}

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
static bool scanMdImageAt(const std::string& s, size_t& i, std::string& alt, std::string& src) {
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

// A per-run monotonic Kitty image id. timg assigns the SAME id to every render of a given file, so
// two images in one document would collide — a later transmit replaces the earlier image and
// re-lays its placement. Handing out a fresh id per image keeps them independent. Atomic so
// parallel image workers (ADR 0003) can mint ids concurrently without colliding or racing on the
// counter.
uint32_t nextKittyId() {
    static std::atomic<uint32_t> id{1000};
    return ++id;
}

// Sentinel kittyId for renderImageBlock: "leave timg's default id; the id is stamped later, in
// document order, on the writer thread" (ADR 0003 deferred slots). Distinct from 0 (mint now).
constexpr uint32_t kKittyIdDefer = 0xFFFFFFFFu;

// Set the i= id on the FIRST Kitty controls segment of `apc` to `id` (byte-exact, leaving the
// base64 payload and all chunk continuations untouched). An existing i= value is rewritten;
// when timg omitted it (older timg — Debian's 1.4.3 emits a bare `a=T,f=100,m=0` with neither
// i= nor q=), i=<id> is INSERTED right after `a=T`. Without an id the terminal assigns the image
// the default id 0, so every image in a document would collide on 0 (a later transmit replaces
// the earlier image) — and its OK reply reports `i=0`. Inserting a unique id fixes both.
std::string kittyRewriteId(const std::string& apc, uint32_t id) {
    size_t g = apc.find("\033_G");
    if (g == std::string::npos) return apc;
    size_t keyStart = g + 3;
    size_t semi = apc.find(';', keyStart);  // controls run up to the first ';'
    if (semi == std::string::npos) return apc;
    size_t ip = apc.find("i=", keyStart);
    if (ip != std::string::npos && ip < semi) {  // rewrite an existing i=
        size_t numStart = ip + 2;
        size_t numEnd = numStart;
        while (numEnd < semi && std::isdigit((unsigned char)apc[numEnd])) ++numEnd;
        return apc.substr(0, numStart) + std::to_string(id) + apc.substr(numEnd);
    }
    // No i= present: insert it at the head of the controls run (controls always start with a=T
    // on a real first chunk, so a leading "i=<id>," is well-formed).
    return apc.substr(0, keyStart) + "i=" + std::to_string(id) + "," + apc.substr(keyStart);
}

// Whether the Kitty controls run in [ctrlStart, ctrlEnd) is an image's FIRST chunk — i.e. it
// carries the action key `a=` (a=T transmit+display, a=t transmit-only). Continuation chunks
// carry only `m=` (and, with some timg builds, `q=`), so `a=` is the reliable first-chunk marker.
// Keying off `a=` rather than `i=` matters because older timg emits no i= at all (see
// kittyRewriteId).
static bool kittyIsFirstChunk(const std::string& s, size_t ctrlStart, size_t ctrlEnd) {
    // Search only the controls run: an unbounded find() on a continuation chunk (which has no
    // a=) would scan the rest of the buffer's base64, making renumbering quadratic in image size.
    static const char key[] = "a=";
    return std::search(s.begin() + static_cast<std::ptrdiff_t>(ctrlStart),
                       s.begin() + static_cast<std::ptrdiff_t>(ctrlEnd),
                       key,
                       key + 2) != s.begin() + static_cast<std::ptrdiff_t>(ctrlEnd);
}

// Renumber the i= id of every Kitty image in `bytes`, in order, with successive ids from
// nextKittyId(). Each image's FIRST chunk carries the action key (a=T); continuation chunks (m=)
// carry no a= and are left alone, so exactly one fresh id is minted per image. Used by the
// writer thread (ADR 0003) to stamp ids in document (slot) order, regardless of the order parallel
// workers produced the bytes — keeping output byte-identical to the serial renderer, which minted
// ids in document order on one thread. `bytes` may hold zero, one, or many images (a single
// deferred <img>, or a whole table row).
std::string kittyRenumberAll(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size());
    size_t pos = 0;
    for (;;) {
        size_t g = bytes.find("\033_G", pos);
        if (g == std::string::npos) {
            out.append(bytes, pos, std::string::npos);
            break;
        }
        size_t semi = bytes.find(';', g + 3);
        out.append(bytes, pos, g - pos);  // bytes before this APC
        if (semi == std::string::npos || !kittyIsFirstChunk(bytes, g + 3, semi)) {
            // No action key in this controls run: a continuation chunk (or not an image). Copy
            // "\033_G" only and continue scanning just past it.
            out.append("\033_G");
            pos = g + 3;
            continue;
        }
        // An image's first chunk: set its i= to the next document-order id (inserting i= when
        // timg emitted none) and copy through the ';'.
        out.append(kittyRewriteId(bytes.substr(g, semi - g + 1), nextKittyId()));
        pos = semi + 1;
    }
    return out;
}

// Inject (or replace) the cell-footprint controls c=<cols>,r=<rows> on the FIRST Kitty controls
// segment of `apc`, byte-exact. timg emits no c=/r=, so the terminal would otherwise derive the
// footprint by dividing the PNG's pixel size by ITS OWN cell size — which differs from the ~9px
// cell timg assumed when it sized the image (it has no terminal to query on a pipe). That mismatch
// makes a width-bound table image paint wider than the column mdcat reserved, overlapping the next
// cell. With c=/r= the terminal scales the image into EXACTLY that cell box, so the painted
// footprint always equals mdcat's reservation. (iTerm requires BOTH c and r and honours them
// exactly — verified via tools/probe-kitty-aspect.sh — so we always set both; aspect is preserved
// because cols and rows are computed from the same painted pixels and the same real cell ratio.)
std::string kittyRewriteFootprint(const std::string& apc, int cols, int rows) {
    if (cols <= 0 || rows <= 0) return apc;
    size_t g = apc.find("\033_G");
    if (g == std::string::npos) return apc;
    size_t semi = apc.find(';', g + 3);  // controls run up to the first ';'
    if (semi == std::string::npos) return apc;
    // Strip any existing c=/r= keys from the controls run so we never duplicate them.
    std::string ctrls = apc.substr(g + 3, semi - (g + 3));
    std::string kept;
    size_t i = 0;
    while (i < ctrls.size()) {
        size_t comma = ctrls.find(',', i);
        std::string kv =
            ctrls.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
        if (kv.compare(0, 2, "c=") != 0 && kv.compare(0, 2, "r=") != 0) {
            if (!kept.empty()) kept += ',';
            kept += kv;
        }
        if (comma == std::string::npos) break;
        i = comma + 1;
    }
    std::string footprint = ",c=" + std::to_string(cols) + ",r=" + std::to_string(rows);
    return apc.substr(0, g + 3) + kept + footprint + apc.substr(semi);
}

// Ensure every Kitty APC in `bytes` suppresses ALL terminal replies with q=2. For each control
// section: rewrite an existing q=<n> to q=2, and on an image's first chunk that carries NO q= at
// all, insert one. Two cases matter. (1) The missing-q= case — older timg (Debian's 1.4.3) emits
// a bare `a=T,...` with no quiet flag, so the terminal defaults to q=0 (always reply). (2) The
// wrong-level case — iTerm2 ignores q=1 (which per spec only suppresses success replies) and
// still sends `ESC_Gi=0;OK ESC\` back; only q=2 silences it (see gmore_emulator.cpp). Over SSH
// those replies pile up in the remote pty's input buffer and spill onto the shell prompt as
// garbage after mdcat exits. mdcat never reads Kitty replies, so q=2 is always the right level.
std::string kittyQ2(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size() + 16);
    size_t pos = 0;
    for (;;) {
        size_t found = bytes.find("\033_G", pos);
        if (found == std::string::npos) {
            out.append(bytes, pos, std::string::npos);
            break;
        }
        out.append(bytes, pos, found - pos);  // bytes before this APC
        size_t ctrlStart = found + 3;
        size_t semi = bytes.find(';', ctrlStart);
        size_t st = bytes.find("\033\\", ctrlStart);
        size_t ctrlEnd = std::min(semi != std::string::npos ? semi : bytes.size(),
                                  st != std::string::npos ? st : bytes.size());
        // Bounded to the controls run (see kittyIsFirstChunk): an unbounded find() on a chunk
        // lacking q= would scan the rest of the payload, making this quadratic.
        static const char qkey[] = "q=";
        auto qit = std::search(bytes.begin() + static_cast<std::ptrdiff_t>(ctrlStart),
                               bytes.begin() + static_cast<std::ptrdiff_t>(ctrlEnd),
                               qkey,
                               qkey + 2);
        size_t q = qit == bytes.begin() + static_cast<std::ptrdiff_t>(ctrlEnd)
            ? std::string::npos
            : static_cast<size_t>(qit - bytes.begin());
        if (q != std::string::npos && q + 2 < ctrlEnd) {
            // Existing q=<n>: copy through it, force the value to 2, skip the old digit.
            out.append(bytes, found, (q + 2) - found);
            out += '2';
            pos = q + 3;  // past "q=" + one value digit
        } else {
            // No q= in THIS control section — insert q=2. This must happen on EVERY chunk,
            // not just the first: iTerm2 keys its ;OK reply off the FINAL chunk (m=0), which
            // in Debian timg 1.4.3's output is a bare `m=0` with no q= (→ q=0, always reply)
            // and no i= (so the reply is the tell-tale `ESC_Gi=0;OK ESC\`). macOS timg 1.6.3
            // happens to stamp q= on every chunk, which is why the first-chunk-only rewrite
            // masked this on macOS but not over SSH to the Ubuntu box.
            out.append("\033_Gq=2");
            if (ctrlStart != ctrlEnd) out += ',';  // empty controls (`ESC_G;…`): no trailing comma
            pos = ctrlStart;
        }
    }
    return out;
}

// Read a PNG's pixel width/height from the IHDR (big-endian uint32s at byte offsets 16 and 20,
// right after the 8-byte signature + 4-byte length + "IHDR"). `png` is the raw PNG bytes. Returns
// false if the buffer is too short or not a PNG.
bool pngSize(const std::string& png, int& w, int& h) {
    if (png.size() < 24) return false;
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8; ++i)
        if (static_cast<unsigned char>(png[i]) != sig[i]) return false;
    auto u32 = [&](size_t o) {
        return (static_cast<unsigned char>(png[o]) << 24) |
            (static_cast<unsigned char>(png[o + 1]) << 16) |
            (static_cast<unsigned char>(png[o + 2]) << 8) | static_cast<unsigned char>(png[o + 3]);
    };
    w = u32(16);
    h = u32(20);
    return w > 0 && h > 0;
}

// Decode the base64 PNG payload out of a Kitty APC (concatenating all chunks) and read its pixel
// size, so the caller can compute the exact cell footprint timg's downscaled image will occupy.
// Returns false if no PNG could be recovered. Only a small prefix is needed (the IHDR), but timg's
// first chunk already contains it, so we decode just the first chunk's payload.
bool kittyImageSize(const std::string& apc, int& pw, int& ph) {
    size_t g = apc.find("\033_G");
    if (g == std::string::npos) return false;
    size_t semi = apc.find(';', g);
    if (semi == std::string::npos) return false;
    size_t end = apc.find("\033\\", semi);
    if (end == std::string::npos) return false;
    std::string b64 = apc.substr(semi + 1, end - semi - 1);
    // Minimal base64 decode (standard alphabet, no whitespace in Kitty payloads).
    static const std::string tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (char c : b64) {
        if (c == '=') break;
        size_t pos = tbl.find(c);
        if (pos == std::string::npos) continue;
        val = (val << 6) | static_cast<int>(pos);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
        if (out.size() >= 24) break;  // enough for the IHDR
    }
    return pngSize(out, pw, ph);
}

// Read the painted pixel size out of a sixel data stream's raster attributes. A sixel begins
// ESC P <params> q "Pan;Pad;Ph;Pv ... where Ph/Pv are the graphic's pixel width/height. We scan for
// the '"' that introduces them and parse the last two of the four numbers. Returns false if absent
// (older sixel without raster attributes), in which case callers fall back to the requested size.
bool sixelPixelSize(const std::string& sixel, int& pw, int& ph) {
    size_t q = sixel.find("\033P");
    if (q == std::string::npos) return false;
    size_t quote = sixel.find('"', q);
    if (quote == std::string::npos) return false;
    int pan = 0, pad = 0, w = 0, h = 0;
    if (std::sscanf(sixel.c_str() + quote + 1, "%d;%d;%d;%d", &pan, &pad, &w, &h) == 4 && w > 0 &&
        h > 0) {
        pw = w;
        ph = h;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Image worker thread pool (ADR 0003)
// ---------------------------------------------------------------------------
//
// A fixed pool of N = hardware-concurrency workers that run image-conversion tasks (timg / mmdc
// subprocesses) off the render thread. Each task is a std::function returning void and captures its
// own inputs/outputs, so the pool stays agnostic about ImageJob shape; callers submit() a packaged
// task and wait on the returned future. For now (step 2) call sites submit and immediately .get(),
// so behaviour and output are unchanged — later steps overlap the work.
class ThreadPool {
public:
    explicit ThreadPool(unsigned n) {
        for (unsigned i = 0; i < n; ++i) workers_.emplace_back([this] { workerLoop(); });
    }
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }

    // Enqueue `fn` and return a future for its result. The task runs on some worker thread.
    // R is deduced with decltype rather than std::invoke_result/result_of: result_of was removed
    // in C++20, and some libc++ builds gate invoke_result behind -std=c++20, so decltype is the
    // portable spelling that works across the toolchains this project is built with.
    template <class F>
    auto submit(F&& fn) -> std::future<decltype(fn())> {
        using R = decltype(fn());
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(mu_);
            queue_.emplace([task] { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

private:
    void workerLoop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                job = std::move(queue_.front());
                queue_.pop();
            }
            job();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> queue_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
};

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
                      bool forceWidthBound = false,
                      uint32_t kittyId = 0) {
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
                        uint32_t kittyId = 0) {
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

// Whether `s` is sixel image data rather than a text fallback: a sixel contains a DCS introducer
// (ESC P), which rendered inline text never does (it uses only ESC[ for SGR and ESC] for OSC 8).
bool isSixelImage(const std::string& s) {
    return s.find("\033P") != std::string::npos;
}

// Whether `s` is Kitty graphics image data: a Kitty APC begins ESC _ G.
bool isKittyImage(const std::string& s) {
    return s.find("\033_G") != std::string::npos;
}

// Whether `s` is an inline image (either protocol) rather than a one-line text fallback.
bool isImageBlock(const std::string& s) {
    return isSixelImage(s) || isKittyImage(s);
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

std::string base64Encode(const std::string& in) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        size_t n = std::min<size_t>(3, in.size() - i);
        uint32_t v = static_cast<uint32_t>(static_cast<unsigned char>(in[i])) << 16;
        if (n > 1) v |= static_cast<uint32_t>(static_cast<unsigned char>(in[i + 1])) << 8;
        if (n > 2) v |= static_cast<uint32_t>(static_cast<unsigned char>(in[i + 2]));
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += n > 1 ? kAlphabet[(v >> 6) & 63] : '=';
        out += n > 2 ? kAlphabet[v & 63] : '=';
    }
    return out;
}

// A Kitty APC that transmits and displays `png` (f=100) in a `cols` x `rows` cell box, written by
// mdcat itself rather than by timg (ADR 0004). C=1 leaves the cursor where it was. Draw the image
// over the text. Images must use transparency where the text needs to be visible. The payload
// is split into chunks of at most 4096 base64 bytes, and every chunk carries q=2 (suppress all
// replies; see kittyQ2).
std::string kittyApcFromPng(
    const std::string& png, int cols, int rows, uint32_t id, int xOffset = 0) {
    constexpr size_t kChunk = 4096;
    std::string b64 = base64Encode(png);
    std::string apc;
    apc.reserve(b64.size() + b64.size() / kChunk * 16 + 96);
    for (size_t pos = 0; pos < b64.size() || pos == 0; pos += kChunk) {
        bool first = pos == 0;
        bool more = pos + kChunk < b64.size();
        apc += "\033_G";
        if (first)
            apc += "a=T,f=100,i=" + std::to_string(id) + ",q=2,C=1,c=" + std::to_string(cols) +
                ",r=" + std::to_string(rows) + ",z=0" +
                (xOffset > 0 ? ",X=" + std::to_string(xOffset) : std::string()) +
                (more ? ",m=1" : "");
        else
            apc += std::string("q=2,m=") + (more ? "1" : "0");
        apc += ';';
        apc.append(b64, pos, kChunk);
        apc += "\033\\";
    }
    return apc;
}

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

// The non-tty output sink, defined near main(). If `out` is backed by one, deferred image slots can
// be reserved on it (ADR 0003); otherwise (tty buffer, or a recursive ostringstream) images render
// synchronously inline. Forward-declared (with thin free-function wrappers, defined after the
// class) so the deferred call sites here can detect the sink and reserve a slot without the
// complete type.
class SlotSink;
SlotSink* asSlotSink(std::ostream& out);
void slotDeferImage(SlotSink* sink, std::future<std::string> fut);

// A Markdown string of a diagram label as styled lines that stand alone (defined below).
std::vector<std::string> mermaidMarkdownLines(const std::string& md, int width);

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

// ---------------------------------------------------------------------------
// Block model
// ---------------------------------------------------------------------------


// defined below, near emitHeading (which uses it too)
std::string renderLeadingImageBlock(const std::string& text,
                                    int availW,
                                    const std::string& fileDir,
                                    bool bold,
                                    const std::string& rule,
                                    uint32_t kittyId = 0);

void emitParagraph(const std::vector<std::string>& lines, std::ostream& out) {
    // Join the paragraph's source lines (a soft line break becomes a space), render inline markup,
    // then reflow the styled result to the terminal width by display columns.
    std::string joined;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) joined += ' ';
        joined += trim(lines[i]);
    }
    // A paragraph that is just an <img> tag for a local PNG renders as an image instead of text.
    // renderImageBlock returns either a multi-row sixel (ih > 1, or ih == 1 with sixel bytes) or a
    // one-line text fallback (ih == 1, plain text — e.g. on a non-graphics terminal). The sixel is
    // placed via the reserve-and-paint helper; the text fallback is emitted as a normal line.
    std::string imgText = trim(joined);
    int availW = terminalWidth();
    std::string fileDir = gFileDir;
    // Quick non-rendering test: is this paragraph an image tag at all? parseImgTag/parseMdImage are
    // pure and cheap, so checking here avoids submitting a job for ordinary paragraphs. (A
    // graphics- capable check is implicit — renderImageBlock falls back to text when graphics are
    // unavailable.)
    {
        std::map<std::string, std::string> probe;
        bool isImgTag = parseImgTag(imgText, probe) || parseMdImage(imgText, probe);
        if (isImgTag) {
            // Deferred slot (non-tty): submit the conversion, reserve an ordered slot for its
            // framed bytes, and return immediately so later blocks/images proceed in parallel. The
            // worker produces the exact bytes this site would have written: framed image, or text
            // fallback.
            if (SlotSink* sink = asSlotSink(out)) {
                // Defer the Kitty id: the worker leaves timg's default id and the writer stamps a
                // document-order id when it drains this slot, so out-of-order completion stays
                // serial- identical (see kKittyIdDefer and SlotSink::deferImage's stampKittyId).
                std::future<std::string> fut = imagePool().submit([imgText, availW, fileDir] {
                    std::string image;
                    int iw = 0, ih = 0;
                    if (!renderImageBlock(imgText,
                                          availW,
                                          fileDir,
                                          image,
                                          iw,
                                          ih,
                                          /*forceWidthBound=*/false,
                                          kKittyIdDefer) ||
                        image.empty())
                        return std::string();
                    if (isImageBlock(image)) return imageParagraphBytes(image, ih);
                    return image + "\n";  // one-line text fallback
                });
                slotDeferImage(sink, std::move(fut));
                return;
            }
            // Synchronous path (tty buffer or recursive ostringstream): render now and write
            // inline.
            std::string image;
            int iw = 0, ih = 0;
            if (imagePool()
                    .submit(
                        [&] { return renderImageBlock(imgText, availW, fileDir, image, iw, ih); })
                    .get()) {
                if (image.empty()) return;
                if (isImageBlock(image))
                    emitImageParagraph(image, ih, out);
                else
                    out << image << '\n';  // one-line text fallback
                return;
            }
        }
    }
    // A paragraph that STARTS with a Markdown image followed by more text, e.g.
    // "![Logo](logo.png) A short blurb", gets the same two-column treatment as a heading (see
    // renderLeadingImageBlock/emitHeading): image on the left, text to its right. If the leading
    // token turns out not to be a renderable image (no graphics support, bad file, etc.) the
    // worker falls back to the ordinary reflowed-paragraph text instead, computed right here
    // (cheap: no timg involved) and captured into the deferred bytes so the slot is unconditional
    // like the whole-image-paragraph case above.
    if (!imgText.empty() && imgText[0] == '!') {
        std::string textFallback;
        {
            std::ostringstream fb;
            std::string reflowed = reflow(renderInline(joined), availW);
            if (!reflowed.empty()) fb << reflowed << '\n';
            textFallback = fb.str();
        }
        if (SlotSink* sink = asSlotSink(out)) {
            std::future<std::string> fut =
                imagePool().submit([imgText, availW, fileDir, textFallback] {
                    std::string withImage = renderLeadingImageBlock(
                        imgText, availW, fileDir, /*bold=*/false, /*rule=*/"", kKittyIdDefer);
                    return withImage.empty() ? textFallback : withImage;
                });
            slotDeferImage(sink, std::move(fut));
            return;
        }
        std::string withImage = imagePool()
                                    .submit([&] {
                                        return renderLeadingImageBlock(imgText,
                                                                       availW,
                                                                       fileDir,
                                                                       /*bold=*/false,
                                                                       /*rule=*/"");
                                    })
                                    .get();
        out << (withImage.empty() ? textFallback : withImage);
        return;
    }
    // A paragraph that is exactly one block-math expression ($$...$$) is centered, the way GitHub
    // (and TeX display math) sets it off on its own centered line. Each rendered line is centered
    // independently within the available width.
    std::string trimmed = trim(joined);
    bool blockMath = trimmed.size() >= 4 && trimmed.compare(0, 2, "$$") == 0 &&
        trimmed.compare(trimmed.size() - 2, 2, "$$") == 0 &&
        trimmed.find("$$", 2) == trimmed.size() - 2;

    if (blockMath) {
        // Display math: try a two-dimensional layout so a top-level \frac stacks numerator over
        // denominator (renderMathStacked). If the expression has no top-level \frac it returns
        // false and we fall back to the flat single-line rendering below. The stacked box's rows
        // are each centered within the terminal, keeping the whole figure horizontally centered.
        std::string tex = trim(trimmed.substr(2, trimmed.size() - 4));
        MathBox box;
        if (renderMathStacked(tex, box)) {
            int pad = std::max(0, (terminalWidth() - box.width) / 2);
            for (const std::string& row : box.rows) {
                // Right-trim the row so a centered blank tail doesn't emit trailing spaces; the
                // constant left pad (same for every row) keeps the columns aligned.
                std::string trimmedRow = row;
                while (!trimmedRow.empty() && trimmedRow.back() == ' ') trimmedRow.pop_back();
                out << std::string(static_cast<size_t>(pad), ' ') << trimmedRow << '\n';
            }
            return;
        }
    }

    std::string reflowed = reflow(renderInline(joined), terminalWidth());
    if (reflowed.empty()) return;
    if (blockMath) {
        for (const std::string& line : splitOnNewlines(reflowed)) {
            int w = displayWidth(line);
            int pad = std::max(0, (terminalWidth() - w) / 2);
            out << std::string(static_cast<size_t>(pad), ' ') << line << '\n';
        }
        return;
    }
    out << reflowed << '\n';
}

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
                                    uint32_t kittyId) {
    if (text.empty() || text[0] != '!') return "";
    size_t i = 0;
    std::string alt, src;
    if (!scanMdImageAt(text, i, alt, src)) return "";
    std::string rest = trim(text.substr(i));

    std::string image;
    int iw = 0, ih = 0;
    std::string imgTag = text.substr(0, i);
    if (!renderImageBlock(
            imgTag, availW, fileDir, image, iw, ih, /*forceWidthBound=*/false, kittyId) ||
        image.empty() || !isImageBlock(image))
        return "";

    // A logo sized to its own natural width can be arbitrarily tall (e.g. a portrait graphic),
    // which would balloon a single heading line into a huge block. Cap it to a small height by
    // re-rendering height-bound through a synthetic <img height=...> tag (routes through
    // renderImageBlock's existing height-bound geometry path; width follows the image's aspect).
    // The requested pixel height and the measured footprint go through independent roundings
    // (request: nominal cell; footprint: the terminal's real cell), so a single attempt at exactly
    // the cap can still measure one row over. Shrink and retry a few times until it actually fits.
    constexpr int kMaxLeadingImageRows = 3;
    if (ih > kMaxLeadingImageRows) {
        // src/alt travel through an attribute value quoted with '"'; a literal '"' in either
        // (rare, but Markdown does not forbid it) would break the synthetic tag's parse, so drop
        // it rather than escape it (alt/src are cosmetic here — the geometry is what matters).
        std::string safeSrc = src, safeAlt = alt;
        safeSrc.erase(std::remove(safeSrc.begin(), safeSrc.end(), '"'), safeSrc.end());
        safeAlt.erase(std::remove(safeAlt.begin(), safeAlt.end(), '"'), safeAlt.end());
        int targetRows = kMaxLeadingImageRows;
        for (int attempt = 0; attempt < 4 && targetRows >= 1; ++attempt) {
            std::string boundTag = "<img src=\"" + safeSrc + "\" alt=\"" + safeAlt +
                "\" height=\"" + std::to_string(targetRows * CellMetrics::kNominalH) + "px\">";
            std::string boundImage;
            int biw = 0, bih = 0;
            if (!renderImageBlock(boundTag,
                                  availW,
                                  fileDir,
                                  boundImage,
                                  biw,
                                  bih,
                                  /*forceWidthBound=*/false,
                                  kittyId) ||
                boundImage.empty() || !isImageBlock(boundImage))
                break;  // re-render failed; keep whatever the previous attempt produced
            image = boundImage;
            iw = biw;
            ih = bih;
            if (ih <= kMaxLeadingImageRows) break;
            --targetRows;  // still too tall: ask for one fewer row and try again
        }
    }

    std::string styled = renderInline(rest);
    int textW = std::max(1, availW - iw - 2);
    std::vector<std::string> textLines = splitOnNewlines(reflow(styled, textW));
    if (textLines.empty()) textLines.emplace_back();
    int bandH = std::max(ih, static_cast<int>(textLines.size()));

    // Vertically center the text lines within the band (extra padding split top/bottom).
    int topPad = (bandH - static_cast<int>(textLines.size())) / 2;

    std::ostringstream out;
    for (int k = 0; k < bandH; ++k) {
        out << std::string(static_cast<size_t>(iw), ' ') << "  ";
        int textIdx = k - topPad;
        if (textIdx >= 0 && textIdx < static_cast<int>(textLines.size())) {
            if (bold) out << kBoldOn;
            out << textLines[textIdx];
            if (bold) out << kBoldOff;
        }
        out << '\n';
    }
    out << "\033[" << bandH << 'A';          // up to the band top
    replaySixel(image, out);                 // paint; cursor restored to band top
    out << "\033[" << bandH << 'B' << '\r';  // down to the band bottom, column 1
    if (!rule.empty()) out << rule << '\n';
    return out.str();
}

void emitHeading(int level, const std::string& text, std::ostream& out) {
    static const std::vector<std::string> kUnderlines = {"━", "─", "┈", "┄"};
    int availW = terminalWidth();
    std::string rule = horizontalLine(availW, kUnderlines[std::min(level, 4) - 1]);
    // Quick non-rendering test: does this heading even start with an image tag? Avoids submitting
    // a pool job (and, on the sync path, blocking on it) for the common case of a plain heading.
    if (!text.empty() && text[0] == '!') {
        std::string fileDir = gFileDir;
        if (SlotSink* sink = asSlotSink(out)) {
            // Defer the Kitty id: the writer stamps a document-order id when it drains this slot
            // (see kKittyIdDefer), so out-of-order completion stays serial-identical. If the text
            // does not actually start with a renderable image, the worker returns "" and the
            // slot's bytes are the plain heading text instead, rendered synchronously here (cheap:
            // it never involves timg) so it can be captured into the deferred bytes.
            std::string plainFallback;
            {
                std::ostringstream fb;
                std::string styled = renderInline(text);
                fb << kBoldOn << styled << kBoldOff << '\n' << rule << '\n';
                plainFallback = fb.str();
            }
            std::future<std::string> fut =
                imagePool().submit([text, availW, fileDir, rule, plainFallback] {
                    std::string withImage = renderLeadingImageBlock(
                        text, availW, fileDir, /*bold=*/true, rule, kKittyIdDefer);
                    return withImage.empty() ? plainFallback : withImage;
                });
            slotDeferImage(sink, std::move(fut));
            return;
        } else {
            std::string withImage =
                imagePool()
                    .submit([&] {
                        return renderLeadingImageBlock(text, availW, fileDir, /*bold=*/true, rule);
                    })
                    .get();
            if (!withImage.empty()) {
                out << withImage;
                return;
            }
        }
    }
    // Bold heading text, followed by an underline whose character depends on the level, matching
    // the original script's "━─┈┄" for levels 1..4 (deeper levels reuse the level-4 character).
    std::string styled = renderInline(text);
    out << kBoldOn << styled << kBoldOff << '\n' << rule << '\n';
}

void emitThematicBreak(std::ostream& out) {
    out << horizontalLine(terminalWidth()) << '\n';
}

void emitCodeBlock(const std::vector<std::string>& lines,
                   const std::string& lang,
                   std::ostream& out) {
    // Fenced code: print with the light-gray background used for inline code, padded to the
    // width of the widest line so the block reads as a solid panel.  Syntax highlighting is
    // applied when a recognized language tag is present.
    //
    // kCodeOff only resets fg/bg; cancel bold+italic explicitly so highlighted attributes
    // cannot leak past the line boundary into subsequent terminal output.
    const std::string kAttrOff = "\033[22;23m";  // cancel bold (22) and italic (23)
    const auto highlighted = highlightCode(lines, lang);
    int maxw = 0;
    for (const auto& l : lines) maxw = std::max(maxw, displayWidth(l));
    // Clamp the panel width so a row (2-space margins + content) never exceeds the terminal width.
    // Code lines are emitted verbatim (never reflowed), so a line longer than this still overflows
    // and wraps — that is unavoidable — but short lines must not be over-padded to the widest
    // line's width, which would push them past the edge and make the pager wrap them into phantom
    // rows with a broken background (gmore renders into a fixed-width grid; the terminal soft-wraps
    // under cat).
    maxw = std::min(maxw, std::max(1, terminalWidth() - 4));
    for (const auto& l : highlighted) {
        out << kCodeOn << "  " << padTo(l, maxw) << "  " << kAttrOff << kCodeOff << '\n';
    }
}

// Is `line` a GFM table delimiter row, e.g. |---|:--:| ? It must contain only pipes, colons,
// hyphens and spaces, include at least one hyphen, and at least one pipe.
bool isTableDelimiterRow(const std::string& line) {
    bool hasDash = false, hasPipe = false;
    for (char c : line) {
        if (c == '-')
            hasDash = true;
        else if (c == '|')
            hasPipe = true;
        else if (c != ':' && c != ' ' && c != '\t')
            return false;
    }
    return hasDash && hasPipe;
}

// Split a table row on unescaped '|', dropping one optional leading and trailing pipe, and trim
// each resulting cell. Pipes escaped as \| become literal pipes within a cell.
std::vector<std::string> splitTableRow(const std::string& raw) {
    std::string row = trim(raw);
    size_t b = 0, e = row.size();
    if (b < e && row[b] == '|') ++b;
    if (e > b && row[e - 1] == '|') --e;
    std::vector<std::string> cells;
    std::string cur;
    for (size_t i = b; i < e; ++i) {
        if (row[i] == '\\' && i + 1 < e && row[i + 1] == '|') {
            cur += '|';
            ++i;
            continue;
        }
        if (row[i] == '|') {
            cells.push_back(trim(cur));
            cur.clear();
            continue;
        }
        cur += row[i];
    }
    cells.push_back(trim(cur));
    return cells;
}

// Parse the per-column alignment from a delimiter row. Each cell is one of `---` (left, the
// default), `:--` (left), `--:` (right), or `:-:` (center); a leading colon means "left edge
// pinned", a trailing colon "right edge pinned". Cells are split the same way as data rows.
std::vector<Align> parseTableAlignment(const std::string& delimiter) {
    std::vector<Align> aligns;
    for (const std::string& cell : splitTableRow(delimiter)) {
        std::string c = trim(cell);
        bool left = !c.empty() && c.front() == ':';
        bool right = !c.empty() && c.back() == ':';
        if (left && right)
            aligns.push_back(Align::Center);
        else if (right)
            aligns.push_back(Align::Right);
        else
            aligns.push_back(Align::Left);
    }
    return aligns;
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

void emitTable(const std::vector<std::string>& rawRows, std::ostream& out) {
    // rawRows[0] is the header, rawRows[1] the delimiter (consumed by the caller's detection and
    // not printed), and the rest are body rows. Cells are inline-rendered.
    std::vector<std::vector<std::string>> rows;  // rendered cells, header + body
    rows.push_back(splitTableRow(rawRows[0]));
    for (size_t i = 2; i < rawRows.size(); ++i) rows.push_back(splitTableRow(rawRows[i]));

    size_t ncols = 0;
    for (auto& r : rows) ncols = std::max(ncols, r.size());
    for (auto& r : rows) r.resize(ncols);  // pad short rows with empty cells
    if (ncols == 0) return;

    // Per-column alignment from the delimiter row, padded to ncols with the Left default for any
    // columns the delimiter did not specify.
    std::vector<Align> aligns = parseTableAlignment(rawRows[1]);
    aligns.resize(ncols, Align::Left);

    const int W = terminalWidth();
    int budget = W - 2 * static_cast<int>(ncols - 1);
    if (budget < static_cast<int>(ncols)) budget = static_cast<int>(ncols);  // at least 1 col each

    // Render every cell, distinguishing image cells (a lone <img> for a local PNG that produced an
    // actual sixel) from text cells. An image cell holds raw sixel bytes that must never reach
    // displayWidth/reflow, so we record its column width and row height separately and carry the
    // bytes through layout untouched. A non-image result (ordinary text, or an <img> fallback to
    // its alt text/filename when graphics are unavailable) is treated as normal reflowable text.
    std::vector<std::vector<bool>> isImage(rows.size(), std::vector<bool>(ncols, false));
    std::vector<std::vector<int>> imgRows(rows.size(), std::vector<int>(ncols, 0));  // image height
    std::vector<std::vector<int>> imgCellW(rows.size(), std::vector<int>(ncols, 0));  // image width
    std::vector<std::vector<std::string>> imgText(rows.size(), std::vector<std::string>(ncols));
    std::vector<int> imgWidth(ncols, 0);  // forced width of an image column, if any
    // Batch-join (ADR 0003): submit every image-tag cell's conversion to the pool up front, then
    // wait on the whole batch before laying out the table — the column-width algorithm needs every
    // image's footprint, so the table can't be emitted until its own cells are measured, but the
    // conversions run in parallel with each other (and overlap any earlier blocks' deferred images
    // already queued). Per-cell results live in stable storage so each job writes its own slot
    // without racing.
    struct CellImg {
        std::string block;
        int iw = 0, ih = 0;
        bool ok = false;
    };
    std::vector<std::vector<CellImg>> cimg(rows.size(), std::vector<CellImg>(ncols));
    // future is move-only, so the row vectors can't be copy-filled; resize each instead.
    std::vector<std::vector<std::future<void>>> cfut(rows.size());
    for (auto& row : cfut) row.resize(ncols);
    std::string fileDir = gFileDir;
    for (size_t i = 0; i < rows.size(); ++i) {
        for (size_t j = 0; j < ncols; ++j) {
            std::string cellText = trim(rows[i][j]);
            std::map<std::string, std::string> probe;
            if (!parseImgTag(cellText, probe) && !parseMdImage(cellText, probe)) continue;
            imgText[i][j] = cellText;
            CellImg* r = &cimg[i][j];
            // Render at the full content budget so an explicit height is honoured at the image's
            // natural width; a column too narrow to hold it is handled by a re-render after layout.
            cfut[i][j] = imagePool().submit([cellText, budget, fileDir, r] {
                r->ok = renderImageBlock(cellText,
                                         budget,
                                         fileDir,
                                         r->block,
                                         r->iw,
                                         r->ih,
                                         /*forceWidthBound=*/false,
                                         kKittyIdDefer);
            });
        }
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        for (size_t j = 0; j < ncols; ++j) {
            if (cfut[i][j].valid()) cfut[i][j].get();  // join this cell's conversion
            const CellImg& r = cimg[i][j];
            if (r.ok && isImageBlock(r.block)) {
                // The Kitty id is left as timg's default (kKittyIdDefer); the writer renumbers
                // every image in document order when it drains this table's slot.
                rows[i][j] = r.block;
                isImage[i][j] = true;
                imgRows[i][j] = std::max(1, r.ih);
                imgCellW[i][j] = r.iw;
                imgWidth[j] = std::max(imgWidth[j], r.iw);
            } else if (r.ok && !r.block.empty() && r.ih == 1) {
                rows[i][j] = r.block;  // <img> fallback text (alt/filename): reflow as text
            } else {
                imgText[i][j].clear();  // not an image after all; drop the kept tag
                rows[i][j] = renderInline(rows[i][j]);
            }
        }
    }

    // Natural (unwrapped) width of each column = its widest rendered cell. Image cells contribute
    // their recorded width rather than displayWidth (which can't measure a sixel block). A text
    // cell may already hold its own line breaks (from a <br>), so measure its widest line, not the
    // whole string (which would wrongly sum the lines and count the '\n').
    std::vector<int> natural(ncols, 0);
    for (size_t i = 0; i < rows.size(); ++i)
        for (size_t j = 0; j < ncols; ++j) {
            if (isImage[i][j]) {
                natural[j] = std::max(natural[j], imgWidth[j]);
                continue;
            }
            for (auto& ln : splitOnNewlines(rows[i][j]))
                natural[j] = std::max(natural[j], displayWidth(ln));
        }

    // Decide a target width per column with max-min fair sharing of the content budget (which
    // already excludes the two-space separators). A column whose natural width is at most its fair
    // share keeps that natural width; the columns that want more split the leftover budget evenly.
    // We iterate because fixing the columns that fit raises the share available to the rest — so
    // e.g. one narrow column does not let the wide ones starve each other (the earlier
    // left-to-right greedy did).
    std::vector<int> target(ncols, 0);
    std::vector<bool> fixed(ncols, false);
    int remBudget = budget, remCols = static_cast<int>(ncols);
    for (bool changed = true; changed && remCols > 0;) {
        changed = false;
        int fair = remBudget / remCols;
        for (size_t j = 0; j < ncols; ++j) {
            if (fixed[j] || natural[j] > fair) continue;
            target[j] = natural[j];  // fits within its share: keep natural width
            fixed[j] = true;
            remBudget -= natural[j];
            --remCols;
            changed = true;
        }
    }
    if (remCols > 0) {  // over-share columns split the rest evenly
        int base = remBudget / remCols, extra = remBudget % remCols, k = 0;
        for (size_t j = 0; j < ncols; ++j)
            if (!fixed[j]) target[j] = base + (k++ < extra ? 1 : 0);  // remainder to earliest
    }

    // Apply the targets. A text column is reflowed to its target and may end narrower still; an
    // image column takes the smaller of its natural width and the target (it does not wrap), and is
    // re-rendered below if that is narrower than the image it already produced.
    std::vector<int> widths(ncols, 0);
    for (size_t j = 0; j < ncols; ++j) {
        if (imgWidth[j] > 0) {
            widths[j] = std::min(natural[j], target[j]);
        } else {
            int actual = 0;
            for (auto& r : rows) {
                r[j] = reflow(r[j], target[j]);
                for (auto& ln : splitOnNewlines(r[j])) actual = std::max(actual, displayWidth(ln));
            }
            widths[j] = actual < 1 ? (natural[j] == 0 ? 0 : 1) : actual;
        }
    }

    // An image whose painted width exceeds its allocated column (the table could not give it the
    // room its requested height implied) is re-rendered WIDTH-BOUND to exactly that column width,
    // so it never overflows into the next column or past the table. forceWidthBound is required
    // because the image's true (real-cell) footprint can exceed the column even when the
    // nominal-cell estimate does not, so renderImageBlock's own heuristic would otherwise keep it
    // height-bound and too wide. This is the only case that runs timg twice, and only when columns
    // are tight.
    std::vector<int> allocWidth = widths;  // the budgeted width; tightening must not exceed it
    // The forced width-bound re-renders are submitted as a follow-up batch and joined the same way
    // (ADR 0003), again with kKittyIdDefer + row-major id stamping for serial-identical output.
    std::vector<std::vector<CellImg>> rimg(rows.size(), std::vector<CellImg>(ncols));
    std::vector<std::vector<std::future<void>>> rfut(rows.size());
    for (auto& row : rfut) row.resize(ncols);
    for (size_t i = 0; i < rows.size(); ++i) {
        for (size_t j = 0; j < ncols; ++j) {
            if (!isImage[i][j] || allocWidth[j] >= imgCellW[i][j]) continue;
            std::string cellText = imgText[i][j];
            int allocW = allocWidth[j];
            CellImg* r = &rimg[i][j];
            rfut[i][j] = imagePool().submit([cellText, allocW, fileDir, r] {
                r->ok = renderImageBlock(cellText,
                                         allocW,
                                         fileDir,
                                         r->block,
                                         r->iw,
                                         r->ih,
                                         /*forceWidthBound=*/true,
                                         kKittyIdDefer);
            });
        }
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        for (size_t j = 0; j < ncols; ++j) {
            if (!rfut[i][j].valid()) continue;
            rfut[i][j].get();
            CellImg& r = rimg[i][j];
            if (r.ok && isImageBlock(r.block)) {
                rows[i][j] = r.block;  // id left deferred; writer renumbers in document order
                imgRows[i][j] = std::max(1, r.ih);
                imgCellW[i][j] = r.iw;
            }
        }
    }

    // Tighten each image column to the width its image actually paints (aspect-fit and rounding can
    // make it narrower than the allocated cap), so a column hugs its image and the gap between
    // columns stays the intended two spaces rather than padding out to the cap. Never go below the
    // column's widest TEXT cell (a header wider than the image would overflow and misalign later
    // columns) and never ABOVE the budgeted width (which would overflow the table — a re-render can
    // round a cell or two wide).
    for (size_t j = 0; j < ncols; ++j) {
        if (imgWidth[j] <= 0) continue;
        int w = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (isImage[i][j])
                w = std::max(w, imgCellW[i][j]);
            else
                for (auto& ln : splitOnNewlines(rows[i][j])) w = std::max(w, displayWidth(ln));
        }
        if (w > 0) widths[j] = std::min(w, allocWidth[j]);
    }

    // Re-pin each Kitty image's cell footprint (c=/r=) to the FINAL column width. The c=/r= baked
    // in during rendering reflects that render's availWidth cap (the full budget on the first
    // pass), not the width this column ends up with after fair-sharing and tightening — so without
    // this an image can carry a c= wider than its column and overflow into the next cell. We
    // rewrite c to the final width and scale r by the same ratio to keep the aspect ratio the
    // render already chose. (Sixel images carry no c=/r=; kittyRewriteFootprint no-ops on a
    // non-Kitty block, so the guard is just isImage.) imgRows/imgCellW are also corrected so the
    // band reserves the right number of rows.
    for (size_t j = 0; j < ncols; ++j) {
        if (imgWidth[j] <= 0) continue;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (!isImage[i][j] || !isKittyImage(rows[i][j]) || imgCellW[i][j] <= 0) continue;
            int newC = widths[j];
            int newR = std::max(1, (imgRows[i][j] * newC + imgCellW[i][j] / 2) / imgCellW[i][j]);
            rows[i][j] = kittyRewriteFootprint(rows[i][j], newC, newR);
            imgCellW[i][j] = newC;
            imgRows[i][j] = newR;
        }
    }

    int total = 0;
    for (int w : widths) total += w;
    total += 2 * static_cast<int>(ncols - 1);  // two-space column separators
    std::string hline = horizontalLine(total);

    if (std::getenv("MDCAT_DEBUG_TABLE")) {
        std::fprintf(stderr, "mdcat: table W=%d budget=%d total=%d widths=[", W, budget, total);
        for (size_t j = 0; j < ncols; ++j) std::fprintf(stderr, "%s%d", j ? "," : "", widths[j]);
        std::fprintf(stderr, "] imgW=[");
        for (size_t j = 0; j < ncols; ++j) std::fprintf(stderr, "%s%d", j ? "," : "", imgWidth[j]);
        std::fprintf(stderr, "]\n");
    }

    // The 1-based starting column of each cell, accounting for the two-space separators. Used to
    // position image sixels during the overlay pass.
    std::vector<int> colOrigin(ncols, 1);
    for (size_t j = 1; j < ncols; ++j) colOrigin[j] = colOrigin[j - 1] + widths[j - 1] + 2;

    // Render one table row. The row occupies a band as tall as the tallest cell — text line count
    // or image row height, whichever is greater. Text is laid out first, top-aligned, one line at a
    // time across the band; image cells contribute blank padding of their column width during this
    // pass (reserving their space). Then, in a second pass, each image's sixel is painted into its
    // reserved column with relative cursor moves: up to the band top, across to the cell's column,
    // replay (which leaves the cursor at column 1 just below the image), then back down to the band
    // bottom. This keeps every image inside its own cell and keeps row separators below the band.
    auto emitRow = [&](size_t rowIdx, bool bold) {
        const std::vector<std::string>& cells = rows[rowIdx];
        std::vector<std::vector<std::string>> cellLines(ncols);
        int bandH = 1;
        for (size_t j = 0; j < ncols; ++j) {
            if (isImage[rowIdx][j]) {
                bandH = std::max(bandH, imgRows[rowIdx][j]);
            } else {
                cellLines[j] = splitOnNewlines(cells[j]);
                bandH = std::max(bandH, static_cast<int>(cellLines[j].size()));
            }
        }
        // Text pass: bandH lines. Image cells reserve their column width with spaces.
        for (int k = 0; k < bandH; ++k) {
            std::string line;
            for (size_t j = 0; j < ncols; ++j) {
                if (j) line += "  ";
                if (isImage[rowIdx][j]) {
                    line.append(static_cast<size_t>(widths[j]), ' ');
                } else {
                    std::string piece = static_cast<size_t>(k) < cellLines[j].size()
                        ? cellLines[j][k]
                        : std::string();
                    if (bold && !piece.empty()) piece = kBoldOn + piece + kBoldOff;
                    line += padAligned(piece, widths[j], aligns[j]);
                }
            }
            out << line << '\n';
        }
        // Overlay pass: paint each image into its reserved space. The cursor starts at column 1 on
        // the row just below the band; every move is relative to that, so the band's earlier scroll
        // (if any) does not matter. Each sixel is bracketed by DECSC/DECRC (in replaySixel), so it
        // restores the cursor to the band-top position regardless of how the terminal advances
        // after a sixel — then we step deterministically back to the band bottom for the next
        // image.
        for (size_t j = 0; j < ncols; ++j) {
            if (!isImage[rowIdx][j]) continue;
            out << "\033[" << bandH << 'A';          // up to the band top
            out << "\033[" << colOrigin[j] << 'G';   // across to this cell's column
            replaySixel(cells[j], out);              // paint; cursor restored to band top
            out << "\033[" << bandH << 'B' << '\r';  // down to the band bottom, column 1
        }
    };

    for (size_t i = 0; i < rows.size(); ++i) {
        emitRow(i, /*bold=*/i == 0);
        if (i == 0)
            out << hline << '\n';  // header underline
        else if (i + 1 < rows.size())
            out << kLightGray << hline << kReset << '\n';  // light-gray row separator
    }
}

// ---------------------------------------------------------------------------
// Block parser / document driver
// ---------------------------------------------------------------------------

void render(const std::vector<std::string>& lines, std::ostream& out);

// Drop a single trailing '\n' if present. render() terminates every block with one, so the buffered
// output of a recursive render ends in a newline; removing it keeps splitOnNewlines from yielding a
// spurious empty final line (which would draw an extra bare quote rule).
std::string trimTrailingNewline(const std::string& s) {
    if (!s.empty() && s.back() == '\n') return s.substr(0, s.size() - 1);
    return s;
}

// True if `line` opens a block quote: up to three spaces of indentation, then a '>'. (GFM allows
// the block-quote marker to be indented by at most three spaces; four would be a code indent, which
// this renderer does not implement, but checking the bound keeps the detection honest.)
bool isBlockQuote(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ') ++i;
    return i < line.size() && line[i] == '>';
}

// Strip one block-quote marker from a line that isBlockQuote(): drop up to three leading spaces,
// the '>', and one optional space after it. The remainder is the line's content one level in.
std::string stripBlockQuote(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ') ++i;
    if (i < line.size() && line[i] == '>') ++i;  // the marker itself
    if (i < line.size() && line[i] == ' ') ++i;  // one optional space of padding
    return line.substr(i);
}

// A parsed list-item marker. `markerWidth` is the number of leading columns the marker plus its
// trailing spaces occupy on the first line — i.e. the column the item's content begins at, which is
// also the indent every continuation line and nested block must reach to stay in the item.
struct ListMarker {
    bool ordered = false;    // true for "1." / "1)", false for a "-"/"+"/"*" bullet
    char delim = 0;          // bullet char ('-','+','*') or ordered delimiter ('.'/')')
    int start = 0;           // ordered list's starting number (the digits before the delimiter)
    size_t markerWidth = 0;  // columns from line start to the item's content (the content indent)
};

// If `line` begins a list item, fill `m` and return true. A marker is up to three spaces of indent,
// then either a bullet (-, +, *) or an ordered marker (1-9 digits then '.' or ')'), then at least
// one space (or end of line). The trailing run of spaces is folded into markerWidth so the content
// indent is known. An empty item (marker then end of line) gets a content indent of marker + 1, the
// conventional single space. (We don't implement GFM's 4-space-content rule for over-indented item
// bodies; up to a normal run of spaces after the marker is consumed.)
bool parseListMarker(const std::string& line, ListMarker& m) {
    size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ') ++i;
    if (i >= line.size()) return false;
    size_t markerStart = i;
    if (line[i] == '-' || line[i] == '+' || line[i] == '*') {
        m.ordered = false;
        m.delim = line[i];
        ++i;
    } else if (std::isdigit(static_cast<unsigned char>(line[i]))) {
        size_t d = i;
        while (d < line.size() && std::isdigit(static_cast<unsigned char>(line[d]))) ++d;
        if (d - i > 9 || d >= line.size() || (line[d] != '.' && line[d] != ')')) return false;
        m.ordered = true;
        m.start = std::atoi(line.substr(i, d - i).c_str());
        m.delim = line[d];
        i = d + 1;
    } else {
        return false;
    }
    // The marker must be followed by a space (or be the whole line, an empty item).
    if (i < line.size() && line[i] != ' ') return false;
    (void)markerStart;
    size_t afterMarker = i;
    while (i < line.size() && line[i] == ' ') ++i;
    // An empty item, or one indented by many spaces, still uses a single space of content indent.
    if (i >= line.size() || i - afterMarker == 0)
        m.markerWidth = afterMarker + 1;
    else
        m.markerWidth = i;
    return true;
}

// True if `line` begins a list item of any kind.
bool isListItem(const std::string& line) {
    ListMarker m;
    return parseListMarker(line, m);
}

// Two list markers belong to the same list when they are the same type, and for bullets the same
// bullet character, and for ordered lists the same delimiter ('.'/')'). A change of any of these
// (or a blank line followed by a different type) starts a new list. (GFM also treats a change of
// bullet character as a new list; we follow that.)
bool sameListType(const ListMarker& a, const ListMarker& b) {
    if (a.ordered != b.ordered) return false;
    return a.delim == b.delim;
}

// True if the trimmed line is a valid ATX heading marker (1-6 '#' then space or end of line).
bool isAtxHeading(const std::string& t) {
    size_t h = 0;
    while (h < t.size() && t[h] == '#') ++h;
    return h >= 1 && h <= 6 && (h == t.size() || t[h] == ' ');
}

// True if the trimmed line opens or closes a fenced code block (>= 3 backticks or tildes).
bool isCodeFence(const std::string& t) {
    if (t.size() < 3 || (t[0] != '`' && t[0] != '~')) return false;
    size_t r = 0;
    while (r < t.size() && t[r] == t[0]) ++r;
    return r >= 3;
}

// True if `line` is indented enough to be an indented code line: at least four leading columns of
// indentation, counting a leading tab as reaching the four-column stop. A blank line does not count
// (it is handled separately as a possible interior blank). See GFM 4.4.
bool isIndentedCode(const std::string& line) {
    int cols = 0;
    for (char c : line) {
        if (c == ' ')
            ++cols;
        else if (c == '\t')
            cols += 4;
        else
            break;
        if (cols >= 4) return true;
    }
    return false;  // ran out of leading whitespace before reaching column four (or the line is
                   // blank)
}

// Remove the four-column indent that introduces an indented code block, leaving the rest verbatim.
// A leading tab counts as four columns and is consumed whole; otherwise up to four leading spaces
// are dropped. Content beyond the first four columns is preserved exactly.
std::string stripCodeIndent(const std::string& line) {
    size_t i = 0;
    int cols = 0;
    while (i < line.size() && cols < 4) {
        if (line[i] == ' ') {
            ++cols;
            ++i;
        } else if (line[i] == '\t') {
            cols += 4;
            ++i;
        } else
            break;
    }
    return line.substr(i);
}

// True if the trimmed line is a thematic break: >= 3 of the same -, * or _ (spaces allowed).
bool isThematicBreak(const std::string& t) {
    std::string compact;
    for (char c : t)
        if (c != ' ' && c != '\t') compact += c;
    if (compact.size() < 3) return false;
    return compact.find_first_not_of('-') == std::string::npos ||
        compact.find_first_not_of('*') == std::string::npos ||
        compact.find_first_not_of('_') == std::string::npos;
}

// True if line `j` begins a new leaf block, so a paragraph in progress must stop before it.
bool startsBlock(const std::vector<std::string>& lines, size_t j) {
    const std::string t = trim(lines[j]);
    if (t.empty()) return true;
    if (isAtxHeading(t)) return true;
    if (isCodeFence(t)) return true;
    if (isThematicBreak(t)) return true;
    if (isBlockQuote(lines[j])) return true;
    if (isListItem(lines[j])) return true;
    if (lines[j].find('|') != std::string::npos && j + 1 < lines.size() &&
        isTableDelimiterRow(lines[j + 1]))
        return true;  // a GFM table header row
    return false;
}

// Strip up to `n` leading spaces from `line` (fewer if the line has fewer). Used to remove a list
// item's content indent from its continuation lines so the item's body renders at column zero.
std::string stripIndent(const std::string& line, size_t n) {
    size_t i = 0;
    while (i < n && i < line.size() && line[i] == ' ') ++i;
    return line.substr(i);
}

// Count the leading spaces of `line` (its indentation in columns; tabs are not expanded — the
// inputs here use spaces, matching the rest of the parser).
size_t leadingSpaces(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    return i;
}

// One gathered list item: the content lines with the item's content indent already stripped, plus
// whether a blank line appeared anywhere inside it (which, like a blank line between items, makes
// the whole list loose — GFM 5.3).
struct ListItem {
    std::vector<std::string> lines;
    bool hadBlank = false;
};

// If `s` begins with a GFM task-list checkbox — "[ ]", "[x]" or "[X]" followed by a space (or end
// of the content) — set `checked` and strip the checkbox plus its trailing space, returning true.
// The checkbox must be the very start of the item's content (GFM task-list extension).
bool stripTaskCheckbox(std::string& s, bool& checked) {
    if (s.size() < 3 || s[0] != '[' || s[2] != ']') return false;
    char c = s[1];
    if (c == ' ')
        checked = false;
    else if (c == 'x' || c == 'X')
        checked = true;
    else
        return false;
    if (s.size() == 3) {
        s.clear();
        return true;
    }  // checkbox is the whole content
    if (s[3] != ' ') return false;  // must be followed by whitespace
    s.erase(0, 4);
    return true;
}

// Render a gathered list. Each item's content is a full block sequence rendered recursively (one
// container level deeper) into a buffer; the buffer's first line is prefixed with the item marker
// and the rest with equal-width padding so the body hangs under the marker. Bullets use a
// depth-varying glyph (● ○ ▪︎, matching GitHub's disc/circle/square); ordered items keep their
// number with a '.'. An item whose content opens with a task-list checkbox ("[ ]"/"[x]") swaps the
// marker for a ☐/☑ glyph (GFM task-list extension). gListDepth tracks bullet nesting for the glyph;
// gIndent is raised by the marker width so inner content reflows within the narrower column. A
// loose list prints a blank line between items.
void emitList(const std::vector<ListItem>& items,
              const ListMarker& marker,
              bool loose,
              std::ostream& out) {
    static const std::vector<std::string> kBullets = {"●", "○", "▪︎"};
    // Each list level indents its content by four columns (matching most Markdown viewers), so the
    // marker is padded out to at least four columns. A longer ordered marker (e.g. "10. ") keeps
    // its natural width.
    const int kListIndent = 4;
    // The outermost list block is set off from surrounding text by a two-space left lead; nested
    // lists already step in through their parent item's marker, so they add no further lead.
    const std::string lead = (gListNesting == 0) ? "  " : "";
    gIndent += static_cast<int>(lead.size());
    ++gListNesting;
    for (size_t idx = 0; idx < items.size(); ++idx) {
        if (idx && loose) out << '\n';

        // A task-list item: strip the leading checkbox from its content and remember its state so
        // the marker becomes a checkbox glyph below.
        std::vector<std::string> itemLines = items[idx].lines;
        bool isTask = false, checked = false;
        if (!itemLines.empty() && stripTaskCheckbox(itemLines[0], checked)) isTask = true;

        // Build the marker text and the matching blank padding for continuation lines.
        std::string mark;
        if (isTask) {
            mark = (checked ? "☑" : "☐") + std::string(" ");  // ☑ / ☐
        } else if (marker.ordered) {
            mark = std::to_string(marker.start + static_cast<int>(idx)) + marker.delim + ' ';
        } else {
            mark = kBullets[std::min<size_t>(gListDepth, kBullets.size() - 1)] + std::string(" ");
        }
        // Pad the marker out to the four-column indent so the body hangs at the indented column.
        if (displayWidth(mark) < kListIndent)
            mark += std::string(static_cast<size_t>(kListIndent - displayWidth(mark)), ' ');
        int markWidth = displayWidth(mark);
        std::string pad(static_cast<size_t>(markWidth), ' ');

        std::ostringstream buf;
        gIndent += markWidth;
        if (!marker.ordered) ++gListDepth;
        if (!loose) gTightItem = true;  // suppress lead-paragraph/sublist spacing in a tight item
        render(itemLines, buf);
        if (!marker.ordered) --gListDepth;
        gIndent -= markWidth;

        std::vector<std::string> body = splitOnNewlines(trimTrailingNewline(buf.str()));
        for (size_t k = 0; k < body.size(); ++k)
            out << lead << (k == 0 ? mark : pad) << body[k] << '\n';
    }
    --gListNesting;
    gIndent -= static_cast<int>(lead.size());
}

// Render the gathered lines of a block quote (markers already stripped) one container level deeper.
// The contents are a full sequence of blocks, so they go through render() recursively; the rendered
// output is then prefixed line by line with the quote rule. gIndent is raised by the rule's width
// while the contents render, so paragraphs reflow and rules fill within the narrower inner column.
void emitBlockQuote(const std::vector<std::string>& inner, std::ostream& out) {
    const int prefixWidth = 2;  // displayWidth(kQuoteBar): the bar glyph plus one space
    std::ostringstream buf;
    gIndent += prefixWidth;
    render(inner, buf);
    gIndent -= prefixWidth;
    // Prefix every line of the rendered contents with the quote rule. Blank separator lines between
    // inner blocks get the bar too, so the rule is continuous down the whole quote.
    for (const std::string& line : splitOnNewlines(trimTrailingNewline(buf.str())))
        out << kQuoteBar << line << '\n';
}

void render(const std::vector<std::string>& lines, std::ostream& out) {
    size_t n = lines.size();
    bool emitted = false;  // has any block been printed yet?
    size_t curLine = 0;    // source-line index of the block currently being emitted (for mark)
    // A tight list item suppresses the blank line between its own top-level blocks (its lead
    // paragraph and a nested list). Snapshot the flag and clear it immediately, so this applies
    // only to this render's top level — blocks rendered deeper, and recursive renders, separate
    // normally.
    bool tight = gTightItem;
    gTightItem = false;
    // Print a blank line before every block except the first, so blocks are visually separated
    // without a leading or trailing blank line in the output. A tight item skips that blank.
    // After the separator (so the cursor sits on the block's first row) emit the provenance
    // marker tagging that row with the block's markdown source line: gEmitProvenance gates it
    // to the pager path and gIndent == 0 restricts it to top-level blocks, whose curLine is a
    // real source line the pager can re-render from. See srcAtRow in gmore_emulator.cpp.
    auto separate = [&] {
        if (emitted && !tight) out << '\n';
        emitted = true;
        if (gEmitProvenance && gIndent == 0) out << "\033]9999;" << curLine << "\033\\";
    };
    for (size_t i = 0; i < n;) {
        const std::string& line = lines[i];
        std::string t = trim(line);

        if (t.empty()) {
            ++i;
            continue;
        }  // blank lines separate blocks
        curLine = i;  // the source line separate()'s provenance marker will tag
        if (gIndent == 0) gBlockLine = i + 1;

        // Stream incrementally: flush the output sink before starting the next block, so each block
        // (and any subprocess-rendered image it contains) reaches the terminal as soon as it is
        // ready rather than the whole document being withheld until the slowest image finishes. The
        // previous block is fully written by now and the cursor sits at a line boundary, so a flush
        // here never splits a Kitty APC chunk mid-stream. At the top level `out` is the SlotSink,
        // whose flush commits the finished block as an ordered slot for the writer thread; in
        // recursive renders it is a local ostringstream and the flush is a no-op. A possibly-empty
        // first flush is harmless.
        if (emitted) out.flush();

        // ATX heading: 1-6 '#' followed by a space (or end of line).
        if (isAtxHeading(t)) {
            separate();
            size_t h = 0;
            while (h < t.size() && t[h] == '#') ++h;
            std::string text = trim(t.substr(h));
            // A heading may have an optional closing run of '#'.
            size_t e = text.size();
            while (e > 0 && text[e - 1] == '#') --e;
            if (e < text.size() && (e == 0 || text[e - 1] == ' ')) text = trim(text.substr(0, e));
            emitHeading(static_cast<int>(h), text, out);
            ++i;
            continue;
        }

        // Fenced code block: a run of >= 3 backticks or tildes; closed by a matching fence.
        if (isCodeFence(t)) {
            separate();
            char fence = t[0];
            size_t run = 0;
            while (run < t.size() && t[run] == fence) ++run;
            std::string lang = trim(t.substr(run));  // info string, e.g. "cpp" or "python"
            std::vector<std::string> body;
            size_t j = i + 1;
            for (; j < n; ++j) {
                std::string tj = trim(lines[j]);
                size_t r = 0;
                while (r < tj.size() && tj[r] == fence) ++r;
                if (r >= run && tj.find_first_not_of(fence) == std::string::npos)
                    break;  // closing fence
                body.push_back(lines[j]);
            }
            // A ```mermaid block renders per gMermaidMode: the built-in renderer, then a diagram
            // via the mermaid CLI when it (and a graphics terminal) are available, then ordinary
            // code.
            std::string mlang = lang;
            for (char& c : mlang)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            int mAvailW = terminalWidth();
            if (mlang == "mermaid" && gMermaidMode == MermaidMode::Source) {
                emitCodeBlock(body, lang, out);
            } else if (mlang == "mermaid") {
                if (gMermaidMode != MermaidMode::Mmdc) {
                    std::string why;
                    bool tooComplex = false;
                    if (emitBuiltinMermaid(body, out, why, tooComplex)) {
                        i = (j < n) ? j + 1 : j;  // skip the closing fence if present
                        continue;
                    }
                    // Inside a list or quote the lines are re-based, so the location is that of
                    // the enclosing top-level block.
                    if (std::getenv("MDCAT_MERMAID_DEBUG"))
                        std::cerr << "mdcat: " << (gSourceName.empty() ? "<stdin>" : gSourceName)
                                  << ":" << gBlockLine << ": mermaid: fallback: " << why << "\n";
                    if (gMermaidMode == MermaidMode::Builtin || tooComplex) {
                        emitCodeBlock(body, lang, out);
                        i = (j < n) ? j + 1 : j;
                        continue;
                    }
                }
                // The conversion produces the final bytes: the framed diagram on success, else the
                // ordinary code block (the fallback). Computing both in the worker lets the whole
                // block be a single deferred slot (ADR 0003). emitCodeBlock is pure here (top-level
                // gIndent==0, terminalWidth memoized), so it is safe to run off-thread into a local
                // buffer.
                auto convert = [body, lang, mAvailW](uint32_t kid) {
                    std::string mimg;
                    int miw = 0, mih = 0;
                    if (renderMermaidBlock(body, mAvailW, mimg, miw, mih, kid) &&
                        isImageBlock(mimg))
                        return imageParagraphBytes(mimg, mih);
                    std::ostringstream cb;
                    emitCodeBlock(body, lang, cb);
                    return cb.str();
                };
                if (SlotSink* sink = asSlotSink(out)) {
                    // Defer the Kitty id (kKittyIdDefer): the writer assigns a document-order id
                    // when it drains the slot, so out-of-order completion stays serial-identical.
                    slotDeferImage(
                        sink, imagePool().submit([convert] { return convert(kKittyIdDefer); }));
                } else {
                    out << imagePool().submit([convert] { return convert(0); }).get();
                }
            } else {
                emitCodeBlock(body, lang, out);
            }
            i = (j < n) ? j + 1 : j;  // skip the closing fence if present
            continue;
        }

        // Thematic break: a line of three or more -, * or _ (optionally spaced).
        if (isThematicBreak(t)) {
            separate();
            emitThematicBreak(out);
            ++i;
            continue;
        }

        // Block quote: one or more lines starting with '>'. Gather the run, including lazy
        // continuation lines — a non-blank line without its own '>' that continues a paragraph
        // inside the quote (GFM 5.1). A blank line, or an unquoted line that itself starts a new
        // block, ends the quote. The markers are stripped and the contents rendered recursively.
        if (isBlockQuote(line)) {
            separate();
            std::vector<std::string> inner;
            size_t j = i;
            for (; j < n; ++j) {
                if (isBlockQuote(lines[j])) {
                    inner.push_back(stripBlockQuote(lines[j]));
                } else if (!trim(lines[j]).empty() && !startsBlock(lines, j)) {
                    inner.push_back(lines[j]);  // lazy paragraph continuation
                } else {
                    break;
                }
            }
            emitBlockQuote(inner, out);
            i = j;
            continue;
        }

        // List: a run of items sharing a marker type (GFM 5.3). Reached only after the
        // thematic-break check above, so a line like "- - -" or "***" is a rule, not a one-item
        // list. Each item is gathered as the content lines belonging to it (its content indent
        // stripped); a line at the list's own indent that opens a same-type marker starts the next
        // item; one that opens a different-type marker, a non-indented block start, or follows a
        // blank line at a lower indent ends the list. The list is loose if any blank line falls
        // between items or inside an item.
        if (isListItem(line)) {
            separate();
            ListMarker listMarker;
            parseListMarker(line, listMarker);  // defines the list's type for the whole run

            std::vector<ListItem> items;
            bool loose = false;
            size_t j = i;
            while (j < n) {
                ListMarker m;
                // A new item: a marker line at this list's indent level and matching type.
                if (parseListMarker(lines[j], m) &&
                    leadingSpaces(lines[j]) < listMarker.markerWidth) {
                    if (!sameListType(m, listMarker))
                        break;  // different marker type: a separate list
                    ListItem item;
                    // The first line keeps everything after the marker (drop the marker itself, not
                    // just leading spaces — there are none before it at this point).
                    item.lines.push_back(lines[j].substr(std::min(m.markerWidth, lines[j].size())));
                    size_t k = j + 1;
                    bool pendingBlank =
                        false;  // a blank line seen but not yet committed to the item
                    for (; k < n; ++k) {
                        if (trim(lines[k]).empty()) {
                            pendingBlank = true;
                            continue;
                        }
                        size_t ind = leadingSpaces(lines[k]);
                        if (ind >= m.markerWidth) {
                            // Indented to (or past) the content column: part of this item's body. A
                            // committed blank line before it is preserved and makes the list loose.
                            if (pendingBlank) {
                                item.hadBlank = true;
                                item.lines.push_back("");
                                pendingBlank = false;
                            }
                            item.lines.push_back(stripIndent(lines[k], m.markerWidth));
                        } else if (!pendingBlank && !startsBlock(lines, k)) {
                            // Lazy continuation: an unindented paragraph line directly following
                            // the item's text (no blank between) still belongs to its paragraph.
                            item.lines.push_back(lines[k]);
                        } else {
                            break;  // unindented block start, marker, or post-blank line: item
                                    // ends.
                        }
                    }
                    if (item.hadBlank) loose = true;
                    // A blank line separating this item from a following same-type item at the
                    // list's own indent makes the whole list loose (GFM 5.3).
                    if (pendingBlank && k < n) {
                        ListMarker next;
                        if (parseListMarker(lines[k], next) &&
                            leadingSpaces(lines[k]) < listMarker.markerWidth &&
                            sameListType(next, listMarker))
                            loose = true;
                    }
                    items.push_back(std::move(item));
                    j = k;
                } else {
                    break;
                }
            }
            emitList(items, listMarker, loose, out);
            i = j;
            continue;
        }

        // GFM table: a header row whose next line is a delimiter row. Gather contiguous rows.
        if (line.find('|') != std::string::npos && i + 1 < n && isTableDelimiterRow(lines[i + 1])) {
            separate();
            std::vector<std::string> rows;
            rows.push_back(line);          // header
            rows.push_back(lines[i + 1]);  // delimiter
            size_t j = i + 2;
            for (; j < n; ++j) {
                if (trim(lines[j]).empty() || lines[j].find('|') == std::string::npos) break;
                rows.push_back(lines[j]);
            }
            emitTable(rows, out);
            i = j;
            continue;
        }

        // Indented code block: lines indented four or more columns, rendered verbatim (GFM 4.4).
        // Reached only at a fresh block position — an indented line right after a paragraph line is
        // gathered as that paragraph's lazy continuation instead, because isIndentedCode is
        // deliberately NOT part of startsBlock(), so a code block cannot interrupt a paragraph.
        // Interior blank lines are kept; trailing blank lines are dropped. The four-column indent
        // is stripped from each line and the rest emitted exactly, reusing the fenced-code panel
        // style.
        if (isIndentedCode(line)) {
            separate();
            std::vector<std::string> body;
            size_t j = i;
            for (; j < n; ++j) {
                if (trim(lines[j]).empty()) {
                    body.push_back(std::string());
                    continue;
                }
                if (!isIndentedCode(lines[j])) break;
                body.push_back(stripCodeIndent(lines[j]));
            }
            while (!body.empty() && body.back().empty()) body.pop_back();  // drop trailing blanks
            emitCodeBlock(body, "", out);
            // Resume right after the lines that became code; any trailing blank lines we gathered
            // but popped are left for the main loop, which treats blank lines as block separators.
            i += body.size();
            continue;
        }

        // Paragraph: line i is not a block start, so consume it and any following lines until a
        // blank line or the start of another block. The same startsBlock() predicate that the
        // dispatcher relies on is reused here, so the two can never disagree (which previously
        // produced an empty paragraph and an infinite loop on lines like "#620](...)").
        separate();
        std::vector<std::string> para;
        size_t j = i;
        do {
            para.push_back(lines[j]);
            ++j;
        } while (j < n && !startsBlock(lines, j));
        emitParagraph(para, out);
        i = j;
    }
}

std::vector<std::string> splitLines(std::istream& in) {
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // tolerate CRLF
        lines.push_back(line);
    }
    return lines;
}

// An ordered slot list with a dedicated writer thread (ADR 0003), used as the non-tty output sink.
// render() writes text into it exactly like an ostream; at each block boundary (sync()) the
// accumulated text is committed as one ordered slot. A deferred image instead reserves a *future*
// slot (deferImage) and returns immediately, so the render thread keeps converting later images
// while this one is still in flight.
//
// A single block's bytes still reach the wire contiguously — a text slot is written in one
// writeAll, and an image's framed bytes arrive as one slot — so a Kitty APC chunk is never split by
// a buffer-boundary write (a consumer that sees a chunk arrive in pieces with a gap can give up and
// dump the partial base64 as text).
//
// The writer thread walks slots strictly front-to-back: for a ready slot it writes the bytes; for a
// future slot it blocks on the future, then writes. Because the writer is its own thread, a stalled
// write() blocks only the writer — image workers keep filling later future slots. Output emerges in
// document order (the writer never reorders), so the bytes are identical to the serial renderer's
// output; only timing changes.
//
// All Kitty image ids are assigned HERE, on the writer thread, in slot (= document) order: workers
// and tables render with kKittyIdDefer (leaving timg's default id), and writerLoop runs
// kittyRenumberAll on every slot's bytes. That is the one place ids are minted, so parallel,
// out-of-order conversion still yields the exact id sequence the single-threaded serial renderer
// produced. (Text slots contain no APC, so renumbering them is a cheap no-op.)
class SlotSink : public std::streambuf {
public:
    // maxPending bounds the lookahead window: the producer (render thread) blocks before adding a
    // slot once this many slots are queued-but-unwritten, so a near-bottom giant image can't make
    // workers buffer the whole rest of the document in RAM ahead of a stalled writer (ADR 0003
    // backpressure). 2*N (pool size) keeps every worker busy without unbounded buffering.
    explicit SlotSink(int fd)
        : fd_(fd),
          maxPending_(2 * std::max(1u, std::thread::hardware_concurrency())),
          writer_([this] { writerLoop(); }) {}
    // Buffer mode (fd_ == -1): the writer appends each drained slot to buffer_ instead of
    // write()ing to a descriptor. The pager path (ADR 0003 step 6) uses this to get parallel image
    // conversion via the deferred-slot machinery while still assembling the whole document into one
    // buffer for gmore. No backpressure cap is needed — there is no stalling consumer, the buffer
    // is the consumer — so the window is left unbounded so the producer never blocks.
    SlotSink()
        : fd_(-1),
          maxPending_(std::numeric_limits<size_t>::max()),
          writer_([this] { writerLoop(); }) {}
    ~SlotSink() override { finish(); }

    // Buffer-mode result: the fully assembled, Kitty-renumbered document bytes. Call after
    // finish().
    std::string takeBuffer() { return std::move(buffer_); }

    // Commit any buffered text, then append `fut` as the next ordered slot. Returns without waiting
    // on the conversion, but may block briefly for backpressure if the lookahead window is full.
    void deferImage(std::future<std::string> fut) {
        commitText();
        appendSlot(Slot(std::move(fut)));
    }

    // Flush any trailing text and join the writer thread. Idempotent.
    void finish() {
        if (done_) return;
        commitText();
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
        }
        cv_.notify_one();
        if (writer_.joinable()) writer_.join();
        done_ = true;
    }

protected:
    int_type overflow(int_type ch) override {
        if (ch != traits_type::eof()) text_.push_back(static_cast<char>(ch));
        return ch;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        text_.append(s, static_cast<size_t>(n));
        return n;
    }
    // render() flushes at every top-level block boundary; commit the accumulated text as one slot
    // so it is ordered ahead of whatever the next block produces (text or a deferred image).
    int sync() override {
        commitText();
        return 0;
    }

private:
    struct Slot {
        bool ready;
        std::string bytes;             // valid when ready
        std::future<std::string> fut;  // valid when !ready
        explicit Slot(std::string b) : ready(true), bytes(std::move(b)) {}
        explicit Slot(std::future<std::string> f) : ready(false), fut(std::move(f)) {}
    };

    void commitText() {
        if (text_.empty()) return;
        appendSlot(Slot(std::move(text_)));
        text_.clear();
    }

    // Append a slot, blocking first if the unwritten-slot window is full so the producer can't race
    // far ahead of the writer (ADR 0003 backpressure). The writer signals drained_ each time it
    // pops a slot.
    void appendSlot(Slot&& s) {
        std::unique_lock<std::mutex> lk(mu_);
        drained_.wait(lk, [this] { return slots_.size() < maxPending_; });
        slots_.emplace_back(std::move(s));
        lk.unlock();
        cv_.notify_one();
    }

    void writeAll(const std::string& b) {
        size_t off = 0;
        while (off < b.size()) {
            ssize_t n = write(fd_, b.data() + off, b.size() - off);
            if (n <= 0) {
                if (errno == EINTR) continue;
                return;
            }
            off += static_cast<size_t>(n);
        }
    }

    void writerLoop() {
        for (;;) {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return closed_ || !slots_.empty(); });
            if (slots_.empty()) {
                if (closed_) return;
                continue;
            }
            // Move the front slot out; release the lock before any blocking write/get so the
            // producer can keep appending and image workers can keep filling later future slots.
            Slot front = std::move(slots_.front());
            slots_.pop_front();
            lk.unlock();
            drained_.notify_one();  // a slot freed: a producer blocked on backpressure may proceed
            // Assign Kitty ids in slot order (document order): every image in this slot's bytes
            // gets the next id. Text slots have no APC, so this is a no-op for them.
            std::string bytes = front.ready ? std::move(front.bytes) : front.fut.get();
            std::string renumbered = kittyRenumberAll(bytes);
            // Force every Kitty APC to q=2 (suppress all terminal replies). mdcat never reads
            // those replies, so if we let them through they pile up in the pty input buffer and
            // spill onto the shell prompt as garbage after mdcat exits — most visibly over SSH,
            // where the reply travels back to the remote shell. Applies to pipe, file, and
            // buffer (pager) output alike. iTerm2 only honours q=2, not q=1.
            renumbered = kittyQ2(renumbered);
            if (fd_ < 0)
                buffer_ += renumbered;  // buffer mode: assemble in RAM for the pager
            else
                writeAll(renumbered);
        }
    }

    int fd_;              // output descriptor, or -1 in buffer mode
    std::string buffer_;  // buffer mode (fd_ < 0): assembled document, read via takeBuffer()
    size_t maxPending_;   // backpressure cap on unwritten slots
    std::string text_;    // current (render-thread) text buffer, pre-commit
    std::deque<Slot> slots_;
    std::mutex mu_;
    std::condition_variable cv_;       // writer waits for work
    std::condition_variable drained_;  // producer waits for room (backpressure)
    bool closed_ = false;
    bool done_ = false;
    std::thread writer_;
};

SlotSink* asSlotSink(std::ostream& out) {
    return dynamic_cast<SlotSink*>(out.rdbuf());
}
void slotDeferImage(SlotSink* sink, std::future<std::string> fut) {
    sink->deferImage(std::move(fut));
}

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
