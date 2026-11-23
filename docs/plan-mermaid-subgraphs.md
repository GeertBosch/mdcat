# Implementation plan — Mermaid follow-up: label text, four shapes, subgraphs

Follow-up to [plan-mermaid-flowcharts.md](plan-mermaid-flowcharts.md), whose
*Guiding decisions*, *Model contract* and *Definition of done* still apply. It
closes the gaps found by running the builtin over every Markdown file in the
MongoDB server repository (`git grep -l mermaid '**/*.md'`: 38 diagrams, 24
files). Of the 38, 5 were drawn and 33 declined:

| Reason | Diagrams | Addressed by |
| ------ | -------: | ------------ |
| not a flowchart (sequence, class, C4) | 17 | never: left to `mmdc` |
| `unsupported syntax: subgraph …` | 7 | Steps 4–8 |
| `unsupported shape: …` | 5 | Steps 2–3 (4 of the 5 also need subgraphs) |
| `html in label` (a literal `c < 5`) | 2 | Step 1 |
| `unsupported label: …` (edge text with `()` or `[]`) | 2 | Step 1 |

The shapes `doc`, `lin-doc`, `docs` and `processes` (`procs`) are **not** in this
plan; the diagrams that use them stay declined.

Syntax reference: <https://mermaid.js.org/syntax/flowchart.html>. `mmdc` is
installed on the development machine (`/usr/local/bin/mmdc`) and is the oracle for
what Mermaid accepts: when a step says "verify with mmdc", render the snippet with
`mmdc -q -i in.mmd -o out.svg` and read the SVG. The facts below marked
*(verified)* were checked that way with the installed version when this plan was
written.

## How to run a step

Each step is sized for **one Sonnet session at high effort**. Use this prompt:

> Implement Step N of `docs/plan-mermaid-subgraphs.md`. Read `CLAUDE.md`, the
> *Guiding decisions*, *Model contract* and *Definition of done* of
> `docs/plan-mermaid-flowcharts.md`, the introduction of
> `docs/plan-mermaid-subgraphs.md`, and Step N. Do only Step N.

| Step | Title | Needs |
| ---- | ----- | ----- |
| 1 | Edge text and literal `<` in labels | — |
| 2 | Shapes III: divided, lined process, lined cylinder | — |
| 3 | Shapes IV: horizontal cylinder (`das`) | 2 |
| 4 | Subgraphs I: parsing and the model | — |
| 5 | Subgraphs II: top-down layout and drawing, one level, no titles | 4 |
| 6 | Subgraphs III: titles | 5 |
| 7 | Subgraphs IV: LR, RL and BT | 6 |
| 8 | Subgraphs V: nesting | 7 |
| 9 | MongoDB corpus, calibration, docs | all |

Steps 1, 2 and 4 are independent. Run steps one at a time anyway: they all touch
the fixtures and goldens, and parallel branches would conflict in them.

### Common to every step

- The tests run through `tests/mermaid-kitty.sh`. A new fixture
  `tests/mermaid-<N>-<name>.md` needs a `golden_case` line there, goldens made
  with `UPDATE=1 tests/mermaid-kitty.sh`, and a row in `tests/README.md`. The next
  free fixture number is **13**.
- Goldens are **read and judged**, not just regenerated. The quadrant preview
  (`.w<N>.txt`) is coarse; for a new outline, also look at the pixels with
  `tests/kitty-preview.py` or in a real Kitty terminal before calling it right.
- A fixture's declined cases end up in its `.err` golden. When a step turns a
  previously declined case into a drawn one (in any fixture), move it to the
  drawn part of the fixture and say so in the final report.
- User-facing documentation lives in [MERMAID.md](MERMAID.md): its section for
  the feature, the *fallback reasons* table in §2, and *Limits*. Every step that
  changes what is drawn updates them. The README demo is touched only in Step 9.
