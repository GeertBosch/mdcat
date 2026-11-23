# Mermaid subgraphs

`subgraph ... end` is read into the model (`Graph::clusters`, `Node::cluster`) and drawn, in every
direction but only one level deep (docs/plan-mermaid-subgraphs.md, Steps 5-8): the box is one outline
under the nodes and edges, around the nodes named inside it, with the subgraph's title, if it has one,
in bold under the top border. The cases whose heading starts with *Drawn* are drawn; the title says the
direction where it is not top-down. The rest are declined: a nested subgraph is `subgraph layout` for
now, and the other cases are declined for the reason named in their heading or, for the membership
cases, because of what they hold. `MDCAT_MERMAID_DEBUG=1` prints each reason on stderr, and
`mermaid-kitty.sh` compares it with `expected/mermaid-14-subgraphs.w80.err`. Which node belongs to which
subgraph is checked outside the repository, with a scratch parser.

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

## Drawn, LR: one box around a chain

Expected: the top-down chain case turned on its side. A box around A, B and C, one blank column and one
blank row inside it on each side. X's edge enters it through the left border and C's leaves through the
right border. Y is reached by X's second edge, which goes round under the box.

```mermaid
graph LR
    X --> A
    subgraph s[" "]
        A --> B --> C
    end
    C --> Y
    X --> Y
```

## Drawn, LR: two boxes, both directions and a back edge

Expected: two boxes side by side, a few columns apart so that the jog of the edge between them is a
blank column from each border. That edge crosses both borders; the back edge from the second to the
first goes round under both boxes and enters the first box through its bottom border.

```mermaid
graph LR
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

## Drawn, LR: an edge goes round a box

Expected: X's edge to Y runs above the box and Z's below it; the edge into A crosses the left border
and D's leaves through the right one.

```mermaid
graph LR
    X --> Y
    subgraph s[" "]
        A --> B --> C --> D
    end
    X --> A
    X --> Z
    Z --> Y
    D --> Y
```

## Drawn, LR: a rank of the span that holds no member

Expected: A and D are in the box and X, between them, is not. X is above the box, and the edges from A
to X and from X to D cross its top border.

```mermaid
graph LR
    subgraph s[" "]
        A
        D
    end
    A --> X --> D
    X --> Y
```

## Drawn, LR: a long edge, a label and a self-loop inside a box

Expected: everything is inside the box: the edge from A to C runs along its bottom, "yes" sits between
B and D, and the loop on C is below C.

```mermaid
graph LR
    subgraph s[" "]
        A --> B --> C
        A --> C
        B -->|yes| D
        C --> C
    end
    S --> A
    D --> T
```

## Drawn, LR: a title, centred where no edge crosses it

Expected: a box around A and B with `Plan` in bold on the row under the top border, in the middle of
the box, then a blank row, then the members. Nothing crosses the top border.

```mermaid
graph LR
    subgraph s[Plan]
        A --> B
    end
```

## Drawn, LR: a title wider than the box

Expected: the box is as wide as its title needs plus a blank column and the border on each side, not
as wide as its one member: the extra columns are inside the box, half before the member and half after
it, so the edges into and out of the box are longer there.

```mermaid
graph LR
    subgraph s[A title much wider than its one member]
        A
    end
    X --> A
    A --> Y
```

## Drawn, LR: a title beside an edge that crosses the top border

Expected: Y is above the box, and its edge from X runs along above the box and then down into D, through
the top border. `A long title here for the box to hold` is not centred: it sits to the right of that edge,
a blank column from it, and the box grows to the right of D to make room.

```mermaid
graph LR
    X --> Y
    Y --> D
    X --> A
    subgraph s[A long title here for the box to hold]
        A --> B --> C --> D
    end
```

## Drawn, LR: boxes side by side, titles of different heights

Expected: two boxes, one above the other, X's edges entering both through their left borders. The title
of the upper one is two lines and the lower one's is one; each has a blank row after its title and then
its members.

```mermaid
graph LR
    subgraph l["two<br>lines"]
        A --> C
    end
    subgraph r[one]
        B --> D
    end
    X --> A
    X --> B
```

## Drawn, LR: the title of a box that an edge leaves through the top

Expected: the box's first member has an edge to Y, which lies above the box. The edge runs up from the
gap between A and B, through the top border and past the title: `Wide title here for it` sits to the
right of that edge, and the box grows to the right.

```mermaid
graph LR
    P --> Y
    subgraph s[Wide title here for it]
        A --> B
    end
    P --> A
    A --> Y
```

## Drawn, BT: a titled box, an edge leaving through the title's rows

Expected: the flow runs upwards, so the top of the box is where its edges leave: X's edge enters it
through the bottom border and B's leaves through the top one, past the title. `one` is in bold on
the row under the top border, left of that edge and a blank column from it.

```mermaid
graph BT
    X --> A
    subgraph one
        A --> B
    end
    B --> Y
