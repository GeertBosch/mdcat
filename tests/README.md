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
| `mermaid-1-basic.md` | Built-in Mermaid chains and a small tree drawn as a Kitty image plus text labels. Goldens at widths 80 and 30 (the LR chain only fits 30 columns with tight spacing), each with a `.err` golden of the fallback reasons (checked by `mermaid-kitty.sh`) |
| `mermaid-2-fallback.md` | A `classDiagram`, a `pie`, a bare `direction`, `end` as an id, a link with mismatched ends and an unknown direction: all declined by the builtin, shown as source, with `.w80.err` reasons (checked by `mermaid-kitty.sh`) |
| `mermaid-3-dag.md`   | Built-in Mermaid DAGs, top-down and left-to-right: tree, fan-in, diamond, skip edge, long links (`--->`, `---->`), two components, a late source. Goldens at widths 80 and 44 (checked by `mermaid-kitty.sh`) |
| `mermaid-4-routing.md` | Built-in Mermaid graphs that need several tracks between two ranks (two and three, top-down and left-to-right) and crossings, unavoidable ones drawn as bridges. Also aligned boxes, K₃,₃ (eight crossings, the most that fit 80 columns), and the case declined as too complex: `crossings: 21 > 8` (K₄,₄). Goldens at width 80 with `.err` reasons (checked by `mermaid-kitty.sh`) |
| `mermaid-5-crossings.md` | Built-in Mermaid layout quality: graphs wired so that declaration order would cross edges, untangled by the crossing sweeps; a long edge kept straight through the ranks it passes; a node pulled next to its neighbours by rank tightening; the Mermaid docs' `A & B --> C & D`. Golden at width 80 with an empty `.err` (checked by `mermaid-kitty.sh`) |
| `mermaid-6-syntax.md` | Built-in Mermaid syntax: all four directions and the `>`, `<`, `^`, `v` aliases, no direction, `flowchart-elk`, `;` and newline separators, chains, `&`, redefined labels, comments and multi-line directives, a front-matter `title:` (narrow and wider than the diagram), and syntax that is read but has no visual effect (`style`, `classDef`, `class`, `linkStyle`, `click`, `:::cls`, edge ids and properties, `accTitle`, `accDescr`), plus non-ASCII, `.` and `-` in ids. Golden at width 80 with an empty `.err` (checked by `mermaid-kitty.sh`) |
| `mermaid-7-links.md` | Built-in Mermaid link styles: arrows, open, dotted, thick, invisible (`~~~`, which places boxes without a stroke), circle and cross ends, markers at both ends, top-down and left-to-right; link lengths; mixed styles fanning out of, and merging into, one box (one port per style); an invisible link that sets the ranking. Golden at width 80 with an empty `.err`; `mermaid-kitty.sh` also reads the pixels of a dotted link to check its dashes (the quadrant preview is too coarse) |
| `mermaid-8-edge-labels.md` | Built-in Mermaid edge labels: every syntax (`-- text -->`, `-->\|text\|`, `-. text .->`, `== text ==>`, open, circle, cross and both-ended links), a yes/no decision fan-out, a label on a multi-rank link, all four directions, two labelled edges into one target, chains and `&`, brackets and pipes in link text (`-- foo() -->`), and text links with no space after the start (`--[7] -->`, `-.[x].->`, `==[y]==>`). Also long labels (over 24 columns, wrapped after a space; a single word of up to 40 columns stays whole) and the cases declined for now: a label with a single word over 40 columns, one with HTML, a label on an invisible link, `-->\|foo()\|`, `--x-->`, text touching the link end. Goldens at widths 80 and 44 with `.err` reasons; `mermaid-kitty.sh` also reads a label's pixels to check that the stroke stops a quarter of a cell's height short of it on both sides |
| `mermaid-9-label-text.md` | Built-in Mermaid label text: quoted labels (brackets, pipes and `-->` inside quotes), entity codes (`#quot;`, `#35;`, `#9829;`, `#amp;`), `<br>` line breaks (taller boxes, each line centred, top-down and left-to-right), Markdown strings (`**bold**`, `*italic*`, `` `code` ``, a newline as a hard break, an entity that is not read as Markdown), text after a quoted string (`["`**title**`"` then a line), a Markdown edge label, wrapping at 24 columns and `markdownAutoWrap: false`, plain labels that stay literal, and CJK and emoji, a literal `<` (`c < 5`, `b >= 0`, `1 <= 2`), blank labels (`s(" ")`, `c{" "}`: empty boxes) and a spacer (a blank node with `style … height:0px`, kept in the layout and not drawn). Also the cases declined: `b[""]`, other HTML, an unknown entity, an unclosed quote, HTML in a Markdown string, a quote inside quotes, `a<b`. Golden at width 80 with `.err` reasons and a `.ansi.txt` golden of the image rows' text with its styles (`\e` for ESC; `mermaid-kitty.sh` uses `kitty-preview.py --ansi`) |
| `mermaid-10-shapes.md` | Built-in Mermaid node shapes: box, rounded box, stadium, circle, double circle, subroutine, cylinder, rhombus and hexagon, top-down and left-to-right; a yes/no decision on a rhombus; multi-line labels that widen the circle and rhombus; edges that leave and arrive off the middle of a curved or slanted side; `BT` and `RL` (the cylinder's lid stays on top); a redefined shape; quoted and Markdown labels in shapes. Also the cases declined: a slanted `[/text/]` and an unclosed shape. Golden at width 80 with `.err` reasons; `mermaid-kitty.sh` also reads the pixels down the middle of `a --> <shape>` to check that each arrowhead touches its outline |
| `mermaid-11-cycles.md` | Built-in Mermaid cycles: retry loops, a 2-cycle, a long loop, a labelled and a dotted back edge, in all four directions, and the docs' "minimum length of a link" example in both label spellings (drawn alike). Back edges run against the flow with ports of their own. Also self-loops (plain, labelled, thick, dotted, on shapes, in every direction), parallel edges (plain, labelled, three or five between one pair, a back edge twice, `A <--> B`), repeated self-loops (only the last is drawn, as in `mmdc`), and the case declined: `back edges: 10 > 8`. Goldens at width 80 with `.err` reasons (checked by `mermaid-kitty.sh`) |
| `mermaid-12-fit.md` | Built-in Mermaid fitting to the width: tighter spacing, then labels wrapped at 24, 16 and 12 columns, never a change of direction; a diagram that still does not fit is drawn wider than the terminal. Goldens at widths 120, 80 and 50 with empty `.err` goldens (checked by `mermaid-kitty.sh`) |
| `mermaid-13-shapes-3.md` | Built-in Mermaid shapes with one extra stroke, written `@{ shape: … }` only: the divided process `div-rect` (a line a row under the top), the lined process `lin-rect` (a line just inside the left side) and the lined cylinder `lin-cyl` (a second arc under the lid), each top-down and left-to-right, with multi-line labels, several edges into and out of each, `BT` and `RL` (the line and the lids stay on top, the lined box's line stays on the left), a styled one (stroke colours the extra line, fill fills the outline) and every alias; and the horizontal cylinder `h-cyl` (a half ellipse at the left end, a full one at the right, also in `RL` and `BT`), with one-line and three-line labels, several edges into its caps, a styled one and its aliases `das` and `horizontal-cylinder`. Also the case declined: `lin-doc`. Golden at width 80 with a `.err` reason; `mermaid-kitty.sh` also reads the pixels down the middle of `a --> <shape>` to check each arrowhead touches the outline |
| `mermaid-14-subgraphs.md` | Mermaid `subgraph … end`: parsed into the model (`Graph::clusters`, `Node::cluster`) and drawn, in every direction and nested up to three levels deep, with its title in bold under the top border, or none when the title is blank (`subgraph s[" "]`): one box around a chain with edges in, out and round it; two boxes with edges both ways and a back edge; an edge between outsiders passing a box; a rank of the span with no member; a long edge, an edge label and a self-loop inside a box; edges into and out of the middle of a box; styled members, a lone box and boxes side by side; a box in a longer chain; MongoDB's admission diagram with its titles blanked or real (its 36-column edge label wraps) and with shorter labels, blank and real (all drawn); titles: centred when no edge crosses the row, beside the edge that enters the box (the box grows on that side), every form (`S`, `id [Title]`, `id[Title]`, `id["Title"]`, `"Q::plan()"`, `Index Tagging`, Markdown, `<br>` and `;`), wider than the members, a Markdown title wrapped at 24 columns, plain titles wrapped by the fit attempts, and boxes side by side with titles of different heights; the membership rules that need no nesting (a node first named outside, a node named in two siblings, `style`/`class` inside, `&`, `;` and one-line forms, ids that start with `end`); and a declined case where an edge would have to cross a box (`subgraph layout: edge`); in LR, BT and RL: the chain, two boxes with a back edge, an edge round a box, a rank with no member, a long edge with a label and a self-loop, titles (centred, wider than the box, beside an edge that crosses the top border, two boxes with titles of different heights, an edge leaving through the title's rows in BT), and MongoDB's query-commands diagram (LR, `extract` shape left out) with shorter labels (drawn) and as written (drawn: its edge labels are single words of up to 30 columns), plus the declined box-crossing case in LR. Nested, in every direction: edges into the inner box from outside both, an edge from the inner box to a member of the outer one only with a back edge, a label, a self-loop and long edges in the inner box, titles with both boxes starting on one rank, an inner box that starts later and ends earlier, a title wider than the box (inner, outer), siblings inside a parent, a box that holds a box and a node of its own, three levels (TD and LR), MongoDB's plan-enumeration diagram (a box of boxes only; `procs`, `docs` and `doc` shapes replaced by `subproc` and `rect`), and the four membership cases of the parser (a box inside another, a node named again in the outer one, three levels and siblings, a subgraph of subgraphs only); BT and RL nested with titles and BT with two boxes in a parent; and a declined case, nesting deeper than three levels (`subgraph layout: depth`). Also the parser's declines: `direction` inside, a link to or from a subgraph id, a subgraph id as a node, `style`/`class` on one, a stray `end`, a subgraph never closed, a repeated id, an empty subgraph, `end` as an id and bad titles. Golden at width 80 with `.err` reasons and an `.ansi.txt` golden for the bold of the titles (checked by `mermaid-kitty.sh`); which node is in which subgraph is not visible there and was checked with a scratch parser harness |
| `mermaid-15-sequence.md` | Built-in Mermaid sequence diagrams (participants and messages; notes and self-messages are in `mermaid-16-sequence-notes.md`, blocks are in `mermaid-17-sequence-blocks.md`): every arrow kind (`->>`, `-->>`, `->`, `-->`, `-x`, `--x`, `-)`, `--)`) with a title; implicit participants whose names hold spaces, including two that differ only in case; aliases (`participant A as Long name`, with `<br>`, a later `as` that renames, any case of the keyword); a message that spans three lifelines, which break behind its text, in both directions and skipping one; multi-line text (`<br/>`, `<br>`) and an empty text; and the syntax that is read and not drawn: `;` separators, `%%` and `#` comments, `#35;`, `->>+` / `-->>-`, `activate` / `deactivate` (drawn as activations), `accTitle`, a front-matter title. Also one declined case for every reason: each unsupported keyword (`actor`, `box`, `create`, `destroy`, `autonumber`, `rect`, `link`, `links`, `properties`, `details`), `<<->>`, a message with no colon, a half arrow, `activate` of an unknown participant, a stray `else` and `end`, a block never closed, HTML in text, an empty diagram, and too large (13 participants; 151 messages). Golden at width 80 with `.err` reasons (checked by `mermaid-kitty.sh`) |
| `mermaid-16-sequence-notes.md` | Built-in Mermaid sequence diagrams, step 3: self-messages in all eight arrow styles (loop to the right, text above it, an empty text, the last participant's two-line text widening the picture); notes in all four placements (`over` one lifeline, over two in both orders, over three with the lifeline between breaking at the border, `left of` the first lifeline widening the picture, `right of` the last, a four-line one at the right edge, and notes beside a lifeline that move the neighbour); activations (`+` / `-`, nested to three levels, arrows into the box from both sides, a self-message leaving from the side of the stack, standalone `activate` / `deactivate`, a note over a box cutting it, one still open at the end, a zero-height one skipped); and three declined cases (`-` with nothing to close, `deactivate` with nothing open, a message whose text passes in front of an activated lifeline: `sequence: overlap`). Golden at width 80 with `.err` reasons and the text layer's styles (`.ansi.txt`: the note fill), checked by `mermaid-kitty.sh` |
| `mermaid-17-sequence-blocks.md` | Built-in Mermaid sequence diagrams, step 4: blocks as frames with a tab and bracketed text. One of each keyword (`loop`, `opt`, `break`); sections (`alt` with two `else`, `par` with `and`, `critical` with two `option`, dotted divider and text on the row below); a `loop` inside a `par` section with a long divider text that wraps; a frame touching only the middle two of four lifelines; a frame around notes (`right of` the last lifeline, `over` two, `left of` the first) with a two-line header; a frame with no text and an empty section; a divider text wrapping beside an activated lifeline; and two declined cases (`sequence: unsupported syntax: empty block`, also one holding only an empty block). Golden at width 80 with `.err` reasons, checked by `mermaid-kitty.sh` |

The `mermaid-<N>-*.md` fixtures are numbered in the order the features were built, from
the basics to the advanced; `docs/MERMAID.md` follows the same progression.

`kitty-preview.py` turns mdcat's Kitty output into text for goldens: each image's
strokes become quadrant blocks, and its PNGs are decoded and validated.
`mermaid-kitty.sh` uses it, and it can be run on its own:
`MDCAT_CELL_W=10 MDCAT_CELL_H=20 ./mdcat --img kitty doc.md | tests/kitty-preview.py`.
With `--ansi` it prints only the rows the images cover, with the SGR styles of the text.
