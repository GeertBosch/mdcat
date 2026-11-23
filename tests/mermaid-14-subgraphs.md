# Mermaid subgraphs: parsed, not drawn yet

`subgraph ... end` is read into the model (`Graph::clusters`, `Node::cluster`), but nothing lays
a subgraph out yet (docs/plan-mermaid-subgraphs.md, Steps 5-8), so every diagram that parses is
declined with `subgraph layout` and shown as source. The other cases are declined by the parser
for the reason named in their heading. `MDCAT_MERMAID_DEBUG=1` prints each reason on stderr, and
`mermaid-kitty.sh` compares it with `expected/mermaid-14-subgraphs.w80.err`. Which node belongs
to which subgraph can't be seen in the output; it is checked outside the repository.

## One subgraph

```mermaid
graph TD
    subgraph one
        A --> B
    end
```

## MongoDB: admission control

The Mermaid source is the MongoDB diagram as written; its subgraphs have no titles of their own, so the id is the title.

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

## Title forms

Every way Mermaid names a subgraph: a bare id, `id [Title]`, `id[Title]`, `id["Title"]`, a blank title, a quoted id, a title with spaces, and Markdown, `<br>` and `;` in titles.

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
    subgraph id4[" "]
        A5
    end
    subgraph "Q::plan()"
        A6
    end
    subgraph Index Tagging
        A7
    end
    subgraph id5["`**Markdown** title`"]
        A8
    end
    subgraph id6["two<br>lines"]
        A9
    end
    subgraph id7[A & B; C]
        A10
    end
```

## Membership: a node named outside first

`C` and `F` belong to `S`, `A` does not.

```mermaid
graph TD
    A --> C
    subgraph S
        C --> F
    end
```

## Membership: a node named in two siblings

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

## Membership: style and class do not name a node

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

## Membership: `&`, `;` and one line

```mermaid
graph TD
    subgraph S; A & B --> C; end
    subgraph T;
        D
    end;
    X --> Y
```

## Ids that start with `end` or `subgraph`

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