```

## Drawn, BT: boxes in a chain, titled

Expected: two boxes one above the other, each with its title in bold under its top border. A1's edge
leaves the lower box through its top border and enters the upper box through its bottom border, and A2's
edge to A3 leaves that through its top border. Both leave through the title's rows, so each title sits
beside its edge, a blank column from it.

```mermaid
graph BT
    subgraph S
        A1
    end
    subgraph id1 [Title one]
        A2
    end
    A1 --> A2
    A2 --> A3
```

## Drawn, BT: two boxes, both directions and a back edge, one title two lines high

Expected: the second box has the two-line title `two` and `lines` under its top border, and the first
none. The back edge goes round the right of both boxes.

```mermaid
graph BT
    subgraph a[" "]
        A1 --> A2
    end
    subgraph b["two<br>lines"]
        B1 --> B2
    end
    A2 --> B1
    B2 --> A1
    S --> A1
    B2 --> T
```

## Drawn, RL: a titled box

Expected: the flow runs from the right: X is at the right of the box, which holds A then B right to
left, and Y is at the left. `one` is centred under the top border.

```mermaid
graph RL
    X --> A
    subgraph one
        A --> B
    end
    B --> Y
```

## Drawn, RL: a title wider than the box, and one beside an edge

Expected: the first box is as wide as its title needs, with the extra columns inside it before and
after the member. In the second, Y is above the box and its edge enters through the top border, so the
title is not centred: it sits at the left of that edge and the box grows to the left.

```mermaid
graph RL
    subgraph s[A title much wider than its one member]
        A
    end
    X --> A
    A --> Y
```

```mermaid
graph RL
    P --> Y
    subgraph t[Wide title here for it]
        A --> B
    end
    P --> A
    A --> Y
```

## Drawn, RL: two boxes, both directions and a back edge

Expected: the left-to-right case mirrored: two boxes side by side, the first on the right, and the back
edge goes round under both. The first box has no title and the second has two lines.

```mermaid
graph RL
    subgraph a[" "]
        A1 --> A2
    end
    subgraph b["two<br>lines"]
        B1 --> B2
    end
    A2 --> B1
    B2 --> A1
    S --> A1
    B2 --> T
```

## Drawn, LR: MongoDB query commands, shorter edge labels

The diagram from `src/mongo/db/commands/query_cmd/README.md`, with its subgraph title blank as written
and its edge labels cut to at most 24 columns, and `n10`'s `extract` shape (which this renderer has
not got) left out. Expected: the box holds Find, Distinct and Count, and the three edges from it merge
into IDL Parsing, outside it to its right.

```mermaid
---
config:
  themeVariables:
    fontSize: 32px
---
flowchart LR
 subgraph s1[" "]
        n1["Find"]
        n2["Distinct"]
        n3["Count"]
  end
    n1 --> n10["IDL Parsing"]
    n2 --> n10
    n3 --> n10
    n10 --> n11["FindCommandRequest"] & n12["DistinctCommandRequest"]
    n10 --> n21["CountCommandRequest"]
    n13["ParsedFindCommand"] --> n17["CanonicalQuery"]
    n19["ParsedDistinctCommand"] --> n17
    n21 -- parseFromCount() --> n11
    n11 -- "find::parse()<br>MatchParser::parse()" --> n13
    n12 -- "distinct::parse()<br>MatchParser::parse()" --> n19

    n17@{ shape: dbl-circ}
```

## Declined, LR: a box that an edge would have to cross

Expected: declined, as the same graph is top-down: N1's edge to N6 would cross the box of N3, N4 and N5.

```mermaid
graph LR
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

## Declined, LR: MongoDB query commands, as written

The diagram exactly as MongoDB wrote it, except for the `extract` shape left out. Declined for the 30-column
edge label, as it would be without the subgraph.

```mermaid
flowchart LR
 subgraph s1[" "]
        n1["Find"]
        n2["Distinct"]
        n3["Count"]
  end
    n1 --> n10["IDL Parsing"]
    n2 --> n10
    n3 --> n10
    n10 --> n11["FindCommandRequest"] & n12["DistinctCommandRequest"]
    n10 --> n21["CountCommandRequest"]
    n13["ParsedFindCommand"] --> n17["CanonicalQuery"]
    n19["ParsedDistinctCommand"] --> n17
    n21 -- parseFromCount() --> n11
    n11 -- "parsed_find_command::parse()<br>MatchExpressionParser::parse()" --> n13
    n12 -- "parsed_distinct_command::parse()<br>MatchExpressionParser::parse()" --> n19

    n17@{ shape: dbl-circ}
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
