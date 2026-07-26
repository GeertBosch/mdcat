#pragma once

#include <csignal>
#include <cstdlib>
#include <termios.h>

// Process-wide guard against leaving the controlling terminal without echo/canonical
// mode after a signal. Every place that flips a tty into raw/no-echo mode — the brief
// startup capability probes (cell size, background colour, graphics support) and
// gmore's long-lived interactive pager session — registers its fd and prior termios
// here via enter()/leave(). If SIGINT/SIGTERM/SIGQUIT/SIGHUP arrives while a raw
// section is active (e.g. mid-probe, waiting on a reply that never comes), the
// handler restores the saved settings before re-raising, so the shell gets its echo
// back instead of being left in whatever raw state the probe or pager set.
//
// Header-only + inline variables (C++17) so no new translation unit is needed.
namespace ttyguard {

inline int gFd = -1;
inline struct termios gSaved{};
inline volatile sig_atomic_t gActive = 0;
inline bool gInstalled = false;

inline void restore() {
    if (gActive) {
        tcsetattr(gFd, TCSANOW, &gSaved);
        gActive = 0;
    }
}

inline void onSignal(int sig) {
    restore();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

inline void install() {
    if (gInstalled) return;
    gInstalled = true;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGQUIT, onSignal);
    std::signal(SIGHUP, onSignal);
    std::atexit(restore);
}

// Call once tcgetattr(fd, &saved) has succeeded, right before tcsetattr() puts the
// tty into raw/no-echo mode: records (fd, saved) as what a signal handler must
// restore while that mode is active.
inline void enter(int fd, const struct termios& saved) {
    install();
    gFd = fd;
    gSaved = saved;
    gActive = 1;
}

// Call right after tcsetattr() has restored the saved settings normally.
inline void leave() {
    gActive = 0;
}

}  // namespace ttyguard
