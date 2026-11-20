# mdcat tests

Hand-checked rendering samples, one Markdown file per feature. There is no
automatic pass/fail yet — render a file and compare against the description and
"Expected" notes that each file prints at the top of its own output.

Run a single test:

```
make && ./mdcat tests/tables-wrap.md
```

Run them all:

```
make && for f in tests/*.md; do printf '\n===== %s =====\n' "$f"; ./mdcat "$f"; done
```

## Automated checks

Three properties are checked automatically. Run them all via:

```
make check
```

`property-concat.sh` asserts that rendering distributes over the file-argument
list — for any files, `mdcat f1 f2 ... fn` produces exactly the concatenation of
`mdcat f1`, `mdcat f2`, ..., `mdcat fn`. This guards the file-boundary handling:
the last block of one file must never merge with the first block of the next.

`property-width.sh` asserts that the two ways of forcing the render width agree:
`mdcat --width N` and `COLUMNS=N mdcat` produce byte-identical output, checked at
one non-standard width. This guards the width-precedence logic.

`property-blank-lines.sh` asserts that the number of blank lines in the output is
invariant over the render width: blank lines come from block structure, so
wrapping must never add or drop one. Checked across a few widths.

Both run over every `*.md` in this directory and exit non-zero (printing a diff) on a
mismatch.

Several tests describe behavior at a specific terminal width. To check those
deterministically regardless of your real window size, force the width with the
`--width` flag (most reliable, beats everything):

```
make && ./mdcat --width 40 tests/tables-wrap.md
```

`$COLUMNS` also works and takes precedence over the detected terminal size, so
this is equivalent (export it or pass it inline; bash does not export it by
default):

```
make && COLUMNS=40 ./mdcat tests/tables-wrap.md
```

