# Mermaid subgraphs

`subgraph ... end` is read into the model (`Graph::clusters`, `Node::cluster`) and, so far, drawn only
top-down and one level deep (docs/plan-mermaid-subgraphs.md, Steps 5-8): the box is one outline under
the nodes and edges, around the nodes named inside it, with the subgraph's title, if it has one, in
bold under the top border. The cases whose heading starts with *Drawn* are drawn. The rest are
declined: a nested subgraph or a direction other than `TD` is `subgraph layout` for now, and the
other cases are declined for the reason named in their heading or, for the membership cases, because
of what they hold. `MDCAT_MERMAID_DEBUG=1` prints each reason on stderr, and `mermaid-kitty.sh`
compares it with `expected/mermaid-14-subgraphs.w80.err`. Which node belongs to which subgraph is
checked outside the repository, with a scratch parser.

A blank title (`subgraph s[" "]`) draws no title, so the first cases use it.

## Drawn: one box around a chain

Expected: a box around A, B and C, one blank column inside it on each side. X's edge enters it through
the top border and C's leaves through the bottom border. Y is reached by X's second edge, which goes
round the right of the box.

```mermaid
graph TD
    X --> A
    subgraph s[" "]
        A --> B --> C
    end
    C --> Y
    X --> Y
```

## Drawn: two boxes, both directions and a back edge

Expected: two boxes one above the other. The edge from the first to the second crosses both borders;
the back edge from the second to the first goes round the right of both boxes and enters the first
box through its right border.

```mermaid
graph TD
    subgraph a[" "]
        A1 --> A2
    end
    subgraph b[" "]
        B1 --> B2
    end
    A2 --> B1
    B2 --> A1
    S --> A1
    B2 --> T
```

## Drawn: an edge goes round a box

Expected: X's edge to Y runs down the left of the box, round it, and Z's edge down the right; the edge
into A crosses the top border and D's leaves through the bottom one.

```mermaid
graph TD
    X --> Y
    subgraph s[" "]
        A --> B --> C --> D
    end
    X --> A
    X --> Z
    Z --> Y
    D --> Y
```

## Drawn: an edge between two outsiders, passing a box

Expected: R's edge to D enters the box through its left border, below C, and nothing outside it runs
through the box.

```mermaid
graph TD
    P --> Q
    P --> A
    subgraph s[" "]
        A --> B --> C --> D
    end
    Q --> R
    R --> D
```

## Drawn: a rank of the span that holds no member

Expected: A and D are in the box and X, between them, is not. The box is wide enough to be clear of
X on the rank that holds only X, and the edges between A, X and D cross its left border.

```mermaid
graph TD
    subgraph s[" "]
        A
        D
    end
    A --> X --> D
    X --> Y
```

## Drawn: a long edge, a label and a self-loop inside a box

Expected: everything is inside the box: the edge from A to C runs down its right, "yes" sits between
B and D, and the loop on C is beside C.

```mermaid
graph TD
    subgraph s[" "]
        A --> B --> C
        A --> C
        B -->|yes| D
        C --> C
    end
    S --> A
    D --> T
```

## Drawn: edges into and out of the middle of a box

Expected: X's edge joins the one into B, crossing the left border; B's edge to Z leaves through the
right border.

```mermaid
graph TD
    X --> B
    subgraph s[" "]
        A --> B --> C
    end
    C --> Y
    B --> Z
```

## Drawn: styled members, a lone box and boxes on one rank

Expected: three boxes side by side. P1 is filled red and Q2 green with a green label; the outlines of
the boxes keep the theme's colour.

```mermaid
graph TD
    subgraph p[" "]
        P1 --> P2
    end
    subgraph q[" "]
        Q1 --> Q2
    end
    subgraph r[" "]
        R1
    end
    style P1 fill:#fdd,stroke:#a00
    style Q2 fill:#dfd,stroke:#0a0,color:#060
```

## Drawn: a box inside a longer chain

Expected: the chain A, B passes into the box at C and leaves it after E; the edge from A to G goes
round the right of the box.

