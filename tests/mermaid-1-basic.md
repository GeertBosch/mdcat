# Mermaid flowcharts drawn by mdcat

On a terminal with the Kitty graphics protocol, a ```` ```mermaid ```` block holding
a flowchart of boxes is drawn by mdcat itself: the boxes and arrows are one
transparent image, and the labels are ordinary text in the cells over it. Checked
by `mermaid-kitty.sh`, which renders it with `--img kitty` and pinned cell sizes.
Other terminals, and every diagram outside this subset, behave as before.
`mermaid-3-dag.md` covers the branching shapes in depth.

## Left to right

Expected: three boxes in a row joined by arrows. Every arrowhead touches the
next box's outline, and each label is centred in its box.

```mermaid
graph LR
    parse[Parse] --> render[Render]
    render --> output[Output]
```

## Top down

Expected: ten boxes in a column, 48 rows tall (taller than most terminals), joined
by vertical arrows that all run through one column. In `gmore` the diagram scrolls
a row at a time.

```mermaid
graph TD
    a[Start] --> b[Step 1]
    b --> c[Step 2]
    c --> d[Step 3]
    d --> e[Step 4]
    e --> f[Step 5]
    f --> g[Step 6]
    g --> h[Step 7]
    h --> i[Step 8]
    i --> j[End]
```

## Branching

Expected: `A` above `B` and `C`, with one arrow from `A` splitting into two.

```mermaid
graph TD
    A --> B
    A --> C
```
