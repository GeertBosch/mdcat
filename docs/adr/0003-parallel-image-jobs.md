# ADR 0003 — Parallel image conversion jobs

## Status

Implemented (steps 1–6 complete).

## Context

`render()` is single-threaded and shells out to `timg`/`mmdc` **synchronously** at
four call sites (standalone `<img>` paragraph, mermaid block, single-cell table
image, multi-image table). Each subprocess (≈60–90 ms for `timg`, seconds for
`mmdc`/headless-Chrome) blocks the render loop, and the per-block flush
([[mdcat-output-streaming]]) is also synchronous.

Two problems follow:

1. **Serial images.** Total wall-clock ≈ the *sum* of every image's subprocess
   time, even though they are independent.
2. **A stalled write halts rendering.** `StreamingSink::sync()` calls `write()` in
   a retry loop; if the consumer stops reading, the pipe fills and `write()`
   blocks. Because rendering and output share the one thread, a slow reader freezes
   image conversion too — even images that could have been computed while we wait.

Goal: queue all image conversions, run up to **N = hardware concurrency** at once,
and let conversion proceed in parallel with output even when the writer stalls —
while keeping output **byte-identical** and in document order.

## Decision

Introduce a small **thread pool** and split rendering into *enqueue* and *assemble*
phases, with a dedicated **writer thread** so a stalled `write()` never blocks
conversion.

### 1. Job model

A `ImageJob` captures everything a worker needs to produce one image's bytes with
no shared mutable state:

```
struct ImageJob {
    enum Kind { Img, Mermaid } kind;
    std::string spec;        // <img> tag text, or mermaid source
    int availWidth;
    bool forceWidthBound;
    std::string fileDir;     // snapshot of gFileDir (workers must not read globals)
    // result, filled by the worker:
    std::string out;         // sixel / Kitty APC, or text fallback
    int cellW = 0, cellH = 0;
    bool ok = false;
};
```

`renderImageBlock`/`renderMermaidBlock` are refactored to be **pure** w.r.t. process
globals: every global they read today (`gFileDir`, the graphics backend, terminal
width, the per-run Kitty id counter) is passed in or made thread-safe. The Kitty id
counter (`kittyRewriteId`) becomes an atomic.

### 2. Thread pool

A fixed pool of `N = std::max(1u, std::thread::hardware_concurrency())` workers
drains a job queue (mutex + condvar). `submit(job)` returns a `std::future`-like
handle (or we use `std::packaged_task`/`std::future` directly). Workers only run
`timg`/`mmdc` and fill the result struct — no I/O ordering concerns.

### 3. Two enqueue patterns, by data dependency

The four call sites differ in whether downstream layout needs the image's
**dimensions**:

- **Deferred (standalone `<img>` paragraph, mermaid):** nothing downstream needs
  `cellW/cellH`. `render()` submits the job and emits an **ordered placeholder
  slot** into the output sequence, then continues immediately. Resolved at assemble
  time.
- **Join-required (table images):** the column-width algorithm needs every image's
  footprint *before* it can lay out the row, and tight columns trigger a second
  width-bound render (mdcat.cpp:2282). So a table **submits all its cell images as a
  batch, then waits on that batch** before running layout. The table still overlaps
  with *other* blocks' images (they were already queued) — we just can't emit the
  table until its own cells are measured. The forced second render is submitted as a
  follow-up job and joined the same way.

### 4. Ordered output + writer thread

Replace "render writes straight to a stream" with an **ordered slot list**. Each
top-level block reserves a slot (or several) in document order:

- text blocks → a slot holding the finished bytes immediately;
- deferred images → a slot holding a `future`.

A single **writer thread** walks the slots front-to-back: for each slot it blocks
on the future (if any), then `write()`s the bytes to stdout with the existing
retry loop. Because the writer is its own thread, a stalled `write()` blocks only
the writer — workers keep converting queued images and filling later slots. Output
still emerges strictly in document order (the writer never reorders), so the bytes
are identical to today's serial output; only timing changes.

The **tty/pager path is unchanged**: it still renders the whole document into one
buffer for gmore. (Pager benefits from parallel *conversion* via the same pool, but
keeps the single-buffer assemble — no writer thread, no per-block streaming.) So
the pool is shared; only the non-tty path gets the writer thread.

### 5. Backpressure / memory

A near-bottom-of-document giant image shouldn't let workers buffer the whole rest
of the document in RAM ahead of a stalled writer. Bound the number of *completed
but unwritten* slots (e.g. `2N`); workers block on submit when the lookahead window
is full. This caps memory and naturally throttles to the writer's pace without
coupling them on the critical path.

## Consequences

- Wall-clock for a multi-image document drops from Σ(image times) to ≈ the slowest
  image (plus serial overhead), bounded by N CPUs.
- A slow/stalled reader no longer freezes conversion; images are ready the instant
  the reader catches up.
- New dependency on `<thread>`/`<mutex>`/`<future>` and `-pthread` in the Makefile.
- `renderImageBlock`/`renderMermaidBlock` must be made global-free and
  thread-safe; this is the bulk of the risk (shared `timg` temp-file names, the
  Kitty id counter, any static buffers).
- Output remains byte-identical and ordered — regression-checkable with `cmp`
  against the current binary, exactly as the streaming change was.

## Implementation steps

1. **Make image rendering thread-safe & global-free** (no behavior change): thread
   `gFileDir`/backend/width through parameters; make the Kitty id counter atomic;
   audit `runMmdc` temp paths for uniqueness across threads. Verify `cmp`-identical.
2. **Add the thread pool** (`<thread>` + queue + `-pthread`); leave call sites
   serial but routed through `submit().get()`. Verify identical output, no speedup
   yet.
3. **Ordered slot list + writer thread** for the non-tty path; convert standalone
   `<img>` and mermaid to deferred slots. Verify identical bytes + first-byte and
   total-time improvements; confirm a stalled reader (slow `head`/`pv -L`) does not
   freeze conversion.
4. **Table batch-join**: submit a table's cell images together, join before layout,
   submit the width-bound re-render as a follow-up. Verify table docs `cmp`-identical.
5. **Backpressure window** (bound unwritten slots) + memory check on a big-image doc.
6. **Pager path**: share the pool for parallel conversion while keeping the single
   buffer. Re-run `make check`. *(Done: the pager renders into a buffer-mode
   `SlotSink` — `fd_ == -1`, unbounded window — so standalone `<img>` and mermaid
   defer onto the pool and convert in parallel, and the writer thread concatenates
   each drained, Kitty-renumbered slot into the buffer handed to `gmore::run`. This
   replaces the old serial `submit().get()` + final `kittyRenumberAll(buf.str())`
   pass; ids are now minted once, in document order, matching the streaming path —
   the only byte difference from the old pager output is the Kitty id starting
   offset, which the old path inflated by double-minting.)*

Each step keeps output byte-identical and is independently committable.