- Keep within the source's style: integer arithmetic for geometry (eighths of a
  pixel, see `src/mermaid_shape.h`), comments that say why, no floating point in
  anything that reaches the PNG.

---

## Step 1 — Edge text and literal `<` in labels

**Assumes:** nothing.

**Start from:** `Parser::parseTextLink`, `plainLabelChar`, `plainLabel`,
`lineBreakAt`, `parseLabelText` and `parseLabel` in `src/mermaid_parse.cpp`.

**Mermaid's rules** *(verified)*:

| Source | Mermaid | mdcat today |
| ------ | ------- | ----------- |
| `A -- foo() --> B` | label `foo()` | `unsupported label: foo()` |
| `A -- [5] --> B` | label `[5]` | `unsupported label: [5]` |
| `A --[7] --> B` (no space after `--`) | label `[7]` | `unsupported syntax` |
| `A -.[x].-> B`, `A ==[y]==> B` | labels `[x]`, `[y]` | `unsupported syntax` |
| `A -- a\|b --> B` | label `a\|b` | `unsupported label: a\|b` |
| `A -->\|foo()\| B`, `A -->\|a[b]\| B` | **parse error** | declined: correct, keep |
| `A --x--> B` | parse error (`--x` is a cross end) | keep declining |
| `A["c < 5"]`, `A["b >= 0"]`, `A["1 <= 2"]` | literal `<` / `>` | `html in label` for `<` |
| `A["a<b"]`, `A["x <i>y</i>"]` | `<b`/`<i>` start a tag (text dropped) | `html in label`: correct, keep |

Work:

1. **Text in the middle of a link** (`-- text -->`, `-. text .->`,
   `== text ==>`): the text is everything between the start and the end of the
   link, and any character other than the link's own stroke run may appear in it
   unquoted, including `( ) [ ] { } |`. Give `parseLabel` (or a variant of it) a
   way to say "edge text" so that these characters are plain text there, while a
   node's `[...]` text and a `|...|` label keep today's rules. Quoted text
   (`-- "…" -->`) and Markdown strings keep working as now.
2. **No space after the start.** `--[7] -->`, `-.[x].->` and `==[y]==>` are a
   text link when the character after the two-character start is not whitespace
   and not one of `- . = > o x <`, and a matching link end follows. Everything
   that parses today must parse the same way: in particular `-->`, `---`, `--o`,
   `--x`, `-.->`, `==>`, `<-->` and links with extra dashes. Run the whole test
   suite after this change before anything else.
3. **Literal `<`.** In any label, `<` starts an HTML tag only when it is followed
   by an ASCII letter, `/` or `!`. Otherwise it is text, as `>` already is.
   `<br>` and its variants still break the line; any other tag still declines
   with `html in label`.

**Out:** `|text|` labels with brackets (Mermaid rejects them too); any HTML other
than `<br>`.

**Tests:** extend `tests/mermaid-8-edge-labels.md` with every row of the table
above that Mermaid accepts (including the space-less starts on solid, dotted and
thick links), and keep the rejected ones as declined cases. Extend
`tests/mermaid-9-label-text.md` with `c < 5`, `b >= 0` and `1 <= 2` in node and
edge labels, and `a<b` as a declined case. Re-run the MongoDB sweep for
`src/mongo/db/repl/README.md`, `src/mongo/db/query/README_logical_models.md` and
`src/mongo/db/exec/runtime_planners/classic_runtime_planner/README.md` (see Step
9 for the command) and report what each now does. The third also uses `<br>`
in node labels and a `--o` link, and may now decline for another reason, which
is fine as long as the report names it.

**Docs:** MERMAID.md §8 (edge labels: any text between `--` and `-->`) and §9
(literal `<`).

---

## Step 2 — Shapes III: divided, lined process, lined cylinder

**Assumes:** nothing.