```mermaid
graph TD
    A --> B
    B --> C
    subgraph s[" "]
        C --> D
        D --> E
    end
    E --> F
    F --> G
    A --> G
```

## Drawn: MongoDB admission control, titles blanked

The diagram as MongoDB wrote it, with the subgraph titles blanked. It is declined because of its
36-column edge label, as it would be without the subgraphs.

```mermaid
flowchart TB
A(Stable Probe) --> |at minimum and tickets not exhausted|A

A --> |"(above minimum and tickets not exhausted) or at maximum"|C(Probe Down)
subgraph decrease[" "]
C --> |throughput increased|F{{Decrease stable concurrency}}
C --> |throughput did not increase|G{{Go back to stable concurrency}}
end
F --> H
G --> H

A --> |below maximum and tickets exhausted| B(Probe Up)
subgraph increase[" "]
B --> |throughput increased|D{{Increase stable concurrency}}
B --> |throughput did not increase|E{{Go back to stable concurrency}}
end
D --> H(Stable Probe)
E --> H
```

## Drawn: MongoDB admission control, shorter edge labels

The same diagram with its edge labels cut to at most 24 columns. Expected: the two boxes side by
side under Stable Probe, each with a probe and two hexagons, and all four edges merging into the
last Stable Probe below them.

```mermaid
flowchart TB
A(Stable Probe) --> |at minimum|A

A --> |above minimum or at max|C(Probe Down)
subgraph decrease[" "]
C --> |throughput increased|F{{Decrease stable concurrency}}
C --> |no throughput gain|G{{Go back to stable concurrency}}
end
F --> H
G --> H

A --> |below max and exhausted| B(Probe Up)
subgraph increase[" "]
B --> |throughput increased|D{{Increase stable concurrency}}
B --> |no throughput gain|E{{Go back to stable concurrency}}
end
D --> H(Stable Probe)
E --> H
```

## Drawn: a title, centred where no edge crosses it

Expected: a box around A and B with `Plan` in bold on the row under the top border, in the middle of
the box, which is as wide as its members make it: nothing enters the box from above.

```mermaid
graph TD
    subgraph s[Plan]
        A --> B
    end
```

## Drawn: a title beside the edge that enters the box

Expected: X's edge enters the box through the top border and runs down through the box's title row,
so `one` is not centred: it sits beside the edge, a blank column from it, and the box is as wide as
that takes, growing to the right of the members.

```mermaid
graph TD
    X --> A
    subgraph one
        A --> B
    end
    B --> Y
```

## Drawn: every way to write a title

Expected: four boxes in a chain, each titled, in bold, and each entered by an edge from the box above:
a bare id (`S`), `id [Title one]`, `id[Title two]` and `id["Title three"]`. The title of `S` is the
widest thing in its box; the others are beside their edge.

```mermaid
graph TD
    subgraph S
        A1
    end
    subgraph id1 [Title one]
        A2
    end
    subgraph id2[Title two]
        A3
    end
    subgraph id3["Title three"]
        A4
    end
    A1 --> A2 --> A3 --> A4
```

## Drawn: a quoted id and a title with spaces

Expected: the boxes `Q::plan()` and `Index Tagging` (a generated id), one above the other, with the
edge from Q to R between them. The second title sits left of the edge into R, and its box grows to
the right, not to the left.

```mermaid
graph TD
    subgraph "Q::plan()"
        P --> Q
    end
    subgraph Index Tagging
        R --> S
    end
    Q --> R
```

## Drawn: Markdown, `<br>` and `;` in titles

Expected: `Markdown title` with `Markdown` bold on top of the bold of the title (so it reads the same),
`two` over `lines` in a box with a two-line title, and `A & B; C` as written.

```mermaid
graph TD
    subgraph id5["`**Markdown** title`"]
        A8
    end
    subgraph id6["two<br>lines"]
        A9
    end
    subgraph id7[A & B; C]
        A10
    end
    A8 --> A9 --> A10
```

