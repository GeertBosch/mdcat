# gmore — missing `more(1)` features, ranked

`gmore` is a graphics-aware pager: it emulates a terminal subset into a cell grid
so it can page any text+sixel stream. The paging *mechanics* (scroll, sixel
re-encode, full-repaint) work well. What it still lacks is most of the
**interaction surface** that a user of UNIX `more(1)` expects.

This document inventories the `more(1)` feature set, marks what gmore already
does, and ranks the gaps by value-to-effort so we can work top-down.

## What gmore has today

- Forward paging: `space`, `f` (one screen)
- Backward paging: `b` (one screen)
- Line stepping: `Enter` / `j` (down), `k` / `y` (up)
- Quit: `q` / `Q`; `space` at `(END)` quits, like `more(1)`
- Status line: `--More--(NN%)` / `(END)`, reverse-video
- (Bonus over `more`: sixel image rendering, the whole point of the tool)

## The `more(1)` command vocabulary, for reference

Real `more` accepts an optional **count** prefix before most commands (e.g. `10j`),
and uses these keys:

| Key            | Action                                        |
|----------------|-----------------------------------------------|
| `space`, `f`   | forward one screen (or N lines with count)    |
| `b`, `^B`      | backward one screen                           |
| `Enter`        | forward one line (or N)                        |
| `d`, `^D`      | forward half screen (and set the scroll size) |
| `u`, `^U`      | backward half screen                          |
| `/pattern`     | search forward for regex                      |
| `?pattern`     | search backward (less; more has `/` only)     |
| `n`            | repeat last search                            |
| `'` (apostroph)| go to start of previous search / file start   |
| `g` / `G`      | go to first line / last line (with count: N)  |
| `=`, `^G`      | show current line number / filename + %        |
| `:n` / `:p`    | next / previous file (multi-file mode)         |
| `v`            | open `$EDITOR` at the current line             |
| `!cmd`         | run a shell command                           |
| `h`            | help screen of commands                       |
| `.`            | repeat the previous command                   |
| `q`, `Q`       | quit                                          |

Plus invocation options: `-N` line numbers, `+N`/`+/pat` start position,
`-p`/`-c` clear behaviour, multiple file arguments, `-` to read stdin.

## Ranked gaps

Ranking weighs (a) how often a pager user reaches for it, (b) how cheap it is in
gmore's current architecture (cell grid + full-repaint per move), and (c) whether
it interacts awkwardly with sixel rendering.

### Tier 1 — high value, low effort (do first)

1. **Count prefixes (`Nj`, `Nf`, `N` + `space` …).**
   Parse leading digits into a repeat count consumed by the next motion. Touches
   only the key loop; `advance`/`retreat` already take an `n`. Foundation for
   `g`/`G` and half-page commands too.

2. **Go to top / bottom (`g` / `G`).**
   `g` → `viewTop = 0`; `G` → `viewTop = maxTop`; with a count, go to line N.
   Trivial given `repaint()`; very frequently wanted in a pager.

3. **Half-screen scroll (`d` / `u`, `^D` / `^U`).**
   `advance(pageH/2)` / `retreat(pageH/2)`. A staple of `less`/`more` navigation,
   essentially free.

4. **Status / position report (`=`, `^G`).**
   Show current top line number, total lines, and percent in the prompt. We
   already compute percent; just surface line counts. Needs the grid to expose
   `total` (it does).

5. **Help screen (`h`).**
   A static overlay listing the keys, dismissed by any key, then `repaint()`.
   Cheap, and it makes every other feature discoverable.

### Tier 2 — high value, moderate effort

6. **Forward search (`/pattern`, `n`).**
   The single biggest reason people use a pager over `cat`. Needs: a prompt line
   that reads a line of input (we already own the bottom row), a regex match over
   the cell grid's text per row, scroll so the hit is at the top, and `n` to
   repeat. Search highlighting is a nice-to-have on top. Effort is in the input
   editor and matching text out of the cell grid (styling/sixels must be ignored
   when extracting row text).

7. **Multiple files + `:n` / `:p`.**
   gmore currently takes one path or stdin. `more file1 file2` and next/prev file
   navigation. Requires holding several parsed grids (or re-parsing on switch) and
   threading the file list through `run()`. The status line gains a filename.

8. **Line numbers (`-N` option / `=`).**
   Show absolute line numbers in a gutter when requested. Interacts with wrap and
   with the cell grid's notion of a "row" (a wrapped logical line spans several
   grid rows) — needs a decision on whether numbers track logical or screen lines.

### Tier 3 — lower value or higher friction

9. **Start position (`+N`, `+/pattern`).**
   Open scrolled to a line or first match. Easy once search exists; low demand.

10. **Repeat last command (`.`).**
    Convenience once the command set is larger; small once we have a command
    dispatcher.

11. **Shell-out / editor (`!cmd`, `v`).**
    `v` opens `$EDITOR` at the current line; `!cmd` runs a shell command. Useful
    but requires suspending raw mode and the sixel screen state cleanly, then
    repainting — non-trivial given RIS-based repaint. Lower priority for a
    graphics pager whose inputs are often piped, not files.

12. **Backward search (`?pattern`).**
    `more(1)` itself only has `/`; this is a `less` feature. Cheap once forward
    search exists, but defer until asked.

## Suggested order of work

Tier 1 (1→5) is a single small PR: a command dispatcher with count prefixes, the
motion commands, the position report, and a help overlay. That alone closes most
of the everyday gap. Then tackle **search (6)** as its own PR — it is the highest
*value* item but deserves isolation because of the input-line editor and text
extraction work. **Multi-file (7)** follows. Everything in Tier 3 is opportunistic.

## Notes specific to gmore's architecture

- Every motion already does a full `repaint()` (RIS + redraw) to keep sixels
  correct; new motions inherit this for free and need not reason about images.
- Search and any feature that reads text must pull row text out of the **cell
  grid**, ignoring attribute and image layers — there is no source-text buffer.
- An interactive input line (search pattern, `:` commands) lives on the bottom
  prompt row, which gmore already manages (`showPrompt` / `clearPrompt`).
- `GMORE_KEYS` (scripted keystrokes) gives us a non-interactive harness to test
  each new command without a tty — add cases there as features land.