**Start from:** `shapeNames()` in `src/mermaid_parse.cpp`; `enum class Shape` in
`src/mermaid_model.h`; `shape::isBox`, `Geom::in`, `Geom::ink` and
`shape::footprint` in `src/mermaid_shape.h`; how `Shape::Subroutine` (inner
verticals) and `Shape::Cylinder` (lid) draw their extra strokes, which these three
shapes copy.

| Name and aliases | Mermaid draws | Here |
| ---------------- | ------------- | ---- |
| `div-rect` (`div-proc`, `divided-process`, `divided-rectangle`) | a rectangle with a horizontal line across it near the top | `Shape::DividedRect`: a box (`isBox` true) plus a horizontal stroke one row below the top edge; footprint +1 row, label one row lower (`labelRow` +1) |
| `lin-rect` (`lin-proc`, `lined-process`, `lined-rectangle`, `shaded-process`) | a rectangle with a vertical line near the left side | `Shape::LinedRect`: a box plus one inner vertical stroke where `Subroutine`'s left one is; footprint +2 cols, label centred (as `Subroutine`) |
| `lin-cyl` (`disk`, `lined-cylinder`) | a cylinder with an extra arc | `Shape::LinedCylinder`: `Cylinder` plus a second front arc; footprint +1 row more than `Cylinder`, label one row lower |

Work:

1. **Check the look with mmdc** before writing code. Every name and alias in
   the table is accepted by mmdc *(verified)*. Render each shape as
   `A@{ shape: <name>, label: "x" }` and read the SVG path to see where the
   extra line sits (top or bottom for `lin-cyl`, which side for `lin-rect`).
   Follow what Mermaid draws, and correct the table in this plan if it is wrong.
2. Add the three `Shape` values, their names, footprints and strokes. The extra
   lines are interior strokes, like the subroutine's, so `Geom::in` (the
   silhouette) is that of the base shape and edges meet the outline as before.
3. Styling: a node's `stroke` colour applies to the extra line too; its `fill`
   fills the silhouette.

**Out:** `doc`, `lin-doc`, `docs`, `procs`/`processes` and every other shape not
listed.

**Tests:** new `tests/mermaid-13-shapes-3.md`, golden at w80: each shape top-down
and left-to-right, multi-line labels, a styled one (`style n fill:…,stroke:…`),
edges into each shape off the middle of a side, `BT` (the divided line and the
cylinder's lid must stay at the top when mirrored, as the existing cylinder's lid
does — check how `mirror` treats `Cylinder`), and the aliases. Extend the
arrowhead pixel check in `tests/mermaid-kitty.sh` (the one that reads pixels down
the middle of `a --> <shape>`) to the new shapes.

**Docs:** MERMAID.md §10's table and the list of accepted `@{ shape }` names.

---

## Step 3 — Shapes IV: horizontal cylinder (`das`)

**Assumes:** Step 2.

**Start from:** `Shape::Cylinder` everywhere it appears in
`src/mermaid_shape.h` (constructor fields `cap`, `in`, `ink`, `footprint`) and
`src/mermaid_draw.cpp`.

| Name and aliases | Mermaid draws |
| ---------------- | ------------- |
| `h-cyl` (`das`, `horizontal-cylinder`) | a cylinder on its side: straight top and bottom, elliptic caps left and right, the front cap's full ellipse on the right |

Work:

1. The three names are accepted by mmdc *(verified)*. Check with mmdc which side
   the full ellipse is on, as in Step 2.
