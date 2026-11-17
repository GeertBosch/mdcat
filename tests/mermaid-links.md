# Mermaid link types and lengths

On a Kitty terminal, the built-in renderer draws every Mermaid link style: arrows
and open links, dotted and thick ones, invisible ones that only place boxes,
circle and cross ends, and markers at both ends. Checked by `mermaid-kitty.sh` at
width 80, with `--img kitty` and pinned cell sizes; the quadrant preview is too
coarse to show dashes, so `mermaid-kitty.sh` also reads a dotted link's pixels.

## Every link style (top down)

Expected: each pair is joined as its label says. `-->` has a filled arrowhead and
`---` has none; `-.->` and `-.-` are dashed, the first with an arrowhead; `==>`
and `===` are thick, the first with a larger arrowhead; `--o` ends in a filled
circle and `--x` in an X. Nothing is drawn for `~~~`, but its two boxes still sit
one rank apart.

```mermaid
graph TD
    a1[arrow] --> b1[head]
    a2[open] --- b2[none]
    a3[dotted] -.-> b3[arrow]
    a4[dotted] -.- b4[open]
    a5[thick] ==> b5[arrow]
```

```mermaid
graph TD
    a6[thick] === b6[open]
    a7[circle] --o b7[end]
    a8[cross] --x b8[end]
    a9[invisible] ~~~ b9[link]
```

## Every link style (left to right)

Expected: the same links turned on their side.

```mermaid
graph LR
    a1[arrow] --> b1[head]
    a2[open] --- b2[none]
    a3[dotted] -.-> b3[arrow]
    a4[dotted] -.- b4[open]
    a5[thick] ==> b5[arrow]
    a6[thick] === b6[open]
    a7[circle] --o b7[end]
    a8[cross] --x b8[end]
    a9[invisible] ~~~ b9[link]
```

## Markers at both ends (top down)

Expected: a marker at each end of every link, sitting on the outline of its box:
arrowheads for `<-->`, circles for `o--o` and X's for `x--x`, then the dotted and
thick forms.

```mermaid
graph TD
    a1[arrows] <--> b1[both ways]
    a2[circles] o--o b2[both ends]
    a3[crosses] x--x b3[both ends]
    a4[dotted] <-.-> b4[arrows]
```

```mermaid
graph TD
    a5[thick] <==> b5[arrows]
    a6[dotted] o-.-o b6[circles]
    a7[thick] x==x b7[crosses]
```

## Markers at both ends (left to right)

Expected: the same links turned on their side.

```mermaid
graph LR
    a1[arrows] <--> b1[both ways]
    a2[circles] o--o b2[both ends]
    a3[crosses] x--x b3[both ends]
    a4[dotted] <-.-> b4[arrows]
    a5[thick] <==> b5[arrows]
    a6[dotted] o-.-o b6[circles]
    a7[thick] x==x b7[crosses]
```

## Lengths

Expected: a link spans one rank more for each extra character. `-->` spans one
rank, `--->` and `-..->` and `===>` two, and `---->` three; the open link `----`
spans two. Each long link keeps its style, dashes or thickness, along the whole
way.

```mermaid
graph TD
    s[Start] --> a[One rank]
    a ---> b[Two ranks]
    b -..-> c[Dotted, two]
    c ===> d[Thick, two]
    d ----> e[Three ranks]
    e ---- f[Open, two]
```

## Styles fanning out of one box

Expected: `Source` leaves through a port for each style, so a dotted, a thick and a
plain arrow do not share a bus, and no stroke joins two edges of different
styles. The two plain arrows do share one.

```mermaid
graph TD
    s[Source] --> a[Plain]
    s --> b[Plain too]
    s -.-> c[Dotted]
    s ==> d[Thick]
```

## Styles merging into one box

Expected: the same for a merge: each style arrives at `Sink` through its own
port, with its own arrowhead.

```mermaid
graph LR
    a[Plain] --> k[Sink]
    b[Dotted] -.-> k
    c[Thick] ==> k
```

## An invisible link places boxes

Expected: `Left` and `Right` are linked by `~~~` alone, so they sit in two ranks,
`Right` below `Left`, with nothing drawn between them. `Right` also receives an
arrow from `Top`, which crosses the gap without touching anything.

```mermaid
graph TD
    top[Top] --> left[Left]
    left ~~~ right[Right]
    top --> right
```

## An invisible link longer than one rank

Expected: `~~~~` makes `Far` sit two ranks below `Near`, and nothing joins them.

```mermaid
graph TD
    near[Near] --> mid[Middle]
    near ~~~~ far[Far]
```

## Dotted links that bend and cross

Expected: dashes restart on every straight stretch, and a dashed edge passes over
a plain one on a bridge.

```mermaid
graph TD
    a[A] -.-> c[C]
    a -.-> d[D]
    b[B] --> c
    b --> d
```
