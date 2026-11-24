# Implementation plan — Mermaid: sequence diagrams, five shapes, entities, border nudge

Follow-up to [plan-mermaid-subgraphs.md](plan-mermaid-subgraphs.md). The
*Guiding decisions*, *Model contract* and *Definition of done* of
[plan-mermaid-flowcharts.md](plan-mermaid-flowcharts.md) still apply.

## Audit: what still does not draw

The sweep below ran on `main` at `effa319` over every Markdown file in the MongoDB
server repository (38 diagrams, 24 files):

```sh
tools/mermaid-sweep.sh "$PWD/mdcat" ~/mongo 80
```

**10 of 38 draw.** The table lists the other 28, with **the first cause** each one
hits. The *Next cause* column shows the next decline once the first cause is gone.
It was measured with a scratch build that maps every missing shape to `rect`. For
the 17 non-flowcharts it was not measured.

| # | Diagram | Type | First cause | Next cause |
| -: | ------- | ---- | ----------- | ---------- |
| 1 | `buildscripts/resmokelib/hang_analyzer/README.md:32` | sequence | not a flowchart | — |
| 2 | `buildscripts/resmokelib/hang_analyzer/README.md:98` | sequence | not a flowchart | — |
| 3 | `docs/idl.md:904` | sequence | not a flowchart | — |
| 4 | `docs/packaging.md:3` | sequence | not a flowchart | — |
| 5 | `docs/server_parameters.md:420` | sequence | not a flowchart | — |
| 6 | `docs/server_parameters.md:438` | sequence | not a flowchart | — |
| 7 | `src/mongo/db/global_catalog/ddl/README_ddl_operations.md:13` | sequence | not a flowchart | — |
| 8 | `src/mongo/db/s/README_migrations.md:9` | sequence | not a flowchart | — |
| 9 | `src/mongo/db/s/balancer/README.md:11` | sequence | not a flowchart | — |
| 10 | `src/mongo/db/s/balancer/README.md:42` | sequence | not a flowchart | — |
| 11 | `src/mongo/db/versioning_protocol/README_versioning_protocols.md:19` | sequence | not a flowchart | — |
| 12 | `docs/server_parameters.md:470` | class | not a flowchart | — |
| 13 | `docs/server_parameters.md:504` | class | not a flowchart | — |
| 14 | `docs/server_parameters.md:540` | class | not a flowchart | — |
| 15 | `src/mongo/db/local_catalog/README_sharding_catalog.md:13` | class | not a flowchart | — |
| 16 | `src/mongo/db/versioning_protocol/README_versioning_protocols.md:69` | class | not a flowchart | — |
| 17 | `src/mongo/db/sharding_environment/README.md:37` | C4Component | not a flowchart | — |
| 18 | `src/mongo/db/commands/query_cmd/README.md:121` | flowchart | shape `extract` | draws |
| 19 | `src/mongo/db/commands/query_cmd/README.md:183` | flowchart | shape `extract` | `label too wide: 45 > 40`: `&lt;`/`&gt;` not decoded |
| 20 | `src/mongo/db/commands/query_cmd/README.md:221` | flowchart | shape `extract` | draws |
| 21 | `src/mongo/db/exec/runtime_planners/classic_runtime_planner/README.md:157` | flowchart | shape `procs` | `subgraph layout: edge` |
| 22 | `src/mongo/db/matcher/README.md:341` | flowchart | shape `docs` | `subgraph layout: edge` |
| 23 | `src/mongo/db/pipeline/README.md:173` | flowchart | shape `processes` | draws |
| 24 | `src/mongo/db/query/README_QO.md:27` | flowchart | shape `docs` | draws (also needs `doc`) |
| 25 | `src/mongo/db/query/README_explain.md:549` | flowchart | shape `doc` | draws (also needs `lin-doc`) |
| 26 | `src/mongo/db/query/plan_enumerator/README.md:12` | flowchart | shape `procs` | draws (also needs `doc`, `docs`) |
| 27 | `src/mongo/db/query/plan_enumerator/README.md:516` | flowchart | shape `procs` | draws (also needs `doc`, `docs`, `tag-rect`, `win-pane`) |
| 28 | `src/mongo/s/query/planner/README.md:50` | flowchart | shape `docs` | draws |