## Drawn: a title wider than its members

Expected: each box is as wide as its title needs plus a blank column and the border on each side.
Both are entered from above, by `X --> A` and `A --> C`, so neither title is centred: each sits to
the right of the entering edge, a blank column from it, and the box grows to the right, leaving its
one member near its left side. The second box has a Markdown title that is wrapped at 24 columns.

```mermaid
graph TD
    subgraph w["A title much wider than its one member"]
        A
    end
    subgraph m["`A *Markdown* title that is a good deal wider than anything inside`"]
        C
    end
    A --> C
    X --> A
```

## Drawn: titles wrapped to fit the width

Expected: three boxes side by side whose plain titles are 30 columns wide or more. At 80 columns they
do not fit with their titles on one line, so the fit attempts wrap the titles at spaces, in three
lines each, and the box widths follow.

```mermaid
graph TD
    X --> A
    X --> B
    X --> C
    subgraph a["Alpha subgraph with a long title"]
        A
    end
    subgraph b["Bravo subgraph with a long title"]
        B
    end
    subgraph c["Charlie subgraph with a long title"]
        C
    end
```

## Drawn: boxes side by side, titles of different heights

Expected: the top borders of the two boxes are on one row. The title of the left one is two lines and
the right one's is one; the right box has a blank row more between its title and its first member.

```mermaid
graph TD
    X --> A
    X --> B
    subgraph l["two<br>lines"]
        A --> C
    end
    subgraph r[one]
        B --> D
    end
```

## Drawn: MongoDB admission control, real titles, shorter edge labels

The diagram with its own subgraph titles (`decrease` and `increase`, the ids) and the edge labels cut
to at most 24 columns. Expected: the two boxes side by side under Stable Probe, each with its title
in bold at the top, and the edges into the probes crossing the top borders.

```mermaid
flowchart TB
A(Stable Probe) --> |at minimum|A

A --> |above minimum or at max|C(Probe Down)
subgraph decrease
C --> |throughput increased|F{{Decrease stable concurrency}}
C --> |no throughput gain|G{{Go back to stable concurrency}}
end
F --> H
G --> H

A --> |below max and exhausted| B(Probe Up)
subgraph increase
B --> |throughput increased|D{{Increase stable concurrency}}
B --> |no throughput gain|E{{Go back to stable concurrency}}
end
D --> H(Stable Probe)
E --> H
```

## Declined: a box that an edge would have to cross

Expected: declined. A box with several members that lie on far apart ranks, with other nodes between
them, cannot always be drawn without an edge running through it: here N1's edge to N6 would cross
the box of N3, N4 and N5.

```mermaid
graph TD
    subgraph c0[" "]
        N3
        N4
        N5
    end
    subgraph c1[" "]
        N2
        N6
    end
    N0 --> N2
    N0 --> N6
    N1 --> N5
    N1 --> N6
    N2 --> N5
    N3 --> N0
    N3 --> N4
    N3 --> N5
    N4 --> N6
    N5 --> N5
```

## Declined: MongoDB admission control, real titles

The Mermaid source is the MongoDB diagram as written; its subgraphs have no titles of their own, so
the id is the title. Declined only for the 36-column edge label, as it would be without subgraphs.

```mermaid
flowchart TB
A(Stable Probe) --> |at minimum and tickets not exhausted|A

A --> |"(above minimum and tickets not exhausted) or at maximum"|C(Probe Down)
subgraph decrease
C --> |throughput increased|F{{Decrease stable concurrency}}
C --> |throughput did not increase|G{{Go back to stable concurrency}}
end
F --> H
G --> H

A --> |below maximum and tickets exhausted| B(Probe Up)
subgraph increase
B --> |throughput increased|D{{Increase stable concurrency}}
B --> |throughput did not increase|E{{Go back to stable concurrency}}
end
D --> H(Stable Probe)
E --> H
```

## Drawn: membership, a node named outside first

`C` and `F` belong to `S`, `A` does not.