| File | Feature under test |
| ---- | ------------------ |
| `headings.md`        | ATX headings, levels 1-6, and their underlines |
| `inline.md`          | Emphasis, strong, code spans, links |
| `paragraphs.md`      | Paragraph reflow to the terminal width |
| `code-blocks.md`     | Fenced code blocks rendered verbatim |
| `code-indented.md`   | Indented (4-space) code blocks; not interrupting a paragraph |
| `thematic-breaks.md` | Horizontal rules |
| `tables-basic.md`    | A small multi-column table that fits |
| `tables-wrap.md`     | Multi-column tables that must wrap to fit the width |
| `tables-inline-wrap.md` | Inline styling wrapping inside cells, with no spill to adjacent cells |
| `images.md`          | `<img>` rendering and its text fallback |
| `images-aspect.md`   | Tall/square/wide images in one table row; row sized to tallest |
| `images-text-taller.md` | A text cell taller than its neighbor image; text drives row height |
| `images-narrow-col.md`  | Images scaled down to narrow columns; standalone-image regressions |
| `block-quotes.md`    | Block quotes: nesting, lazy continuation, inner blocks, left rule |
| `lists.md`           | Bullet/ordered lists: nesting, tight vs loose, multi-block items |
| `mermaid-basic.md`   | Built-in Mermaid chains and a small tree drawn as a Kitty image plus text labels. Goldens at widths 80 and 30 (the LR chain is too wide at 30), each with a `.err` golden of the fallback reasons (checked by `mermaid-kitty.sh`) |
| `mermaid-dag.md`     | Built-in Mermaid DAGs, top-down and left-to-right: tree, fan-in, diamond, skip edge, long links (`--->`, `---->`), two components, a late source. Goldens at widths 80 and 44 (checked by `mermaid-kitty.sh`) |
| `mermaid-cycles.md`  | Built-in Mermaid cycles: retry loops, a 2-cycle, a long loop, a labelled and a dotted back edge, in all four directions, and the docs' "minimum length of a link" example in both label spellings (drawn alike). Back edges run against the flow with ports of their own. Also self-loops (plain, labelled, thick, dotted, on shapes, in every direction), parallel edges (plain, labelled, three or five between one pair, a back edge twice, `A <--> B`), repeated self-loops (only the last is drawn, as in `mmdc`), and the case declined: `back edges: 10 > 8`. Goldens at width 80 with `.err` reasons (checked by `mermaid-kitty.sh`) |
| `mermaid-routing.md` | Built-in Mermaid graphs that need several tracks between two ranks (two and three, top-down and left-to-right) and crossings, unavoidable ones drawn as bridges. Also aligned boxes, K₃,₃ (eight crossings, the most that fit 80 columns), and the case declined as too complex: `crossings: 24 > 8` (K₄,₄). Goldens at width 80 with `.err` reasons (checked by `mermaid-kitty.sh`) |
| `mermaid-crossings.md` | Built-in Mermaid layout quality: graphs wired so that declaration order would cross edges, untangled by the crossing sweeps; a long edge kept straight through the ranks it passes; a node pulled next to its neighbours by rank tightening; the Mermaid docs' `A & B --> C & D`. Golden at width 80 with an empty `.err` (checked by `mermaid-kitty.sh`) |
| `mermaid-syntax.md`  | Built-in Mermaid syntax: all four directions and the `>`, `<`, `^`, `v` aliases, no direction, `flowchart-elk`, `;` and newline separators, chains, `&`, redefined labels, comments and multi-line directives, a front-matter `title:` (narrow and wider than the diagram), and syntax that is read but has no visual effect (`style`, `classDef`, `class`, `linkStyle`, `click`, `:::cls`, edge ids and properties, `accTitle`, `accDescr`), plus non-ASCII, `.` and `-` in ids. Golden at width 80 with an empty `.err` (checked by `mermaid-kitty.sh`) |
| `mermaid-links.md`   | Built-in Mermaid link styles: arrows, open, dotted, thick, invisible (`~~~`, which places boxes without a stroke), circle and cross ends, markers at both ends, top-down and left-to-right; link lengths; mixed styles fanning out of, and merging into, one box (one port per style); an invisible link that sets the ranking. Golden at width 80 with an empty `.err`; `mermaid-kitty.sh` also reads the pixels of a dotted link to check its dashes (the quadrant preview is too coarse) |
| `mermaid-edge-labels.md` | Built-in Mermaid edge labels: every syntax (`-- text -->`, `-->\|text\|`, `-. text .->`, `== text ==>`, open, circle, cross and both-ended links), a yes/no decision fan-out, a label on a multi-rank link, all four directions, two labelled edges into one target, chains and `&`. Also the cases declined for now: a label over 24 columns, one with HTML, a label on an invisible link. Goldens at widths 80 and 44 with `.err` reasons; `mermaid-kitty.sh` also reads a label's pixels to check that the stroke stops a quarter of a cell's height short of it on both sides |
| `mermaid-label-text.md` | Built-in Mermaid label text: quoted labels (brackets, pipes and `-->` inside quotes), entity codes (`#quot;`, `#35;`, `#9829;`, `#amp;`), `<br>` line breaks (taller boxes, each line centred, top-down and left-to-right), Markdown strings (`**bold**`, `*italic*`, `` `code` ``, a newline as a hard break, an entity that is not read as Markdown), a Markdown edge label, wrapping at 24 columns and `markdownAutoWrap: false`, plain labels that stay literal, and CJK and emoji. Also the cases declined: other HTML, an unknown entity, an unclosed quote, HTML in a Markdown string, a quote inside quotes. Golden at width 80 with `.err` reasons and a `.ansi.txt` golden of the image rows' text with its styles (`\e` for ESC; `mermaid-kitty.sh` uses `kitty-preview.py --ansi`) |
| `mermaid-shapes.md` | Built-in Mermaid node shapes: box, rounded box, stadium, circle, double circle, subroutine, cylinder, rhombus and hexagon, top-down and left-to-right; a yes/no decision on a rhombus; multi-line labels that widen the circle and rhombus; edges that leave and arrive off the middle of a curved or slanted side; `BT` and `RL` (the cylinder's lid stays on top); a redefined shape; quoted and Markdown labels in shapes. Also the cases declined: a slanted `[/text/]` and an unclosed shape. Golden at width 80 with `.err` reasons; `mermaid-kitty.sh` also reads the pixels down the middle of `a --> <shape>` to check that each arrowhead touches its outline |
| `mermaid-fallback.md` | A `sequenceDiagram`, a `pie`, `subgraph` flowcharts (one with `direction`), a bare `direction`, `end` as an id, a link with mismatched ends and an unknown direction: all declined by the builtin, shown as source, with `.w80.err` reasons (checked by `mermaid-kitty.sh`) |

`kitty-preview.py` turns mdcat's Kitty output into text for goldens: each image's
strokes become quadrant blocks, and its PNGs are decoded and validated.
`mermaid-kitty.sh` uses it, and it can be run on its own:
`MDCAT_CELL_W=10 MDCAT_CELL_H=20 ./mdcat --img kitty doc.md | tests/kitty-preview.py`.
With `--ansi` it prints only the rows the images cover, with the SGR styles of the text.