The causes, grouped:

| Cause | Diagrams | Size of fix | Chosen |
| ----- | -------: | ----------- | ------ |
| Sequence diagram | 11 | a new parser, layout and drawing; reuses the raster and text layers | **yes** (Steps 1–5) |
| Missing flowchart shape (`doc`, `lin-doc`, `docs`, `procs`/`processes`, `tag-rect`, `win-pane`, `extract`) | 11 (8 draw once the shapes exist) | seven outlines in an existing framework | **yes** (Steps 6–7) |
| HTML entity (`&lt;`) not decoded | 1 (behind a shape) | small: the decoder already reads `#lt;` | **yes**, bundled with the shapes (Step 8) |
| `subgraph layout: edge` (a stub stays level with a border after 3 nudges) | 2 (behind a shape) | layout work. A scratch build allowing 12 nudges draws `matcher:341`; `classic_runtime_planner:157` still declines | **yes** (Step 9) |
| Class diagram | 5 | a second new renderer, with boxes of members and UML arrowheads | no: left to `mmdc` |
| C4 diagram | 1 | rare; its own layout rules | no: left to `mmdc` |

So the three cases chosen are **sequence diagrams**, **the missing shapes with
entity decoding**, and **the subgraph border nudge**. Sequence diagrams are the
largest group, and authors who use them have no flowchart alternative. The
shapes are cheap and unlock 8 diagrams. Once they are done, the entities and the
nudge are the only things in the way of 3 more.

Projected counts, if the steps run in order:

| After step | Drawn | Newly drawn |
| ---------- | ----: | ----------- |
| baseline | 10 | |
| 2 | 12 | #1, #2 |
| 3 | 16 | #3, #4, #5, #11 |
| 4 | 21 | #6, #7, #8, #9, #10 |
| 6 | 24 | #24, #25, #28 |
| 7 | 29 | #18, #20, #23, #26, #27 |
| 8 | 30 | #19 |
| 9 | 32 | #21, #22 |

That leaves 6, the class and C4 diagrams, for `mmdc`.

Syntax references: <https://mermaid.js.org/syntax/sequenceDiagram.html> and
<https://mermaid.js.org/syntax/flowchart.html#complete-list-of-new-shapes>. `mmdc`
11.15.0 is installed on the development machine (`/usr/local/bin/mmdc`) and is the
oracle for what Mermaid accepts and how it draws. To "verify with mmdc", run
`mmdc -q -i in.mmd -o out.svg` and read the SVG. Facts marked *(verified)* below
were checked that way when this plan was written.

## How to run a step

Each step is sized for **one Sonnet session at high effort**. Use this prompt:

> Implement Step N of `docs/plan-mermaid-sequence.md`. Read `CLAUDE.md`, the
> *Guiding decisions*, *Model contract* and *Definition of done* of
> `docs/plan-mermaid-flowcharts.md`, the introduction and *Common to every step*
> of `docs/plan-mermaid-sequence.md`, and Step N. Do only Step N. Finish by
> checking every success criterion of Step N and reporting each one as met or
> not met, with evidence.

| Step | Title | Needs |
| ---- | ----- | ----- |
| 1 | Shared painter: strokes and text to a `Diagram` | — |
| 2 | Sequence I: parser, model, participants and messages | 1 |
| 3 | Sequence II: self-messages, notes, activations | 2 |
| 4 | Sequence III: loop, alt, opt, par, critical, break | 3 |
| 5 | Sequence IV: fitting the width, corpus, docs, README | 4 |
| 6 | Shapes V: document shapes (`doc`, `lin-doc`, `docs`) | — |
| 7 | Shapes VI: `procs`, `tag-rect`, `win-pane`, `extract` | 6 |
| 8 | HTML entities in labels | — |
| 9 | Subgraph border nudge | 7 |
| 10 | Corpus, calibration, README for the flowchart steps | 6–9 |

Steps 6–9 do not depend on Steps 1–5, and they may run first. Run the steps one
at a time anyway: they all touch fixtures, goldens and `MERMAID.md`, so parallel
branches would conflict.

