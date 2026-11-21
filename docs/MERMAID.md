# Mermaid diagrams

A fenced code block tagged `mermaid` is drawn as a diagram. This is the reference
for what mdcat draws itself, what it hands to `mmdc`, and what it leaves as source.
It follows the order in which the features were built, from the basics to the
advanced, which is also the order of the fixtures `tests/mermaid-1-basic.md` to
`tests/mermaid-12-fit.md` (see [tests/README.md](../tests/README.md)). Render this
file with `mdcat` to see every example drawn; the design is in
[ADR 0004](adr/0004-builtin-mermaid-kitty.md) and
[the implementation plan](plan-mermaid-flowcharts.md).

- [How a diagram is rendered](#how-a-diagram-is-rendered)
- [1. Basics](#1-basics)
- [2. Diagrams that are not drawn here](#2-diagrams-that-are-not-drawn-here)
- [3. Branches and merges](#3-branches-and-merges)
- [4. Tracks and bridges](#4-tracks-and-bridges)
- [5. Ordering the boxes](#5-ordering-the-boxes)
- [6. Syntax](#6-syntax)
- [7. Link styles](#7-link-styles)
- [8. Edge labels](#8-edge-labels)
- [9. Label text](#9-label-text)
- [10. Node shapes](#10-node-shapes)
- [11. Cycles, self-loops and parallel edges](#11-cycles-self-loops-and-parallel-edges)
- [12. Fitting the width](#12-fitting-the-width)
- [Limits](#limits)

## How a diagram is rendered

There are three renderers, tried in this order (`--mermaid auto`, the default):

1. **Built-in.** On a terminal with the Kitty graphics protocol, mdcat draws the
   flowcharts it understands itself, without launching anything. The shapes and
   arrows are one transparent PNG that mdcat rasterizes; the labels are ordinary
   text in the character cells over it. The text keeps the terminal's font and
   colours, can be selected and copied, and is found by `gmore`'s search.
2. **`mmdc`.** Any other diagram, or a flowchart the built-in renderer declines,
   is rendered as a real image by the
   [mermaid CLI](https://github.com/mermaid-js/mermaid-cli) when it is installed
   and the terminal supports graphics (Kitty or sixel).
3. **Source.** Otherwise the block is shown as an ordinary code panel, so the
   document is never worse off than plain text.

Choose a renderer with `--mermaid auto|builtin|mmdc|source` or `MDCAT_MERMAID`; the
flag wins over the variable.

| Mode      | Built-in | `mmdc` | Source |
| --------- | :------: | :----: | :----: |
| `auto`    | first    | second | last   |
| `builtin` | yes      | never  | when declined |
| `mmdc`    | no       | yes    | when `mmdc` can't draw it |
| `source`  | no       | no     | always |

`MDCAT_MERMAID_DEBUG=1` prints a line on stderr whenever the built-in renderer
declines a diagram, giving the file, the line of the code block, and the reason:

```
mdcat: README.md:212: mermaid: fallback: unsupported syntax: subgraph one
```

A diagram taller than the terminal is not shrunk: `gmore` scrolls through it like
any other content, revealing a row at a time.

## 1. Basics

A diagram starts with `graph` or `flowchart` and a direction, followed by
statements. `a --> b` declares two boxes, labelled with their ids, and an arrow.
`a[Text]` gives a box a label. A box that appears again keeps its label, and the
last explicit text wins. Boxes are placed in ranks: a box goes one rank after the
boxes that point at it.

```mermaid
graph LR
    parse[Parse] --> render[Render]
    render --> terminal[Terminal]
    render --> pager[Pager]
```

The picture's width and height follow from the labels. Each box is its label plus
two columns of padding on each side and a row above and below; ranks are a few
cells apart. A diagram too wide for the terminal is [fitted](#12-fitting-the-width).

## 2. Diagrams that are not drawn here

The built-in renderer draws flowcharts, and declines anything it does not draw
exactly. A declined diagram is never drawn wrongly; it goes to `mmdc` or to source.
The reasons, as `MDCAT_MERMAID_DEBUG=1` words them:

| Reason | Example |
| ------ | ------- |
| `not a flowchart` | `sequenceDiagram`, `pie`, `gantt`, `classDiagram`, … |
| `unsupported syntax: …` | `subgraph … end`, `direction` inside a graph, `end` as an id, a link with mismatched ends such as `A o--> B`, an unclosed shape or quote |
| `unsupported direction` | `graph XY` |
| `unsupported shape: NAME` | `A@{ shape: doc }` and the other shapes that are not [drawn](#10-node-shapes) |
| `unsupported label: …` | a label that is not valid where it stands, such as `"say "hi""` |
| `unsupported entity: …` | `#nosuchentity;` |
| `html in label` | HTML other than `<br>` |
| `label too wide: N > 24` | an edge label over 24 columns |
| `back edges: N > M`, `crossings: N > M` | too tangled to read, see [Limits](#limits) |
| `too many nodes`, `too many ranks`, `too many ports` | more than 60 boxes or ranks, or an edge fan too wide to give every edge a port |

Most of these go on to `mmdc`. The two tangle limits do not: a diagram that
`mmdc` would only draw as a hairball is shown as source, and `mmdc` is not asked.

## 3. Branches and merges

Where several edges leave one box they share its port and split off a common bus;
where several arrive they merge into one arrowhead. A dot marks each split or
merge, and bends are rounded. Extra dashes make a link longer: `a --> b` puts `b`
one rank after `a`, `a ---> b` two, and `a ----> b` three. A box with no incoming
edge is not forced into the first rank: it is pulled down next to what it points at,
so a late source does not leave a long edge behind it.

```mermaid
graph TD
    root[Root] --> left[Left]
    root --> right[Right]
    left --> join[Join]
    right --> join
    root ---> late[Skips a rank]
```

Ports sit symmetrically on every box, arriving or leaving: an odd number of ports
uses the middle one, an even number leaves it empty, so no edge sits straight ahead
and seems to take priority. A decision (`{…}`) is also the exception to sharing:
each outcome leaves through a port of its own, never a bus. Below, `Ok?` has two
outcomes and `Which?` has three.

```mermaid
graph TD
    ok{Ok?} -->|yes| ship[Ship]
    ok -->|no| fix[Fix]
    pick{Which?} --> a[One]
    pick --> b[Two]
    pick --> c[Three]
```

Separate parts of one diagram, with no edge between them, are drawn side by side
(top down) or one below the other (left to right).

## 4. Tracks and bridges

When edges interleave, each bus gets a **track** of its own between two ranks, and
the gap between the ranks grows by one row (or column) for each track. Where an edge
must cross another, it passes over it on a **bridge**: the stroke breaks, and no
dot is drawn. A dot always means a join, and a bridge never does.

Edges share strokes only in a split from one box or a merge into one box, so a
drawing never suggests a link that isn't there. Below, `Lexer` and `Document` each
merge their own sources, and `Source` and `Config` each leave through two ports, one
per merge, instead of joining buses that would lead from one source up into
another:

```mermaid
graph TD
    src[Source] --> lex[Lexer]
    src --> doc[Document]
    cfg[Config] --> lex
    cfg --> doc
    ext[Extensions] --> lex
```

Complete bipartite graphs such as K₃,₃ (nine edges, eight crossings) are about the
most the width of an 80-column terminal allows; see [Limits](#limits).

## 5. Ordering the boxes

The order in which boxes are declared is a starting point, not a layout. The
renderer sweeps each rank to reorder its boxes so that few edges cross, then places
them across the rank so that long edges run straight through the ranks they pass
and a box sits next to its neighbours. Below, declared in order `C` would come
before `D` and `B`'s edge would cross `A`'s; `D` is put first, so `C` sits between
`A` and `B` and nothing crosses:

```mermaid
graph TD
    A --> C
    A --> D
    B --> C
```

The sweeps minimize crossings; they do not promise the minimum. Whatever remains is
drawn with bridges and counts against the [limit](#limits).

## 6. Syntax

**Directions.** `TB` and `TD` (top down, the default), `BT`, `LR` and `RL`; the
aliases `v` for `TB`, `^` for `BT`, `>` for `LR` and `<` for `RL`; and no
direction at all, which is top down. `flowchart-elk` reads as `flowchart`.
Arrowheads point the true way, and labels are never mirrored.

**Statements.** Separate them with newlines or `;`. A chain, `a --> b --> c`, makes
`a --> b` and `b --> c`. `&` makes every combination: `a --> b & c` and
`a & b --> c & d` (four edges).

**Comments and directives.** `%% a comment`, on its own line or at the end of one,
and `%%{ init: … }%%` directives, over several lines or not, are read and ignored.

**Front matter.** A `title:` is drawn in bold, centred above the diagram. Other
keys, such as `config: theme:`, have no visual effect. The exception is
`config: markdownAutoWrap: false`, see [Label text](#9-label-text).

**Node colours.** `style ids …`, `classDef names …`, `class ids name` and `A:::name`
set a node's `fill` (the box), `stroke` (its outline) and `color` (its label text), as
`#rgb` or `#rrggbb`. A class is applied in the order it was given, then the node's own
`style`, so that wins. Other properties, such as `stroke-width` or `font-family`, are
read and dropped, as are colours in any other form. Edges keep the theme's colour.

**Read but without visual effect.** `linkStyle`, `click`, edge ids (`e1@-->`) and their properties (`e1@{ animate: true }`),
`accTitle` and `accDescr` (also over several lines) are all understood and
dropped.

**Ids** may hold non-ASCII letters, `.` and `-`.

```mermaid
---
title: Right to left, with a chain and a fan-out
---
graph RL
    a[Source] --> b[Build] --> c[Test] %% a chain
    c --> d[Ship] & e[Report]
    style d fill:#bbf
```

Colours from `style`, `classDef` and `class`:

```mermaid
graph TD
    classDef warm fill:#fda,stroke:#333,color:#000
    a[Plan]:::warm --> b[Build]
    b --> c[Ship]
    class b,c cool
    classDef cool fill:#cef,stroke:#333,color:#000
    style c fill:#cec,stroke:#363
```

## 7. Link styles

| Syntax | Drawn as |
| ------ | -------- |
| `-->` | arrowhead |
| `---` | open, no head |
| `-.->`, `-.-` | dotted, with and without a head |
| `==>`, `===` | thick, with and without a head |
| `--o`, `--x` | ends in a circle or a cross |
| `<-->`, `o--o`, `x--x` | markers at both ends |
| `~~~` | nothing, but the boxes are still ranked as if joined |

Length works for every style: one more character is one more rank, so `--->`,
`-..->`, `===>` and `----` (open) span two ranks, and `---->` three. A long
link keeps its dashes or thickness along the whole way. `~~~~` is an invisible link
of two ranks.

Edges of different styles never share a bus: a port holds edges of one style only.
Below, `Build` leaves through one port for its dotted link and another for its
plain one. Invisible links get no port and no stroke at all.

```mermaid
graph LR
    src[Source] ==> build[Build]
    build -.-> docs[Docs]
    build --> test[Test]
    test <--> ci[CI]
    build --x lint[Lint]
```

Dashes restart on every straight stretch of a dotted edge, so a bend never ends in
half a dash, and a dotted edge crosses another on a bridge like any other.

## 8. Edge labels

Edges can carry text: `a -->|yes| b`, `a -- yes --> b`, `a -. yes .-> b`,
`a == yes ==> b`, and the same on open, circle, cross and two-ended links. The
label is ordinary, selectable text that sits on its edge, halfway between the
boxes, and the edge stops just short of it on either side.

The label is a node of its own in the layout. As soon as any edge in a diagram is
labelled, every rank is twice as far apart, so a label always has a rank to sit in,
and a label on a longer link sits in the middle one. Labels do not affect how edges
share ports: edges with different labels, or none, still split from one box and
merge into one on a single bus.

```mermaid
graph TD
    input[Input] --> check{Valid?}
    check -->|yes| build[Build]
    check -->|no| report[Report]
    build == all green ==> ship[Ship]
    report -. later .-> ship
```

A label may be up to 24 columns wide. Anything wider, a label with HTML other than
`<br>`, and a label on an invisible link are [declined](#2-diagrams-that-are-not-drawn-here).

## 9. Label text

Labels follow Mermaid's rules for text.

- **Quoted labels**, `a["x (y) [z]"]`, may hold brackets, parentheses, pipes and
  even `-->`, which are literal text instead of syntax. The quotes are not shown.
  A quote inside quotes is not allowed. Text after the closing quote on the same
  line is accepted, as in `mmdc`.
- **Entity codes**: numeric codes (`#35;`, `#9829;`) and about fifty names (`#quot;`,
  `#amp;`, `#lt;`, `#hearts;`, `#rarr;`, `#copy;`, …) stand for their characters. An
  unknown name is declined.
- **Line breaks**: `<br>`, `<br/>` or `<br />` starts a new line. A node with a
  label of *n* lines is *n* + 2 rows tall, and each line is centred. Other HTML is
  declined.
- **Markdown strings**, ``a["`**bold** and *italic*`"]``, are the one kind of label
  that interprets Markdown: bold, italic, `` `code` `` and a newline as a hard
  break. They wrap at 24 columns. A plain label such as `a*b*c` is drawn exactly as
  written, and so is an entity such as `#35;` inside a Markdown string. Front matter
  `config: markdownAutoWrap: false` turns the wrapping off.
- **Wide characters** are measured by display width: CJK and emoji take two
  columns, and a box is as wide as the text needs.

```mermaid
graph LR
    a["Quoted [brackets] and #quot;entities#quot; #9829;"] --> b["Two<br>lines"]
    b --> c["`**Bold** and *italic*,
    wrapped when it is long`"]
```

## 10. Node shapes

| Syntax | Shape |
| ------ | ----- |
| `[text]` | box |
| `(text)` | rounded box |
| `([text])` | stadium |
| `((text))` | circle |
| `(((text)))` | double circle |
| `[[text]]` | subroutine |
| `[(text)]` | cylinder |
| `{text}` | rhombus |
| `{{text}}` | hexagon |
| `[/text/]`, `[\text\]` | parallelograms |
| `[/text\]`, `[\text/]` | trapezoids |
| `>text]` | flag, with a notch in its left side |

Mermaid's newer form names a shape instead of drawing it, `a@{ shape: db, label:
"Store" }`, and every short name or alias of the shapes above works: `rect`,
`rounded`, `stadium`, `circle`, `dbl-circ`, `fr-rect`, `cyl`, `diam`, `hex`,
`lean-r`, `lean-l`, `trap-b`, `trap-t`, `odd`, `decision`, `database`, and more.
Other keys in the block, such as `icon:`, are ignored. A name for a shape that isn't
drawn here sends the diagram to the fallback.

The curves and slants are drawn exactly, so an arrowhead touches the outline where
the edge meets it, even off the middle of a side. Each shape takes the room its
outline needs to clear the label: a stadium, subroutine and hexagon are two columns
wider than a box; a cylinder is a row taller, its label sits under the lid, and it
is at least as wide as an 8-column label so that the lid is equally deep whatever
the label; a rhombus is four rows taller than its label and about twice as wide; a
circle is widened until the label's box fits inside; and the slanted shapes are two
columns wider, with sides slanted by one column. Quoted and Markdown labels work in
every shape.

```mermaid
graph TD
    read[/Read input/] --> parse{Valid?}
    parse -->|yes| store@{ shape: db, label: "Store" }
    parse -->|no| fail>Report error]
    store --> done(((Done)))
    fail --> done
```

## 11. Cycles, self-loops and parallel edges

A graph may have cycles. The edge that closes a cycle, found by a depth-first search, is a **back edge**: it is drawn against the flow, with an
arrowhead at its real target. It leaves and arrives through ports of its own, a cell
beside the middle, so it never merges with the edges running the other way, and a box
with many ports grows to hold them. Back edges work in all four directions, with
labels and in every style, and a long cycle is drawn the long way round.

A box may point at itself. The **self-loop** is drawn beside the box, to its right
(below it, in a left-to-right diagram), and its label sits beyond it. Of several
self-loops on one box only the last is drawn, as in `mmdc`.

Two edges may join the same pair of boxes: each gets ports of its own and they run
side by side, whether they point the same way or opposite ways. `A <--> B` is one
edge with two heads; `A --> B` and `B --> A` are two.

```mermaid
graph TD
    input[Input] --> check[Valid?]
    check -->|yes| build[Build]
    check -->|no| report[Report]
    build --> ship[Ship]
    report --> ship
    report -->|retry| check
    ship -->|again| ship
```

Here `retry` is the back edge, drawn from `Report` up to `Valid?`; `Ship` points at
itself; and `Valid?` and `Report` are joined in both directions.

## 12. Fitting the width

A diagram wider than the terminal is fitted before anything else, never by changing
its direction. The renderer tries, in turn, and draws the first that fits:

1. the natural spacing;
2. tighter spacing: a column less between siblings or ranks, and one column of
   padding around a label instead of two;
3. labels with spaces wrapped on word boundaries at 24, then 16, then 12 columns.

A single word is never split. If nothing fits, the narrowest attempt is drawn
anyway, wider than the terminal, rather than given up on: widen the window, or
scroll sideways in a pager that doesn't wrap lines. This chain is 98 columns wide at
its natural size, and draws in about 75 when the terminal is 80 wide:

```mermaid
graph LR
    read[Read the input file] --> parse[Parse every line]
    parse --> check[Validate the result]
    check --> write[Write the output]
```

## Limits

| Limit | Value | Reason shown |
| ----- | ----- | ------------ |
| boxes | 60 | `too many nodes` |
| ranks | 60 (long links add some) | `too many ranks` |
| edge label width | 24 columns | `label too wide` |
| back edges | one per ten columns of the terminal's width | `back edges: N > M` |
| crossings (bridges) | one per ten columns of the terminal's width | `crossings: N > M` |

For the last two, a terminal narrower than 72 columns counts as 72 wide, so an
80-column terminal allows 8 of each. Beyond that, a diagram is too tangled to
read: it is shown as source, and `mmdc` is not asked to redraw it.

Not drawn at all: `subgraph`, `direction` inside a graph, and the other diagram types
(sequence, pie, class, state, Gantt, …). Colours, curves and other styling are read
but have no effect. A flowchart that is declined goes to `mmdc` when it is
available.
