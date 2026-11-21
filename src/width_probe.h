// Choosing how to measure text (uw::Mode and its parameters, unicode_width.h) for the terminal in
// front of us, once at startup. Shared by mdcat and standalone gmore; mdcat's built-in pager
// inherits mdcat's choice.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <termios.h>
#include <unistd.h>

#include "tty_guard.h"
#include "unicode_width.h"

namespace uw {

// The column (1-based) from a cursor position report ESC [ row ; col R, or 0 if none arrived.
inline int readCursorColumn(int fd) {
    std::string reply;
    char c;
    while (reply.size() < 32) {
        if (read(fd, &c, 1) <= 0) break;  // timeout
        reply += c;
        if (c == 'R') break;
    }
    size_t semi = reply.rfind(';');
    if (semi == std::string::npos || reply.back() != 'R') return 0;
    return std::atoi(reply.c_str() + semi + 1);
}

// The columns this terminal advances for each sample, in one exchange over the controlling tty
// with echo off (like mdcat's other startup queries): a cursor position report before and after
// each sample, then the samples are erased. A width is -1 when the terminal did not answer.
struct ProbedWidths {
    int zwjSequence = -1;  // woman ZWJ rocket: 2 grapheme clusters, 4 or 5 per code point
    int zwj = -1;          // "a" ZWJ, minus the "a"
    int enclosing = -1;    // "1" U+20E3 (combining enclosing keycap), minus the "1"
};

inline ProbedWidths probeWidths() {
    ProbedWidths out;
    int fd = open("/dev/tty", O_RDWR | O_NOCTTY);
    if (fd < 0) return out;
    struct termios saved = {};
    if (tcgetattr(fd, &saved) != 0) {
        close(fd);
        return out;
    }
    struct termios raw = saved;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 2;  // 0.2 s between bytes before giving up
    tcsetattr(fd, TCSANOW, &raw);
    ttyguard::enter(fd, saved);
    auto send = [fd](const std::string& s) {
        return write(fd, s.data(), s.size()) == static_cast<ssize_t>(s.size());
    };
    static const char* const kSamples[] = {
        "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x9A\x80",  // U+1F469 U+200D U+1F680
        "a\xE2\x80\x8D",                                 // a U+200D
        "1\xE2\x83\xA3",                                 // 1 U+20E3
    };
    int* const widths[] = {&out.zwjSequence, &out.zwj, &out.enclosing};
    const int bases[] = {0, 1, 1};  // the leading ASCII character's own column
    int start = 0;
    if (send("\033[6n")) start = readCursorColumn(fd);
    int col = start;
    for (int i = 0; i < 3 && col > 0; ++i) {
        if (!send(std::string(kSamples[i]) + "\033[6n")) break;
        int next = readCursorColumn(fd);
        if (next <= col) break;  // no reply, or the line wrapped: give up
        *widths[i] = next - col - bases[i];
        col = next;
    }
    if (start > 0) send("\033[" + std::to_string(start) + "G\033[K");  // erase the samples
    tcsetattr(fd, TCSANOW, &saved);
    ttyguard::leave();
    close(fd);
    return out;
}

// Parse MDCAT_WIDTH: "grapheme", or "codepoint" optionally followed by ",zwj=N" and
// ",enclosing=N" (for example "codepoint,zwj=1,enclosing=1", as Apple Terminal measures).
inline void parseWidthSetting(const char* v) {
    gMode = std::strncmp(v, "codepoint", 9) == 0 ? Mode::CodePoint : Mode::Grapheme;
    if (const char* z = std::strstr(v, "zwj=")) gZwjWidth = std::atoi(z + 4);
    if (const char* e = std::strstr(v, "enclosing=")) gEnclosingWidth = std::atoi(e + 10);
}

// Set uw::gMode and its parameters. In order: $MDCAT_WIDTH; when `onTerminal` (stdout is the
// terminal the text is drawn on), the terminal's own answers to probeWidths; if it gave none, the
// values measured on VSCode and Apple Terminal by TERM_PROGRAM; else Mode::Grapheme. Off a
// terminal the default stays, so piped output and the tests do not depend on the terminal they
// happen to run in.
inline void initMode(bool onTerminal) {
    if (const char* v = std::getenv("MDCAT_WIDTH")) {
        parseWidthSetting(v);
        return;
    }
    if (!onTerminal) return;
    ProbedWidths p = probeWidths();
    if (p.zwjSequence == 2) return;  // one character per grapheme cluster
    if (p.zwjSequence > 2) {
        gMode = Mode::CodePoint;
        gZwjWidth = p.zwj == 1 ? 1 : 0;
        gEnclosingWidth = p.enclosing == 1 ? 1 : 0;
        return;
    }
    const char* tp = std::getenv("TERM_PROGRAM");
    if (tp && std::strcmp(tp, "vscode") == 0) parseWidthSetting("codepoint");
    if (tp && std::strcmp(tp, "Apple_Terminal") == 0)
        parseWidthSetting("codepoint,zwj=1,enclosing=1");
}

}  // namespace uw