The reviewer (the main session) reads each step's diff and goldens before
starting the next one. Each step ends with one commit, per `CLAUDE.md`.

### Common to every step

- **Tests.** The tests run through `tests/mermaid-kitty.sh`. A new fixture
  `tests/mermaid-<N>-<name>.md` needs a `golden_case` line there, goldens made
  with `UPDATE=1 tests/mermaid-kitty.sh`, and a row in `tests/README.md`. The
  next free fixture number is **15**. `make check` must pass.
- **Goldens are read and judged**, not just regenerated. The quadrant preview
  (`.w<N>.txt`) is coarse, so for a new outline also look at the pixels with
  `tests/kitty-preview.py`. The Read tool shows a transparent PNG as solid grey,
  so composite it onto white first. If a golden of an earlier fixture changes,
  the final report names it and says why.
- **Corpus.** `tools/mermaid-sweep.sh "$PWD/mdcat"` (MongoDB checkout at
  `~/mongo`) gives each diagram's result. Every step that changes what is drawn
  runs it, and quotes the summary counts and the newly drawn diagrams in its
  report. **No diagram that drew before may decline.**
- **Docs.** User-facing documentation lives in [MERMAID.md](MERMAID.md): the
  feature's section, the *fallback reasons* table in §2, and *Limits*. Every step
  that changes what is drawn updates them.
- **Source style.** Integer arithmetic for geometry (eighths of a pixel, see
  `src/mermaid_shape.h`), comments that say why, and no floating point in
  anything that reaches the PNG. Fallback reasons are stable strings, listed in
  MERMAID.md.
- **Oracle first.** When this plan does not say how Mermaid treats an input,
  check with `mmdc` before writing code, and say in the report what you
  checked.

---

## Step 1 — Shared painter: strokes and text to a `Diagram`

**Status:** done in the commit that adds this line.

**Assumes:** nothing.

**Why:** a sequence diagram is drawn with what flowcharts already have: boxes
(`Stroke::Outline`, `Shape::Rect`), solid and dotted segments, arrowheads, cross
ends, fills and text in cells. The layout, however, is not ranks. Today
`drawDiagram` (`src/mermaid_draw.cpp`) builds its text layer and fills straight
from `Graph` and `Layout`, and `rasterize` takes a `const Graph&`. This step pulls
out a painter that needs neither.

**Do:**

1. Add a `Scene` to `src/mermaid_model.h`, with:
   - `cols`, `rows` and `title` (drawn in bold and centred, as now);
   - `std::vector<Stroke> strokes`;
   - fills, each a `CellBox` and a `0xRRGGBB` colour;
   - text items, each a `CellBox`, its lines, a `Style` (for `color`), a bold
     flag and an alignment (centred, or left as cluster titles are).
2. Add `bool paintScene(const Scene&, const Env&, Diagram&, std::string& reason)`.
   It rasterizes the strokes, applies the fills, encodes the PNG and builds
   `textRows`. Whatever `rasterize` needs from `Graph` (the shaped outlines it
   meets edges against) must come from the strokes themselves. An `Outline` with
   `node == -1` and `cluster == -1` is a free-standing box.
3. `drawDiagram` keeps its validation, then builds a `Scene` and calls
   `paintScene`.

**Success criteria:**

- [x] `make check` passes, and **every existing golden is byte-identical**
      (`git status tests/` shows no change after `UPDATE=1 tests/mermaid-kitty.sh`).
- [x] `paintScene` and `rasterize` do not use `Graph` or `Layout` (check with
      grep).
- [x] `tools/mermaid-sweep.sh` still reports exactly 10 OK.
- [x] No new fixture and no doc change, since behaviour is unchanged.

---

## Step 2 — Sequence I: parser, model, participants and messages

**Status:** done in the commit that adds this line.

**Assumes:** Step 1.

**Start from:** `render()` in `src/mermaid.cpp`, `parseHeader` in
`src/mermaid_parse.cpp`, and the label helpers there (`plainLabel`, `entityAt`,
`lineBreakAt`).

