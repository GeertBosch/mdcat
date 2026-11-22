// SlotSink out-of-line method bodies (see mdcat_slot_sink.h).
#include "mdcat_slot_sink.h"

#include <unistd.h>

#include <cerrno>
#include <ostream>
#include <utility>

#include "mdcat_protocol.h"

namespace mdcat {

// Flush any trailing text and join the writer thread. Idempotent.
void SlotSink::finish() {
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

// Append a slot, blocking first if the unwritten-slot window is full so the producer can't race
// far ahead of the writer (ADR 0003 backpressure). The writer signals drained_ each time it
// pops a slot.
void SlotSink::appendSlot(Slot&& s) {
    std::unique_lock<std::mutex> lk(mu_);
    drained_.wait(lk, [this] { return slots_.size() < maxPending_; });
    slots_.emplace_back(std::move(s));
    lk.unlock();
    cv_.notify_one();
}

void SlotSink::writeAll(const std::string& b) {
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

void SlotSink::writerLoop() {
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

SlotSink* asSlotSink(std::ostream& out) {
    return dynamic_cast<SlotSink*>(out.rdbuf());
}
void slotDeferImage(SlotSink* sink, std::future<std::string> fut) {
    sink->deferImage(std::move(fut));
}

}  // namespace mdcat
