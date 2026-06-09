# Sixel probe scripts

Durable diagnostics for how a terminal positions the cursor around sixel
graphics. `mdcat` renders `<img>` PNGs as sixel via `timg`, and placing those
images precisely on the text grid (especially inside tables, and eventually in a
pager) requires knowing the terminal's exact behavior. These scripts answer the
specific questions; run them in the terminal under test and read the printed
output and/or a screenshot.

All scripts source `probe-common.sh`, require a TTY and `timg`, and render
`$PROBE_PNG` (default `tests/chess-piece.png`, 64×64). Run from the repo root,
e.g. `tools/probe-cell-size.sh`.

| Script | Question it answers | How to read it |
| --- | --- | --- |
| `probe-cell-size.sh` | How many pixels is one character cell (esp. height)? | Prints derived `W×H` from `CSI 16t/14t/18t`. If no reply, count how many ruler rows the native-size image covers. |
| `probe-cursor-after-sixel.sh` | After a sixel, where is the text cursor — advanced to the bottom, or not? Does it scroll? Does DECSDM (`CSI ?80h/l`) matter? | Compare the BEFORE/AFTER cursor rows it reports; screenshot shows where the `<HERE` marker lands vs. the image. |
| `probe-absolute-vs-saverestore.sh` | Which cursor strategy survives a sixel: DECSC/DECRC, CSI s/u, or absolute `CSI r;cH`? | Screenshot: the strategy whose `RESUMED-x` text lands correctly under its image (no overlap) wins. |
| `probe-grid-image.sh` | Can we paint an image into one column of a text grid without bleeding into neighboring rows? | `PROBE_STRATEGY=abs\|decsc\|sco`. Image must stay boxed between the two separators with side columns intact. |
| `probe-overshoot.sh` | Does the last 6px sixel band spill into the next text row when cell height isn't a multiple of 6? Does reprinting clean it? | `PROBE_CELL_H=<px>`. Compare the bottom seam before/after the reprinted hash line. |
| `probe-clear-scrolled-sixel.sh` | Once a TALL sixel has been relocated by scrolling (as gmore's down-scroll does), which clear sequence erases its raster — `ESC[2J`, `+ESC[3J`, soft reset, or `ESC c` (RIS)? | `PROBE_TALL=<px>`, `SCROLL=<lines>`. The first clear whose marker screen shows NO image ghost wins. On VSCode only RIS works; gmore's up-scroll uses it. |

Environment overrides: `PROBE_PNG`, `PROBE_STRATEGY`, `PROBE_CELL_H`, `PROBE_TALL`, `SCROLL`.

These are kept (not throwaway) so the same questions can be re-answered on iTerm,
xterm (`xterm -ti vt340`), and other terminals as support is added.