**Files:** new `src/mermaid_sequence.cpp`, with its model in
`src/mermaid_model.h` (or a new `src/mermaid_sequence.h`). Add it to the
`Makefile`. `render()` dispatches on the header: the first statement after front
matter and `%%` comments is `sequenceDiagram`. `parseFlowchart` keeps declining
everything else with `not a flowchart`. A sequence diagram that fails gives a
reason starting with `sequence:` (for example `sequence: unsupported syntax: box`).

**Mermaid's rules** *(verified with mmdc 11.15.0)*:

| Source | Meaning |
| ------ | ------- |
| `participant A`, `participant A as Long name` | a participant, with an optional display label. Order of first mention is left to right |
| `Task Shut Down ->> Hang Analyzer: text` | implicit participants. **Names may contain spaces**, and are case-sensitive: `Generate task script` and `Generate Task Script` are two participants |
| `title: IDL Flow` in the body (verify whether `title IDL Flow` without the colon is accepted too) | the diagram's title |
| `->>` / `-->>` | solid / dotted line, arrowhead |
| `->` / `-->` | solid / dotted line, no head |
| `-x` / `--x` | solid / dotted line, cross at the end |
| `-)` / `--)` | solid / dotted line, async head (draw it as the arrowhead, and note this in *Limits*) |
| `A->>+B: t`, `B-->>-A: t` | `+` activates the target, `-` deactivates the source. Step 3 draws this; in this step, parse and ignore it |
| `<br>`, `<br/>` in message text | a line break |
| keywords `note`, `Note`, `loop`, `end`, … | case-insensitive |

Participant boxes are drawn **at the top and at the bottom** (Mermaid's default,
`mirrorActors: true`).

**Model:** participants (id, `Label`), and a flat list of events in source order.
Each event is a message (from, to, line, end, activate/deactivate flags, label).
Steps 3 and 4 add notes and block open/divider/close events to the same list.
Parse all of these now (notes, `loop`/`alt`/`else`/`opt`/`par`/`and`/`critical`/
`option`/`break`/`end`, self-messages, activations) so that the grammar is
complete and tested. Decline them in the drawing with `sequence: not yet: note`,
`sequence: not yet: block` and `sequence: not yet: self-message`, which Steps 3
and 4 remove.

**Decline** (`sequence: unsupported syntax: <keyword>`): `actor`, `box`,
`create`, `destroy`, `autonumber`, `rect`, `link`, `links`, `properties`,
`details`, `<<->>`, and anything unparsed. Gates in `tune::`: at most 12
participants and 150 events. Above them, decline as `sequence: too large`
(`Decline::TooComplex`).

**Layout and drawing:**

- **Rows.** Participant boxes are `kNodeFrame` + label lines tall, in a top row
  band and a mirrored bottom band. Each message takes its label's lines, then
  one row for the arrow, then one blank row. Bands and events are separated by
  one blank row.
- **Columns.** Each participant's lifeline runs down the middle of its column,
  in the cell centre (odd hu). A box is its label plus `kNodePad`. Centres are
  placed left to right subject to two kinds of constraint:
  - Neighbouring boxes need half their widths plus `kSepTd` between them.
  - A message between participants i < j needs `centre[j] − centre[i] ≥ label
    width + 2`.

  Satisfy these with integer relaxation: take constraints in order of span, and
  spread any deficit evenly over the gaps the constraint covers.
- **Labels.** A message label is centred between its two lifelines, on the rows
  above its arrow. A lifeline that passes behind a label's text is broken for
  those rows; use segments that stop short, as `reach` does. An arrow runs from
  lifeline to lifeline and crosses the lifelines between them unbroken.
- **Lifelines** are `Line::Dotted` segments from the bottom of the top box to
  the top of the bottom box.
- Build a `Scene` and call `paintScene`. Validate before painting: no two text
  items overlap, and no stroke other than the arrow below it passes through a
  label's cells.

**Tests:** fixture `tests/mermaid-15-sequence.md`, at 80 columns. It covers:

- every arrow kind;
- implicit participants with spaces, and the two case-variant names above;
- an alias;
- a title;
- a three-line `<br/>` label;
- a message that spans three lifelines.

Its declined part has one case per decline reason added in this step.

**Success criteria:**

- [x] `make check` passes. The flowchart goldens are unchanged, and the new
      goldens are judged in the pixel preview.
