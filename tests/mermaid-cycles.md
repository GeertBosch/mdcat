# Mermaid cycles

On a Kitty terminal, the built-in renderer draws a graph with cycles. Like Dagre, it
finds the edges that close a cycle with a depth-first search, in order of first
appearance, and reverses them for the layout, so that the graph is laid out in ranks.
Each of those back edges is then drawn from its real source to its real target, against
the flow: it leaves the top of its source (top down), runs up, and arrives, with its
arrowhead, at the bottom of its target. It has ports of its own, one cell off the middle
of the side, so it never shares strokes with the edges that run the other way.
Checked by `mermaid-kitty.sh` at width 80, with `--img kitty` and pinned cell sizes. The
`.err` golden names the diagrams that are declined, and so are shown as source.

## A retry loop (top down)

Expected: `Try` and `Check` are in a chain below `Start`, and the arrow from `Check`
back to `Try` runs up beside the one from `Try` to `Check`, with its arrowhead at the
bottom of `Try`.

```mermaid
graph TD
    A[Start] --> B[Try]
    B --> C[Check]
    C --> B
```

## A retry loop (left to right)

Expected: the same graph turned on its side. The arrow back runs left, below the one
that goes right, and its arrowhead touches the right side of `Try`.

```mermaid
graph LR
    A[Start] --> B[Try]
    B --> C[Check]
    C --> B
```

## Minimum length of a link, with a loop (top down)

This is the example from the Mermaid documentation. The arrow from `Rethink` back to
`Is it?` is a back edge. The two spellings of the label differ only in the way the label
is written, so the two diagrams are drawn exactly alike.

Expected: `End` is three ranks below `Is it?`, as `---->` says, while `Rethink` is two
ranks below it and loops up to the decision.

```mermaid
flowchart TD
    A[Start] --> B{Is it?}
    B -->|Yes| C[OK]
    C --> D[Rethink]
    D --> B
    B ---->|No| E[End]
```

```mermaid
flowchart TD
    A[Start] --> B{Is it?}
    B -- Yes --> C[OK]
    C --> D[Rethink]
    D --> B
    B -- No ----> E[End]
```

## A 2-cycle (top down and left to right)

Expected: two boxes joined by one arrow each way, side by side, each with an arrowhead.

```mermaid
graph TD
    A[Ping] --> B[Pong]
    B --> A
```

```mermaid
graph LR
    A[Ping] --> B[Pong]
    B --> A
```

## A long loop (top down)

Expected: `Three` points back at `One`. The back edge runs up the side of the chain
through the ranks in between, and arrives at the bottom of `One`.

```mermaid
graph TD
    A[One] --> B[Two]
    B --> C[Three]
    C --> A
```

## A loop with a label (top down)

Expected: the label of the back edge sits on it, halfway up.

```mermaid
graph TD
    A[Fetch] --> B[Parse]
    B --> C[Store]
    C -->|retry| A
```

## A dotted back edge, and one loop in each direction (bottom up, right to left)

Expected: the dotted edge keeps its style, and its arrowhead is at the original target.
In the bottom-up graph the flow, and so the back edge, runs the other way.

```mermaid
graph BT
    A[Base] --> B[Middle]
    B --> C[Top]
    C -.-> A
```

```mermaid
graph RL
    A[One] --> B[Two]
    B --> C[Three]
    C --> B
```

## Three back edges, and a node that grows to hold their ports (top down)

Expected: `D` points back at `A`, `B` and `C`, each arrow running up its own line to the
bottom of its target. `D` is wider than a one-letter box, because its four ports (three
back edges and the arrow in from `C`) need a cell each, clear of its corners.

```mermaid
graph TD
    A --> B
    B --> C
    C --> D
    D --> A
    D --> B
    D --> C
```

## Four nodes, each pointing back at every earlier one (top down)

Expected: six back edges, within the eight that fit 80 columns. The boxes widen to fit
their ports, and every arrowhead touches the box it points at.

```mermaid
graph TD
    A --> B
    B --> C
    C --> D
    D --> A
    D --> B
    D --> C
    C --> A
    C --> B
    B --> A
```

## Three back edges, left to right

Expected: a box with several ports grows taller instead.

```mermaid
graph LR
    A --> B
    B --> C
    C --> D
    D --> A
    D --> B
    D --> C
```

## Too many back edges (declined)

Expected: five nodes with an edge each way between every pair have ten back edges, more
than the eight that fit 80 columns (one per ten columns). The builtin declines with
`back edges: 10 > 8`, and the source is shown, without trying `mmdc`.

```mermaid
graph TD
    A --> B
    A --> C
    A --> D
    A --> E
    B --> C
    B --> D
    B --> E
    C --> D
    C --> E
    D --> E
    B --> A
    C --> A
    D --> A
    E --> A
    C --> B
    D --> B
    E --> B
    D --> C
    E --> C
    E --> D
```

## A self-loop (declined for now)

Expected: the builtin declines with `self-loop`, and the source is shown.

```mermaid
graph TD
    A --> A
```
