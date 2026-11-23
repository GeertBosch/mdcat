# Mermaid edge labels

On a Kitty terminal, the built-in renderer writes an edge's label as ordinary text
on the edge, which stops a little short of it on either side. The label is a node
of its own in the layout, halfway along the edge, so it gets room like any other
box. Every rank is twice as far apart in a diagram with labels, so a label always
has a rank to sit in. A label doesn't stop its edge from sharing a port: edges with different labels, or
none, still split from one box or merge into one on a single bus.
Checked by `mermaid-kitty.sh` at widths 80 and 44, with `--img kitty` and pinned
cell sizes. The `.err` golden names the diagrams that are declined, and so are
shown as source. A diagram too wide for width 44 is drawn wider than it, after
fitting (see `mermaid-12-fit.md`) has done what it can.

## Every syntax (top down)

Expected: each pair is joined as its label says, and the label sits on the edge
midway between the two boxes. `-- text -->` and `-->|text|` draw the same arrow,
and `---` and `-- text ---` the same open link. The dotted link is dashed, the
thick one is thick with a larger arrowhead, and `--o` and `--x` end in a circle and
an X. A space between `-->` and `|` makes no difference.

```mermaid
graph TD
    a1[one] -- text --> b1[arrow]
    a2[two] -->|pipe| b2[arrow]
    a3[three] -- open --- b3[open]
    a4[four] ---|open| b4[open]
    a5[five] -. dots .-> b5[dotted]
```

## Every syntax, continued (top down)

Expected: the remaining spellings. `== text ==>` and `==>|text|` are thick, `-.->|text|`
is dotted, and the marker on the left of `<-- text -->` is an arrowhead at the source.

```mermaid
graph TD
    a1[one] == text ==> b1[thick]
    a2[two] ==>|pipe| b2[thick]
    a3[three] -.->|pipe| b3[dotted]
    a4[four] -- circle --o b4[circle]
    a5[five] -- cross --x b5[cross]
    a6[six] <-- both --> b6[both]
```

## Brackets and pipes in link text (top down)

Expected: text in the middle of a link is everything up to the link's own end, so
parentheses, brackets, braces and a pipe are plain text there (`foo()`, `[5]`, `{n}`,
`a|b`), and the label is drawn on the edge as written. (After `-->` or `---`, a `|text|`
label may not hold them: Mermaid rejects that, and see *Declined*.)

```mermaid
graph TD
    a1[one] -- foo() --> b1[call]
    a2[two] -- [5] --> b2[index]
    a3[three] -- {n} --> b3[count]
    a4[four] -- a|b --> b4[pipe]
```

## Link text without a space after the start (top down)

Expected: `--[7] -->`, `-.[x].->` and `==[y]==>` are text links too, as in Mermaid,
and so is `--[8]-->`, which needs no space at the end either. Each is drawn like its
spaced form: solid, dashed or thick. Every other link, `-->`, `---`, `--o`, `-.->`
and `==>`, reads as it always did.

```mermaid
graph TD
    a1[one] --[7] --> b1[solid]
    a2[two] --[8]--> b2[solid]
    a3[three] -.[x].-> b3[dotted]
    a4[four] ==[y]==> b4[thick]
    a5[five] --> b5[plain]
    a6[six] --- b6[open]
    a7[seven] -.-> b7[dotted]
    a8[eight] ==> b8[thick]
    a9[nine] --o b9[circle]
```

## A decision (top down)

Expected: `Valid?` has two labelled branches, `yes` and `no`, both leaving it through
one port and carrying their labels in a rank of their own halfway down. Both
branches then meet at `Done`.

```mermaid
graph TD
    input[Input] --> check[Valid?]
    check -->|yes| run[Run]
    check -->|no| fix[Fix it]
    run --> done[Done]
    fix --> done
```

## A decision (left to right)

Expected: the same graph turned on its side. Each label sits on its edge in a
column of its own between the boxes, and the edge is cut a little before it and a
little after.

```mermaid
graph LR
    input[Input] --> check[Valid?]
    check -->|yes| run[Run]
    check -->|no| fix[Fix it]
    run --> done[Done]
    fix --> done
```

## A label on a long link (top down)

Expected: `a -- skip ---> c` is two steps long, so its label sits beside `Middle`, the
box halfway down, and the edge runs straight past it. The edge `a -->|one| d` is one
step long, so its label sits one step down, beside nothing.

```mermaid
graph TD
    a[Start] --> b[Middle]
    b --> c[End]
    a -- skip ---> c
    a -->|one| d[Side]
```

## Other directions (bottom to top, right to left)

Expected: labels are never mirrored, and the arrowheads still point the true way.

```mermaid
graph BT
    a[Start] -->|up| b[Middle]
    b -->|more| c[End]
```

```mermaid
graph RL
    a[Start] -->|left| b[Middle]
    b -->|more| c[End]
```

## Two labelled edges into one target (top down)

Expected: the two labelled edges and the unlabelled one from `C` all arrive at
`Target` through one port, on one bus, with one arrowhead.

```mermaid
graph TD
    a[A] -->|first| t[Target]
    b[B] -->|second| t
    c[C] --> t
```

## Two labelled edges into one target (left to right)

Expected: as above, turned on its side.

```mermaid
graph LR
    a[A] -->|first| t[Target]
    b[B] -->|second| t
    c[C] --> t
```

## Chains and groups

Expected: a label belongs to the link it is written on, so in a chain only the
second link has one, and `&` gives the same label to each edge.

```mermaid
graph LR
    a[Start] --> b[Middle] -->|labelled| c[End]
    c -->|fan| d[One] & e[Two]
```

## Declined

Expected: shown as source, as the `.err` golden says, in this order. A label over 24
columns is too wide; a label with HTML other than a line break, or an invisible link
with a label, is not drawn. A `|text|` label with parentheses or brackets is a parse
error in Mermaid, and so is `--x-->`, where `--x` is a link end. Text that touches
the end of its link (`-- foo--> `) is read by Mermaid as `fo` with the `o` taken as a
marker, so it is declined rather than drawn as `foo`. (Quoted and Markdown labels are
drawn: `mermaid-9-label-text.md`.)

```mermaid
graph TD
    a[A] -->|this label is much too long for the edge| b[B]
```

```mermaid
graph TD
    a[A] -->|<b>bold</b>| b[B]
```

```mermaid
graph TD
    a[A] ~~~|hidden| b[B]
```

```mermaid
graph TD
    a[A] -->|foo()| b[B]
```

```mermaid
graph TD
    a[A] -->|a[b]| b[B]
```

```mermaid
graph TD
    a[A] --x--> b[B]
```

```mermaid
graph TD
    a[A] -- foo--> b[B]
```