- [x] The sweep reports **12 OK**: the 10 before plus `hang_analyzer/README.md:32`
      and `:98`. The other nine sequence diagrams decline with a `sequence: not
      yet: …` reason, not `not a flowchart`.
- [x] `hang_analyzer:98` draws **5** participants, the two case variants
      included. This matches mmdc.
- [x] Every new decline reason is in MERMAID.md §2. A new section *Sequence
      diagrams* shows the fixture's main example. *Limits* lists the unsupported
      keywords.
- [x] Diagrams that the builtin declines still reach `mmdc` under `--mermaid
      auto`. Check this with `hang_analyzer`'s neighbour `idl.md:904` and mmdc
      on PATH.

---

## Step 3 — Sequence II: self-messages, notes, activations

**Status:** done in the commit that adds this line.

**Assumes:** Step 2.

**Mermaid's rules** (syntax verified with mmdc; check the drawing details there too):

- `A->>A: t` and `A-->A: t` are self-messages. Mermaid draws a loop that leaves
  the lifeline to the right and comes back below, with the label above it.
- `note over A: t`, `note over A,B: t`, `note left of A: t` and `note right of A: t`
  are notes. Mermaid draws a filled box: over one lifeline it is centred on it;
  over two it spans both lifelines and a margin; left or right, it sits beside
  the lifeline. Note text uses `<br/>` line breaks, as messages do.
- `+`/`-` on a message start/end an activation: a narrow box on the target's
  (source's) lifeline from that message's row to the deactivating one's.
  Activations can nest, and each level is offset a little to the right.

**Drawing:**

- **Self-message.** Draw it as three segments: right from the lifeline by 2
  columns, down one row, and back with the arrowhead or end on the lifeline. The
  label goes to the right of the loop, on the rows above it. Add the
  column constraint `centre[i+1] − centre[i] ≥ loop + label + 1`. For the last
  participant, the label widens the picture.
- **Note.** A `Shape::Rect` outline with a fill: pale yellow (Mermaid's
  `#fff5ad`) in the light theme, and a muted equivalent in the dark theme. Pick
  the dark one so that the text keeps contrast, and say what you picked. The
  label is inside, with one column of padding. A lifeline the note covers stops
  at its border. `left of`/`right of` add column constraints against the
  neighbouring lifeline, or widen the picture at the edges, as
  `README_migrations.md:9`'s long `note right of R` does.
- **Activation.** A box half a cell wide, centred on the lifeline. Each nesting
  level shifts it a quarter cell right. It is drawn with the participant's
  stroke and a fill of the background colour, so that the lifeline does not show
  through. An arrow into or out of an activated lifeline ends at the box's edge.
  An activation still open at the end closes at the last event. A `-` with no
  open activation is ignored, as Mermaid does *(verify)*.

**Tests:** add to `tests/mermaid-15-sequence.md`, or a new
`tests/mermaid-16-sequence-notes.md`:

- self-messages in all arrow styles;
- notes in all four placements, including a spanning one and a long multi-line
  one at the right edge;
- nested activations (`A->>+B`, `B->>+B`, `B-->>-B`, `B-->>-A`).

**Success criteria:**

- [x] `make check` passes, and the goldens are judged.
- [x] The sweep reports **16 OK**. The newly drawn are `idl.md:904`,
      `packaging.md:3`, `server_parameters.md:420` and
      `README_versioning_protocols.md:19`. The remaining five sequence diagrams
      decline only with `sequence: not yet: block`.
- [x] `packaging.md:3` (7 participants, self-messages) is laid out with no
      overlap at `--width 120`. Report its width at 80 and 120 columns. Fitting
      it into 80 is Step 5's job.
- [x] In the pixel preview, no lifeline is visible through a note or an
      activation box.
- [x] MERMAID.md *Sequence diagrams* shows a note and an activation.

---

## Step 4 — Sequence III: loop, alt, opt, par, critical, break

**Status:** done in the commit that adds this line.

**Assumes:** Step 3.

**Mermaid's rules** *(verified with the corpus diagrams; check `alt`/`else`
with mmdc)*:

