# Mermaid node shapes

On a Kitty terminal, the built-in renderer draws each node's outline as part of the
diagram's image, and its label as ordinary text over it. Besides the box, `[text]`,
it draws the rounded box `(text)`, the stadium `([text])`, the circle `((text))`, the
double circle `(((text)))`, the subroutine `[[text]]`, the cylinder `[(text)]`, the
rhombus `{text}` and the hexagon `{{text}}`, and the slanted shapes: the parallelograms
`[/text/]` and `[\text\]`, the trapezoids `[/text\]` and `[\text/]`, and the flag
`>text]`. The same shapes can be named with `A@{ shape: name, label: "text" }`. The
curves and slants are drawn exactly,
so an arrowhead touches them exactly where an edge meets them, even off the middle
of a side. Each shape takes the room its outline needs to clear the label: the
stadium, subroutine and hexagon are 2 columns wider than a box, the cylinder is a row
taller (its label sits under the lid) and at least as wide as an 8-column label, so
that its lid looks equally deep whatever the label, the rhombus has the label's rows
+ 4 rows and
about twice its width, a circle is widened until the label's box fits inside, a
parallelogram or trapezoid is 2 columns wider than a box with its sides slanted by a
column, and the flag is a column wider, with a notch a column deep in its left side.
Checked by `mermaid-kitty.sh` at width 80, with `--img kitty` and pinned cell sizes.

## Every shape, top down

Expected: seven shapes in a column, joined by arrows that end exactly on each
outline: a box, a rounded box with corners of half a row, a stadium with semicircular
ends, a circle (really an ellipse), a double circle with two outlines a few pixels
apart, a subroutine with a vertical line half a cell inside each side, and a cylinder
with a lid. The label of each is centred; the cylinder's is below its lid.

```mermaid
graph TD
    a[Box] --> b(Rounded)
    b --> c([Stadium])
    c --> d((Circle))
    d --> e(((Double)))
    e --> f[[Subroutine]]
    f --> g[(Cylinder)]
```

## Every shape, left to right

Expected: the same shapes in a row, plus the rhombus and the hexagon, which join
them in two rows. Arrows meet the outlines at their left and right points.

```mermaid
graph LR
    a(Rnd) --> b([Std]) --> c((Cir)) --> d(((Dbl)))
    d --> e[[Sub]] --> f[(DB)]
    a --> g{Decide}
    g --> h{{Hex}}
```

## A decision

Expected: a start and an end that are stadiums, and a rhombus whose two labelled
edges leave its bottom point on one bus, as they do from a box. The rhombus is
five rows tall and its label fits well inside it. The edge into the rhombus is
only a little longer than an edge between boxes, although the labelled edges
double the ranks. Below the rhombus, the cylinder and the circle sit in one rank
with their labels on one row, so the circle starts a row lower than a box would.

```mermaid
graph TD
    start([Start]) --> check{Valid?}
    check -->|yes| save[(Database)]
    check -->|no| fail((Error))
    save --> done([Done])
    fail --> done
```

## Several lines

Expected: a label of several lines grows each shape the way it grows a box. The
circle and the rhombus widen until the lines fit inside, and the cylinder gets a
row for each line below its lid.

```mermaid
graph TD
    a(["Two<br>lines"]) --> b{"Three<br>lines<br>here"}
    b --> c(("A circle<br>of text"))
    b --> d[("Stored<br>data")]
    c --> e{{"Hex<br>agon"}}
    d --> e
```

## Several edges at one node

Expected: edges leave and arrive off the middle of a side, at points that depend on
the shape: on the lid of the cylinder, the ellipse, the slanted sides of the
rhombus and the hexagon. Each arrowhead still touches its outline, and every stroke
that leaves a curve starts on it.

```mermaid
graph TD
    src[(Source)] --> lex((Lexer))
    src --> doc{Document}
    cfg([Config]) --> lex
    cfg --> doc
    ext{{Extensions}} --> lex
```

## Other directions

Expected: `BT` and `RL` keep every shape the right way up: the cylinder's lid stays
on top, with its label below it, in both. In the first `BT` diagram the labels of
the cylinder and the circle are still on one row. Left to right (above), the arrow
into the cylinder is level with its label, so it does not jog.

```mermaid
graph BT
    a([Go]) --> b{Ok?}
    b -->|yes| c[(Store)]
    b -->|no| d((Err))
    c --> e[End]
    d --> e
```

```mermaid
graph BT
    a[(Data)] --> b{Check}
    b -.-> c(((Done)))
    b ==> d[[Review]]
```

```mermaid
graph RL
    a[(Data)] --> b{{Parse}}
    b --> c((Tree)) --> d([Output])
```

## Redefining a shape