2. `Shape::HCylinder`: the cylinder's geometry with the axes swapped. The caps'
   half-width is the analogue of `cap`, bounded by the height (so a short, wide
   node does not get a bulbous cap) and by a quarter of the width. `in` is the
   rectangle between the caps plus the two cap ellipses; `ink` adds the inner arc
   of the right cap (the analogue of the lid's front edge).
3. Footprint: +2 cols for the caps (more if the cap half-width needs it), rows as
   a box; the label stays centred in the part between the caps.
4. Mirroring: in `RL` the full ellipse stays on the right, the way the cylinder's
   lid stays on top in `BT`.

**Tests:** add `h-cyl` and its aliases to `tests/mermaid-13-shapes-3.md`
(top-down, left-to-right with edges entering through the caps, `RL`, a one-line
and a three-line label), and to the arrowhead pixel check.

**Docs:** MERMAID.md §10.

---

## Step 4 — Subgraphs I: parsing and the model

**Assumes:** nothing.

**Start from:** where `subgraph` and `end` are declined today in
`src/mermaid_parse.cpp` (`kUnsupportedSyntax`, `reservedId`); `struct Graph` in
`src/mermaid_model.h`; `parseLabel` for the title.

**Mermaid's rules** *(verified)*:

- `subgraph ID` … `end`. Statements in between are ordinary statements.
- **Membership:** every node that a statement inside the subgraph names (a node
  declaration or either end of a link) belongs to it, **even if it was first named
  outside**: in `A --> C` / `subgraph S` / `C --> F` / `end`, `C` and `F` are in
  `S` and `A` is not.
- A node named inside two sibling subgraphs belongs to the one that **closes
  first** (`subgraph S1: X --> Y`, `subgraph S2: Y --> Z` puts `Y` in `S1`).
- **Nesting:** a node named inside an inner subgraph belongs to the inner one;
  the outer one contains the inner one and the nodes named directly in it.
- **Titles:** `subgraph S` has id and title `S`; `subgraph id [Title]` and
  `subgraph id[Title]` have id `id`, title `Title`; `subgraph s1[" "]` has a
  blank title (none is drawn); `subgraph "Q::plan()"` has id and title
  `Q::plan()`; `subgraph Index Tagging` (spaces, no brackets) has a generated id
  and the title `Index Tagging`.

Work:

1. **Model:** add to `mermaid_model.h`

   ```cpp
   struct Cluster {
       std::string id;
       Label title;          // empty lines: no title
       int parent = -1;      // enclosing cluster, or -1
   };
   // in Graph:
   std::vector<Cluster> clusters;   // in the order they close (inner before outer)
   // in Node:
   int cluster = -1;                // innermost cluster that holds the node, or -1
   ```

   Record the closing order, since the "closes first" rule depends on it.
2. **Parsing:** keep a stack of open subgraphs. Apply the membership rules above
   when a subgraph closes. The title goes through `parseLabel` (quoted, Markdown
   strings, `<br>` and entities as for node labels).
3. **Declines** (each with a reason that names it):
   - `direction` inside a subgraph: `unsupported syntax: direction in subgraph`;
   - a link to or from a subgraph id (`A --> S` where `S` is a subgraph):
     `unsupported syntax: link to subgraph S`;
   - `style`, `class` or `classDef` applied to a subgraph id:
     `unsupported syntax: style on subgraph S`;
   - an `end` with no open subgraph, a subgraph never closed, or two subgraphs
     with one id: `unsupported syntax: …` naming the line.
4. **Gate until drawn:** at the start of `layoutGraph`, a graph with any cluster
   declines with the stable reason `subgraph layout`. Steps 5–8 lift this gate
   bit by bit. So after this step no diagram is drawn differently, only the
   reasons change.

**Out:** any layout or drawing.

**Tests:** move the subgraph cases of `tests/mermaid-2-fallback.md` to a new
`tests/mermaid-14-subgraphs.md` (it grows in later steps). For now it holds the
MongoDB title forms, the membership cases above and every decline above, so its
`.w80.err` golden documents the reasons: `subgraph layout` for the valid ones.
Because no subgraph diagram is drawn yet, the membership rules can't be seen in a
golden. Check them with a small temporary harness (a scratch `main` that parses
and prints `Node::cluster`) and say in the final report that it was done; do not
commit it.

**Docs:** MERMAID.md §2's reason table (`subgraph layout`, the new declines).

---

## Step 5 — Subgraphs II: top-down layout and drawing, one level, no titles

**Assumes:** Step 4.

**Start from:** `layoutGraph` in `src/mermaid_layout.cpp`, in particular the
phases commented *Initial order*, the ordering sweeps
(`reduceCrossings`, `sortByBarycenter`), *Placement across the flow* and its *Refine* passes,
`placeRank` (the packer that places a rank's items near their desired starts),
*Position along the flow* (gaps and tracks), and `validate` in
`src/mermaid_draw.cpp`.

**Lift the gate** for graphs whose clusters are all top-level (no `parent`),
whose titles are blank, and whose direction is `TB` (and `TD`). Others keep
declining with `subgraph layout`.

The approach follows Dagre's compound layout, simplified:

1. **Cluster of every item.** A real node's item has its node's cluster. A dummy
   item (a long edge passing a rank) and an edge-label item belong to the
   **lowest common cluster** of the edge's two ends: the same cluster if both ends
   are in it, else none. That keeps an edge between two outside nodes out of a
   cluster's box, and keeps an edge inside a cluster inside it.
2. **Rank span and border items.** A cluster spans the ranks from its first
   member's to its last member's. On every rank it spans, add a left and a right
   **border item**: zero width, belonging to the cluster, tied to the border items
   of the ranks before and after it as if by an edge, so that ordering and
   placement pull them straight. A rank inside the span with no members still gets
   both border items, so that nothing from outside lands inside the box there.
3. **Ordering keeps clusters contiguous.** Wherever a rank is sorted (the initial
   order and every barycenter sweep), sort in two levels: the entries of a rank are
   its items outside every cluster plus one block per cluster, each block keyed by
   the mean barycenter of its items. Sort the entries, then the items inside each
   block, and lay each block out as left border, members, right border. Ties keep
   the current order, as now. Crossing counts are unchanged.
4. **Spacing across the flow.** Between two neighbouring items of a rank, the
   separation grows by `kClusterPad` (new tunable, 2 columns: the border's column
   and a blank one) for every cluster boundary between them. So a member is never
   closer than one blank column to its border, and an outside item or edge never
   shares a column with a border.
5. **Straight sides.** After placement and refinement, each cluster's box spans
   from the leftmost of its left border items to the rightmost of its right ones.
   Re-pack every rank with its border items' desired starts at the box's sides and
   everything else at its current place, and repeat until nothing moves (at most 8
   rounds). If a rank's border items still are not at the box's sides, decline
   with `subgraph layout: sides` rather than draw a ragged box.
6. **Along the flow.** The box's top border sits in the gap above its first rank
   and its bottom border in the gap below its last rank. Each takes rows of its own
   in the gap, outside the tracks: from top to bottom a gap is the leaving row,
   the bottom borders of clusters that end at the rank above, the tracks, the top
   borders of clusters that start at the rank below, and the arriving row. A
   border row is followed (top) or preceded (bottom) by one blank row inside the
   box. The first rank's top borders and the last rank's bottom borders go outside
   the outermost ranks the same way, growing `Layout::rows`.
7. **Layout output.** Add `std::vector<CellBox> clusterBox;` (per cluster, the
   cells of its outline including the border) to `Layout`, and shift it with the
   rest in `mirror` and `addTitle`.
8. **Drawing.** A cluster is one `Stroke::Outline` of shape `Rect`, with
   `node = -1` and a new field or flag saying it is a cluster, drawn **before** the
   nodes and edges in the theme's stroke colour. (Mermaid fills clusters; mdcat
   does not, since the text layer over a fill would lose the theme's background.)
   Add `Occ::Cluster` to the occupancy grid for border cells.
9. **Validation.** In `validate`: no node footprint, label or edge label overlaps
   a border cell; an edge may cross a border only **perpendicularly** (a vertical
   run through a top or bottom border, a horizontal run through a side), never run
   along one; two top-level boxes do not overlap; every member's footprint lies
   inside its box and no non-member footprint does. Each failure declines with a
   reason starting `subgraph layout:`.

**Out:** titles (Step 6), other directions (Step 7), nesting (Step 8).

**Tests:** `tests/mermaid-14-subgraphs.md` gains drawn TD cases, golden at w80:

- one untitled cluster around a chain, with edges entering and leaving it;
- two sibling clusters, edges between them in both directions (including a back
  edge);
- an edge between two outside nodes that would pass the cluster's ranks: it must
  go round the box, not through it;
- a cluster with a rank in its span that holds no member;
- a long edge inside a cluster, an edge label inside a cluster, and a self-loop on
  a member;
- the MongoDB `src/mongo/db/admission/README.md:100` diagram with its titles
  blanked (`subgraph decrease[" "]`).

**Docs:** MERMAID.md gets a new section *Subgraphs* (after §11), and *Limits*
says what is still declined.

---

## Step 6 — Subgraphs III: titles

**Assumes:** Step 5.

**Lift the gate** for titled top-level clusters in `TB`/`TD`.

Work:

1. **Room.** A titled cluster's top border is followed by the title's lines and
   then the blank row, instead of the blank row alone (Step 5, item 6). The box is
   at least the title's width plus 4 columns wide: if the members make it
   narrower, widen it symmetrically, by raising the separation between its border
   items and the neighbouring items before the straight-sides pass (Step 5, item
   5), so that the widening pushes neighbours away instead of overlapping them.
2. **Wrapping.** A title wider than `kMaxLabelWidth` wraps like a node label
   (Markdown titles through `Env::markdownLines`, plain ones on spaces), within
   the fit attempts of `render` in `src/mermaid.cpp`.
3. **Text.** The title is centred in the row(s) below the top border, in the text
   layer, in bold (an SGR bold that the row's reset closes, as the diagram title
   does). Its cells are `Occ::Label` for validation: no edge may cross them. An
   edge that enters the box from above therefore needs a column clear of the
   title; if the layout puts one through it, decline with `subgraph layout: title`
   (Step 9 decides whether that is common enough to route round).
4. `Layout` gets `std::vector<CellBox> clusterLabel;` (per cluster; `cols == 0`
   if untitled).

**Tests:** `tests/mermaid-14-subgraphs.md`: every MongoDB title form (bare,
`id [Title]`, `id["Title"]`, `"Q::plan()"`, a title with spaces), a title wider
than its members, a Markdown title, a `<br>` title, a title wider than 24 columns,
and the admission diagram with its real titles. Golden at w80 and an `.ansi.txt`
golden for the bold.

**Docs:** MERMAID.md *Subgraphs*.

---

## Step 7 — Subgraphs IV: LR, RL and BT

**Assumes:** Step 6.

**Lift the gate** for top-level clusters in every direction.

Work: the layout runs LR as TB with the axes swapped and BT/RL as mirrors
(`mirror`). Make the cluster phases of Steps 5–6 follow:

1. **LR spacing.** Across the flow (rows, in LR) the pad is `kClusterPadLr`
   (new tunable, 1 row plus the border's row); along the flow (columns) the
   borders take columns in the gaps as rows did in TD.
2. **LR titles.** The title sits in the row(s) below the top border, which in LR
   is across the flow, so those rows join the across-flow pad on the top side
   only. The width the title needs is along the flow: widen the gaps at the
   cluster's first and last ranks, half each, when the box is narrower than the
   title plus 4.
3. **Mirroring.** In BT the title stays below the top border (it is text, not
   flipped); in RL it stays centred. Check `mirror` for every new `Layout` field.

**Tests:** `tests/mermaid-14-subgraphs.md`: the Step 5 and 6 cases in LR, one
in BT and one in RL; the MongoDB `src/mongo/db/commands/query_cmd/README.md:121`
diagram (LR, blank title, `@{ shape: extract }` replaced by a plain box). Goldens
at w80 (and w120 if the LR cases need it).

**Docs:** MERMAID.md *Subgraphs*.

---

## Step 8 — Subgraphs V: nesting

**Assumes:** Step 7.

**Lift the gate** for nested clusters, up to `kMaxClusterDepth` (new tunable, 3).
Deeper nesting declines with `subgraph layout: depth`.

Work:

1. **Ordering** becomes recursive: a cluster's block contains its own items and
   its child clusters' blocks, sorted the same way at each level (Dagre's
   `sortSubgraph`).
2. **Lowest common cluster** (Step 5, item 1) uses the cluster tree: the deepest
   cluster that contains both ends.
3. **Border items** of a child sit inside the parent's; the separation counts
   every boundary crossed between two neighbours, so two nested borders are
   `kClusterPad` apart.
4. **Along the flow,** in one gap, the borders that end are ordered inner first
   and those that start outer first, each with its title rows.
5. **Straight sides** run from the innermost clusters out.
6. **Validation:** a child's box lies strictly inside its parent's; sibling
   boxes do not overlap; borders of different clusters never share a cell.

**Tests:** `tests/mermaid-14-subgraphs.md`: two levels with edges into the inner
box from outside both, an edge from inner to parent-only member, siblings inside
a parent, an LR nested case, and a depth-4 decline. The MongoDB
`src/mongo/db/query/plan_enumerator/README.md:12` diagram with its `doc`, `docs`
and `procs` shapes replaced by supported ones.

**Docs:** MERMAID.md *Subgraphs* and *Limits*.

---

## Step 9 — MongoDB corpus, calibration, docs

**Assumes:** all earlier steps.

1. **Sweep.** With a MongoDB checkout at `$MONGO` (on the development machine,
   `~/mongo`), run every diagram through the builtin and collect the reasons:

   ```sh
   cd "$MONGO"
   for f in $(git grep -l mermaid '**/*.md'); do
     (cd "$(dirname "$f")" && MDCAT_MERMAID_DEBUG=1 MDCAT_CELL_W=10 MDCAT_CELL_H=20 \
        PATH=/usr/bin:/bin "$MDCAT" --img kitty --mermaid builtin --width 80 \
        "$(basename "$f")" 2>&1 >/dev/null | sed "s#^mdcat: [^:]*:#$f:#")
   done
   ```

   (`$MDCAT` is the absolute path of the built `mdcat`.) Diagrams that print
   nothing were drawn. Expected: the 5 previously drawn, plus the 2 Step 1 label
   cases (unless they decline for another reason), plus the shape-only
   `src/mongo/s/query/planner/README.md:288`, plus the subgraph diagrams whose
   shapes are all supported. List every diagram in the final report with its
   result.
2. **Calibration.** Look at each newly drawn diagram at 80 and 120 columns in a
   Kitty terminal (the user does this; give one command per diagram). Tune
   `kClusterPad`, `kClusterPadLr` and the gates (`kColsPerCrossing`,
   `kColsPerBackEdge`), recording the values and why next to the constants. For
   every remaining `subgraph layout: …` decline, decide whether to fix it here (if
   small) or record it in MERMAID.md *Limits*.
3. **Docs.**
   - README's Mermaid demo (README.md, *Mermaid diagrams*): add a small subgraph
     with a title, and use one of the new shapes, keeping the demo short (per
     `CLAUDE.md`, the README must show the feature).
   - MERMAID.md: update the *How a diagram is rendered* example that quotes
     `fallback: unsupported syntax: subgraph one`, the reason table, §10 and
     *Limits*.
   - `docs/plan-mermaid-flowcharts.md`, *Out of scope*: say which shapes this
     plan added.

**Tests:** no new fixture; goldens touched by calibration are re-judged and listed
in the final report.