- `loop text … end`, `opt text … end` and `break text … end` are frames.
- `alt text … else text … end`, `par text … and text … end` and
  `critical text … option text … end` are frames with sections.
- A frame is a rectangle around the events inside it. It is as wide as the
  leftmost and rightmost lifelines those events touch, plus a margin. In the
  top-left corner is a small tab with the keyword. The block's text appears in
  brackets beside the tab: `[retry]`. Section dividers are dashed lines across
  the frame, each with its own `[text]`.
- Frames nest: `README_migrations.md:9` has `par` > `loop`, two deep, and
  `README_ddl_operations.md:13` has `loop` > `loop`.

**Drawing:**

- **Frame.** A `Shape::Rect` outline (or `Stroke::Segment`s if a corner needs
  to differ) on the cells around its events. A nested frame is one column
  inside its parent on each side, and one row below its parent's header. Each
  depth adds a column constraint on the leftmost and rightmost lifelines (or
  widens the picture at the edges).
- **Header.** One row. The keyword is in bold, in a tab outline closed by a
  short vertical stroke with a diagonal corner, as Mermaid's pentagon is; a
  plain box is acceptable if the corner reads badly at cell resolution. Then
  `[text]`, left-aligned. The text wraps inside the frame's width.
- **Dividers.** A divider is a `Line::Dotted` segment across the frame, with
  `[text]` on its own row just below it, left-aligned.
- Validation: frames nest properly, no event falls outside its frame, and frame
  text does not overlap message labels.

**Tests:** a fixture `tests/mermaid-17-sequence-blocks.md` with:

- each keyword once;
- `alt` with two `else` sections;
- `par` with `and`;
- a loop inside a `par` section;
- a frame whose events touch only the middle two of four participants;
- a frame holding a note.

**Success criteria:**

- [x] `make check` passes, and the goldens are judged.
- [x] The sweep reports **21 OK**: all 11 sequence diagrams draw. **No reason
      starting with `sequence: not yet` remains** in the code (check with grep).
- [x] `README_migrations.md:9` and `README_ddl_operations.md:13` show their
      nested frames with distinct borders in the pixel preview. Attach the
      flattened PNGs to the report.
- [x] MERMAID.md *Sequence diagrams* shows a nested block, and the fallback
      table no longer lists the `not yet` reasons.

---

## Step 5 — Sequence IV: fitting the width, corpus, docs, README

**Assumes:** Step 4.

**Do:**

1. **Fit.** Give sequence diagrams the same attempts loop as flowcharts
   (`attempts()` in `src/mermaid.cpp`), in this order:
   - the usual spacing;
   - tight spacing (`kSepTd − 1`, `kNodePad − 2`);
   - tight spacing with message, note, frame and participant labels wrapped at
     24, 16 and 12 columns.

   Take the first attempt that fits `availCols`. If none fits, draw the
   narrowest, wider than `availCols`. A single word wider than `kMaxLabelWord`
   still declines.
2. **Corpus.** Render the 11 sequence diagrams at 80 and 120 columns, and look
   at each in the pixel preview. Then hand the user one command per diagram to
   view it in a real Kitty terminal, for example:

   ```sh
   cd ~/mongo/docs && /path/to/worktree/mdcat packaging.md
   ```

   Fix what reads badly, within this step if small; otherwise record it in
   *Limits*.
3. **Docs.**
   - MERMAID.md: finish *Sequence diagrams* (supported syntax, what is drawn
     how, the gates), update §2 and *Limits*, and update the intro, which says
     only flowcharts are drawn.
   - `docs/adr/0004-builtin-mermaid-kitty.md`: an addendum saying sequence
     diagrams are now built in, and why: they are 11 of 38 MongoDB diagrams.
   - README.md *Mermaid diagrams*: add a short sequence diagram with two or three
     participants, a note and a loop. `CLAUDE.md` requires this demo.

**Success criteria:**

- [ ] `make check` passes.
- [ ] At `--width 80`, **at least 9 of the 11** corpus sequence diagrams fit in
      80 columns. At `--width 120`, **all 11** fit in 120. Report each one's
      width at both.
- [ ] The sweep reports **21 OK at 80 columns** and 21 at 120
      (`tools/mermaid-sweep.sh "$PWD/mdcat" ~/mongo 120`).
