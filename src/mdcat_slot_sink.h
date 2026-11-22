// SlotSink: the non-tty output sink, an ordered slot list with a dedicated writer thread (ADR
// 0003).
#pragma once

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <future>
#include <iosfwd>
#include <limits>
#include <mutex>
#include <streambuf>
#include <string>
#include <thread>
#include <utility>

namespace mdcat {

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

    void finish();

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

    void appendSlot(Slot&& s);

    void writeAll(const std::string& b);

    void writerLoop();

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

// If `out` is backed by a SlotSink, deferred image slots can be reserved on it (ADR 0003);
// otherwise (tty buffer, or a recursive ostringstream) images render synchronously inline.
// Thin free-function wrappers so call sites can detect the sink and reserve a slot.
SlotSink* asSlotSink(std::ostream& out);
void slotDeferImage(SlotSink* sink, std::future<std::string> fut);

}  // namespace mdcat