```mermaid
graph TD
    A --> C
    subgraph S
        C --> F
    end
```

## Drawn: membership, a node named in two siblings

`Y` belongs to `S1`, which closes first.

```mermaid
graph TD
    subgraph S1
        X --> Y
    end
    subgraph S2
        Y --> Z
    end
```

## Membership: nesting

`A` and `B` belong to `Q`, `C` and `D` to `P`.

```mermaid
graph TD
    subgraph P
        A
        subgraph Q
            A --> B
        end
        C --> D
    end
```

## Membership: a node named again in the outer subgraph

`A` stays in `Q`, `C` is in `P`.

```mermaid
graph TD
    subgraph P
        subgraph Q
            A --> B
        end
        A --> C
    end
```

## Membership: three levels and siblings

```mermaid
graph TD
    subgraph P
        subgraph Q
            subgraph R
                A
            end
            B
        end
        subgraph T
            C
        end
        D
    end
    E --> A
```

## Membership: a subgraph of subgraphs only

The outer subgraph names no node itself, as in MongoDB's plan enumerator diagram.

```mermaid
graph TD
    subgraph "QueryPlanner::plan()"
        subgraph Index Tagging
            C --> D
        end
        subgraph Plan Enumeration
            G --> H
        end
    end
    A --> C
    D --> G
```

## Drawn: membership, style and class do not name a node

`C` and `D` are outside `S`: only a node declaration or a link puts a node in a subgraph.

```mermaid
graph TD
    C --> D
    subgraph S
        A --> B
        class C foo
        style D fill:#f00
    end
```

## Drawn: membership, `&`, `;` and one line

```mermaid
graph TD
    subgraph S; A & B --> C; end
    subgraph T;
        D
    end;
    X --> Y
```

## Drawn: ids that start with `end` or `subgraph`

```mermaid
graph TD
    subgraph S
        endpoint --> subgraphx
        A["end"] --> D[the end]
    end
```

## Declined: a direction inside a subgraph

```mermaid
graph TD
    subgraph one
        direction LR
        A --> B
    end
```

## Declined: a link to a subgraph

```mermaid
graph TD
    subgraph S
        A
    end
    B --> S
```

## Declined: a link to a subgraph written before it

```mermaid
graph TD
    B --> S
    subgraph S
        A
    end
```

## Declined: a link from a subgraph to itself

```mermaid
graph TD
    subgraph S
        S --> A
    end
```

## Declined: a subgraph's id as a node

```mermaid
graph TD
    subgraph S
        A
    end
    S
```

## Declined: `style` on a subgraph

```mermaid
graph TD
    subgraph S
        A
    end
    style S fill:#f00
```

## Declined: `class` on a subgraph

```mermaid
graph TD
    subgraph S
        A
    end
    class S foo
```

## Declined: `end` with no subgraph

```mermaid
graph TD
    A --> B
    end
```

## Declined: a subgraph never closed

```mermaid
graph TD
    subgraph S
        A --> B
```

## Declined: two subgraphs with one id

```mermaid
graph TD
    subgraph S
        A
    end
    subgraph S
        B
    end
```

## Declined: an empty subgraph

`mmdc` draws an empty subgraph as an ordinary box.

```mermaid
graph TD
    subgraph S
    end
    A --> B
```

## Declined: a subgraph that holds nothing once an earlier one has its nodes

```mermaid
graph TD
    subgraph S1
        X --> Y
    end
    subgraph S2
        Y
    end
```

## Declined: `end` as a subgraph id

```mermaid
graph TD
    subgraph end
        A
    end
```

## Declined: text after a title

```mermaid
graph TD
    subgraph id [Title] extra
        A
    end
```

## Declined: an empty title

```mermaid
graph TD
    subgraph id[]
        A
    end
```

## Declined: parentheses in an unquoted title

```mermaid
graph TD
    subgraph id[Title (x)]
        A
    end
```

## Declined: HTML in a title

```mermaid
graph TD
    subgraph id["a<b>x</b>"]
        A
    end
```