- [ ] `./mdcat README.md` in a Kitty terminal shows the new sequence demo drawn
      (`--mermaid builtin`, no decline on stderr with `MDCAT_MERMAID_DEBUG=1`).
- [ ] The user has confirmed the corpus commands look right in a real terminal,
      or any objections are recorded as follow-ups.

---

## Step 6 — Shapes V: document shapes (`doc`, `lin-doc`, `docs`)

**Assumes:** nothing (independent of Steps 1–5).

**Start from:** `shapeNames()` in `src/mermaid_parse.cpp`, `enum class Shape` in
`src/mermaid_model.h`, `src/mermaid_shape.h` (outline geometry and where edges
meet it), and Step 2 of `plan-mermaid-subgraphs.md` as the pattern for adding a
shape.

**Shapes** (verify each outline with mmdc before drawing):

| Names | Mermaid's outline | Notes |
| ----- | ----------------- | ----- |
| `doc`, `document` | a rectangle whose bottom edge is one wave | the label sits above the wave. An edge entering from below meets the wave, not the box's bottom row |
| `lin-doc`, `lined-document` | `doc` with a vertical line near its left side, as `lin-rect` has | |
| `docs`, `documents`, `st-doc`, `stacked-document` | three `doc` outlines stacked, each offset up and to the right; only the front one is whole | the footprint grows by the offset (about a quarter cell each). Edges meet the front outline |

The wave is a curve in eighths of a pixel, with integer arithmetic only, like the
cylinder's arcs. One period across the width, with an amplitude of about a
third of a cell's height.

**Tests:** extend `tests/mermaid-13-shapes-3.md`, or add
`tests/mermaid-18-shapes-5.md`. Each shape appears in TD and LR, with an edge in
and out on each side.

**Success criteria:**

- [ ] `make check` passes. The new goldens are judged in the pixel preview, and
      edge arrowheads touch the wave, with no gap or overlap.
- [ ] The sweep reports **+3 OK**: `README_QO.md:27`, `README_explain.md:549`
      and `s/query/planner/README.md:50`. `matcher:341` now declines with
      `subgraph layout: edge` instead of a shape.
- [ ] MERMAID.md §10 lists and shows the three shapes. The *Out of scope* entry
      in `plan-mermaid-flowcharts.md` is updated.

---

## Step 7 — Shapes VI: `procs`, `tag-rect`, `win-pane`, `extract`

**Assumes:** Step 6 (for the corpus counts; the code is independent).

**Shapes** (verify each with mmdc):

| Names | Mermaid's outline | Notes |
| ----- | ----------------- | ----- |
| `procs`, `processes`, `st-rect`, `stacked-rectangle` | three rectangles stacked, each offset up and to the right | as `docs`, with straight edges |
| `tag-rect`, `tag-proc`, `tagged-rectangle`, `tagged-process` | a rectangle with a small folded triangle in its bottom-right corner | the edge meet is the plain box |
| `win-pane`, `internal-storage`, `window-pane` | a rectangle with one horizontal line near the top and one vertical line near the left | the label sits in the lower-right pane |
| `extract`, `tri`, `triangle` | a triangle, point up | the label sits in the lower half. Edges meet the slanted sides as they meet a rhombus's. The footprint is wide enough that the label fits inside the triangle |

**Tests:** in the Step 6 fixture, or `tests/mermaid-19-shapes-6.md`. Each shape
appears in TD and LR, `extract` with edges entering from above, below and each
side.

**Success criteria:**

- [ ] `make check` passes, and the goldens are judged.
- [ ] The sweep reports **+5 OK**: `query_cmd/README.md:121` and `:221`,
      `pipeline/README.md:173`, and `plan_enumerator/README.md:12` and `:516`.
      **No `unsupported shape` reason remains** in the sweep. `query_cmd:183`
      declines with `label too wide: 45 > 40`, and
      `classic_runtime_planner:157` with `subgraph layout: edge`.
- [ ] MERMAID.md §10 lists and shows the four shapes.

---

## Step 8 — HTML entities in labels

**Assumes:** nothing.