Expected: a later definition of a node replaces its shape and label: `a` is a
stadium labelled "Second", not a box labelled "First".

```mermaid
graph LR
    a[First] --> b((Circle))
    a([Second]) --> c[Plain]
    b --> c
```

## Quotes and Markdown

Expected: a quoted label may hold the brackets that make the shapes, and a Markdown
string works in any shape.

```mermaid
graph LR
    a(["a (b) [c]"]) --> b{{"`**Bold** hex`"}}
    b --> c[("{x} | y")]
```

## Slanted shapes, top down

Expected: five shapes in a column. The parallelogram `[/…/]` leans right, with its top
edge shifted right of its bottom edge; `[\…\]` leans left. The trapezoid `[/…\]` is
wider at the bottom and `[\…/]` is wider at the top; their sides slant a column. The
flag `>…]` is a box with a notch in its left side, pointing in to the middle of the
label's row. Each arrowhead touches the slanted side or the notch where its edge
meets it, even off the middle of a side.

```mermaid
graph TD
    a[/Lean right/] --> b[\Lean left\]
    b --> c[/Trapezoid\]
    c --> d[\Inverted/]
    d --> e>Odd flag]
```

## Slanted shapes, left to right

Expected: the same shapes in a row. Arrows meet the slanted sides at their middle
and the flag at the tip of its notch.

```mermaid
graph LR
    a[/In/] --> b[\Out\] --> c[/Wide\] --> d[\Inv/] --> e>Flag]
```

## Slanted shapes with several edges

Expected: edges leave and arrive off the middle of a side. Each stroke that leaves a
slanted side starts on it, and each arrowhead ends on it. A label of several lines
grows the shape a row for each line without changing its slant.

```mermaid
graph TD
    src[/Source/] --> lex[\Lexer\]
    src --> doc>"Two<br>lines"]
    cfg[/Config\] --> lex
    cfg --> doc
    lex --> out[\Output/]
    doc --> out
```

## Other directions

Expected: `BT` and `RL` keep the slants the way they are written: a `[/…/]` leans
right in every direction.

```mermaid
graph BT
    a[/Go/] --> b[\Check/]
    b -->|yes| c>Flag]
    b -->|no| d[/Wait\]
```

```mermaid
graph RL
    a>Flag] --> b[/Lean/] --> c[\Trap/]
```

## The `@{ shape }` form

Expected: each node is the shape its name says: `rect` (and its alias `process`), `rounded`,
`stadium`, `circle`, `dbl-circ`, `fr-rect`, `cyl`, `diam`, `hex`, `lean-r`, `lean-l`,
`trap-b`, `trap-t` and `odd`, and the aliases that Mermaid gives them (`event`, `pill`,
`db`, `decision`, `prepare`, `in-out`, `out-in`, `priority`, `manual`). A node with
no label is labelled with its id, and a label may be quoted, hold syntax characters
and be a Markdown string. Other keys are ignored.

```mermaid
graph LR
    a@{ shape: rect, label: "Rect" } --> b@{ shape: rounded, label: "Round" }
    b --> c@{ shape: stadium, label: "Stad" }
    c --> d@{ shape: circle, label: "Circ" }
```

```mermaid
graph LR
    e@{ shape: dbl-circ, label: "Dbl" } --> f@{ shape: fr-rect, label: "Frame" }
    f --> g@{ shape: cyl, label: "Cyl" }
```

```mermaid
graph TD
    a@{ shape: diam, label: "Diamond" } --> b@{ shape: hex, label: "Hex" }
    b --> c@{ shape: lean-r, label: "Lean right" }
    c --> d@{ shape: lean-l, label: "Lean left" }
    d --> e@{ shape: trap-b, label: "Trap bottom" }
    e --> f@{ shape: trap-t, label: "Trap top" }
    f --> g@{ shape: odd, label: "Odd" }
```

```mermaid
flowchart LR
    A@{ shape: db, label: "Store" } --> B@{ shape: decision, label: "Ok?" }
    B -->|yes| C@{ shape: in-out, label: "Read #35; in" }
    B -->|no| D@{ shape: manual, icon: "fa:user", label: "`**Fix** it`" }
    plain@{ shape: process } --> C
    C --> E@{ shape: event }
    E@{ shape: pill, label: "Done" }
```

## Declined

Expected: shown as source, as the `.err` golden says, in this order. A shape that is
never closed, a slanted shape that is never closed, and `@{ shape }` with a name that
is not drawn here.

```mermaid
graph TD
    a((Open) --> b[B]
```

```mermaid
graph TD
    a[/Open/ --> b[B]
```

```mermaid
graph TD
    a@{ shape: doc, label: "Document" } --> b[B]
```
