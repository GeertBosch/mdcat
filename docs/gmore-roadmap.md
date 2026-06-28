# gmore — missing `more(1)`/`less(1)` features, ranked

`gmore` is a graphics-aware pager: it emulates a terminal subset into a cell grid
so it can page any text+graphics stream in either **sixel** or the **Kitty**
graphics protocol (see ADR 0002). Both work locally; Kitty is also what carries
images over SSH, where sixel cannot size itself. The paging *mechanics* (scroll, image
re-encode/re-place, full-repaint) work well, and the core motion vocabulary is now
in place. What it still lacks is most of the interactive **content** surface —
above all, search.

This document inventories the `more(1)`/`less(1)` feature set, marks what gmore
already does, and ranks the remaining gaps by value-to-effort.

## What gmore has today

- Forward paging: `space`, `f` (one screen; `N` + key scrolls N lines)
- Backward paging: `b` (one screen)
- Line stepping: `Enter` / `j` (down), `k` / `y` (up)
- Half-screen scroll: `d` / `^D`, `u` / `^U` (a count sets and sticks the step)
- Go to top / bottom: `g` / `G` (with a count: go to line N)
- Count prefixes: leading digits repeat / parameterize the next motion
- Position report: `=`, `^G` (line number + percent)
- Forward/backward search: `/pattern`, `?pattern`, `n`/`N` (regex, smart-case,
  wrap-around), matched against the cell grid's row text, with blue-background
  match highlighting (saturated for the current match, lighter for the others)
- Help overlay: `h` lists the key bindings, any key dismisses
- Repaint: `^L` clears and repaints the current screen
- Quit: `q` / `Q`; `space` at `(END)` quits, like `more(1)`
- Status line: `--More--(NN%)` / `(END)`, reverse-video
- OSC 8 hyperlinks re-emitted on render
- (Bonus over `more`: image rendering — sixel or Kitty, locally and over SSH — the
  whole point of the tool)

## Ranked gaps (the TODO)

Ranking weighs (a) how often a pager user reaches for it, (b) how cheap it is in
gmore's current architecture (cell grid + full-repaint per move), and (c) whether
it interacts awkwardly with sixel rendering.

### Tier 1 — high value, do next

(Search and its highlighting are done — see "what gmore has". The next items are
all Tier 2.)

### Tier 2 — high value, moderate effort

1. **Multiple files + `:n` / `:p`.**
   gmore currently takes one path or stdin. Support `gmore file1 file2 …` and
   next/previous-file navigation. Requires holding several parsed grids (or
   re-parsing on switch) and threading the file list through `run()`. The status
   line should gain the current filename.

2. **Line numbers (`-N` option).**
   Show absolute line numbers in a gutter when requested. Interacts with wrap and
   with the cell grid's notion of a "row" (a wrapped logical line spans several
   grid rows) — needs a decision on whether numbers track logical or screen
   lines. Pairs naturally with search ("go to line / show line").

### Tier 3 — lower value or higher friction

3. **Start position (`+N`, `+/pattern`).**
   Open already scrolled to a line or to the first match. Easy now that search
   exists; low demand.

4. **Mark / return (`m<x>`, `'<x>`, `''`).**
   `less`-style marks and "return to previous position." Convenience; small once
   we track a position stack.

5. **Repeat last command (`.`).**
   Convenience once the command set is larger; small given a command dispatcher.

6. **Shell-out / editor (`!cmd`, `v`).**
   `v` opens `$EDITOR` at the current line; `!cmd` runs a shell command. Useful
   but requires suspending raw mode and the sixel screen state cleanly, then
   repainting — non-trivial given RIS-based repaint, and lower priority for a
   graphics pager whose inputs are often piped, not files.

## Suggested order of work

Search (forward + backward), `n`/`N`, match highlighting, and the help overlay
are all done. The next increments: **multi-file (1)** and **line numbers (2)** as
standalone PRs. Everything in Tier 3 is opportunistic.

## Notes specific to gmore's architecture

- Every motion already does a full `repaint()` (RIS + redraw) to keep sixels
  correct; new motions inherit this for free and need not reason about images.
- `Nav::dispatch` is the command dispatcher (count prefixes, motions, `MESSAGE`
  / `REDRAW` actions); add new commands there and assert them via `--nav-trace`.
- Search and any feature that reads text must pull row text out of the **cell
  grid**, ignoring attribute and image layers — there is no source-text buffer.
- An interactive input line (search pattern, `:` commands) lives on the bottom
  prompt row, which gmore already manages (`showPrompt` / `clearPrompt`).
- `GMORE_KEYS` (scripted keystrokes) gives a non-interactive harness to test each
  new command without a tty — add cases there as features land.