**Start from:** `entityAt` and `plainLabel` in `src/mermaid_parse.cpp`. Today
only Mermaid's own form `#lt;` is decoded.

**Verify with mmdc** before coding:

- `A["x &lt; y"]`, `A -- "a&lt;b&gt;" --> B`, `&amp;`, `&#60;` and `&#x3C;` in
  node labels, edge text and subgraph titles;
- whether an unknown `&foo;` stays literal;
- whether a decoded `<` can start a tag (it must not: it is text);
- whether Markdown strings (`` "`…`" ``) decode entities the same way.

**Do:** decode `&name;`, `&#NN;` and `&#xHH;` with the same table as `#name;`,
wherever `#name;` is decoded now. Step 2's sequence labels use the same helper,
so they get this for free. Width is measured after decoding.

**Tests:** extend `tests/mermaid-9-label-text.md` with one case per form above,
and an unknown entity.

**Success criteria:**

- [ ] `make check` passes. Only the label-text goldens change, and they are
      judged.
- [ ] `query_cmd/README.md:183` draws (after Step 7; before Step 7 it moves to
      `unsupported shape: extract`). Its edge label reads
      `DocumentSource<Stage>::createFromBson()`.
- [ ] MERMAID.md §9 documents both entity forms.

---

## Step 9 — Subgraph border nudge

**Assumes:** Step 7 (both diagrams need its shapes).

**Background:** after layout, `layoutGraph` (`src/mermaid_layout.cpp`, the
`alignedWithBorder` block near the end) moves a subgraph's box one cell across
the flow whenever an edge runs level with its border. It retries at most 3
times per cluster (`nudge[c] >= 3`), then declines with `subgraph layout: edge`.
In a scratch build with the limit raised to 12, `matcher:341` drew and
`classic_runtime_planner:157` still declined.

**Do:**

1. Find out why `classic_runtime_planner:157` never clears. Candidates:
   - two clusters whose nudges undo each other;
   - a nudge that makes a new edge level with the border;
   - an edge that runs level with the border whichever side the box is on.

   Write the finding down in the report.
2. Fix it so that both draw. Options:
   - a higher limit, as a named `tune::` constant with its calibration note;
   - nudging the other way when one way fails;
   - when no box position clears, moving the edge's track instead of the box.
3. Keep the decline for a case that truly cannot clear. Add a minimal fixture
   for each new behaviour.

**Tests:** cases in `tests/mermaid-14-subgraphs.md` that reproduce both corpus
patterns in a few lines.

**Success criteria:**

- [ ] `make check` passes. Any changed subgraph golden is judged, and listed
      with the reason.
- [ ] The sweep reports **+2 OK**: `matcher/README.md:341` and
      `classic_runtime_planner/README.md:157`, both with no edge level with a
      border in the pixel preview. Fallback, if `classic_runtime_planner:157`
      proves out of scope: `matcher:341` draws, and the root cause of the other
      is in MERMAID.md *Limits* with a minimal reproduction.
- [ ] The layout remains linear-ish: a 60-node subgraph stress case runs in
      under a second (time it).

---

## Step 10 — Corpus, calibration, README for the flowchart steps

**Assumes:** Steps 6–9.

**Do:**

1. Run the sweep at 80 and 120 columns. Look at every diagram newly drawn by
   Steps 6–9 in the pixel preview, and give the user one command per diagram for
   a real Kitty terminal.
2. Tune only what the viewing shows to be wrong, recording any value change and
   why next to the constant.
3. README.md *Mermaid diagrams*: use one document shape and `extract` (or
   `procs`) in the existing flowchart demo, keeping it short.
4. Replace the *Outcome and remaining gaps* of `plan-mermaid-subgraphs.md` with a
   pointer here, and write this plan's *Outcome* below.

**Success criteria:**

- [ ] `make check` passes.
- [ ] The sweep at 80 columns reports **32 OK** (or 31 under Step 9's fallback),
      and the only reason left is `not a flowchart`, for the 5 class and 1 C4
      diagrams.
- [ ] `./mdcat README.md` in Kitty shows the new shapes in the demo.
- [ ] The user has viewed the newly drawn diagrams, or objections are recorded.

---

## Outcome

*(Filled in by Step 10.)*
