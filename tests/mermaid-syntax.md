# Mermaid syntax the built-in renderer reads

Directions, statement forms, and syntax that is understood but has no visual
effect. Checked by `mermaid-kitty.sh` at width 80, with the fallback reasons in
`expected/mermaid-syntax.w80.err`.

## Directions

Expected: the same three boxes in all four directions. Arrowheads follow the true
direction (up for `BT`, left for `RL`), and the labels are never mirrored.

```mermaid
graph BT
    A[Base] --> B[Middle]
    B --> C[Top]
```

```mermaid
graph RL
    A[First] --> B[Second]
    B --> C[Third]
```

## Direction aliases

Expected: `>` is `LR`, `<` is `RL`, `^` is `BT` and `v` is `TB`.

```mermaid
graph >
    A --> B
```

```mermaid
graph <
    A --> B
```

```mermaid
graph ^
    A --> B
```

```mermaid
graph v
    A --> B
```

## No direction, and `flowchart-elk`

Expected: both read as top-down, as `flowchart-elk` reads as `flowchart`.

```mermaid
flowchart
    A --> B
```

```mermaid
flowchart-elk LR
    A --> B
```

## Fan-out, bottom to top

Expected: a split drawn upward, one arrow from `A` to `B` and `C`, heads pointing up.

```mermaid
flowchart BT
    A --> B & C
    B --> D
```

## Fan-in, right to left

```mermaid
flowchart RL
    A & B --> C
```

## Statements

Expected: `;` and newlines both separate statements. The chain makes `A --> B` and
`B --> C`.

```mermaid
graph LR; A --> B --> C
    C --> D; D --> E
```

## Chains and `&`

Expected: `a --> b & c --> d` makes `a --> b`, `a --> c`, `b --> d` and `c --> d`.
(`A & B --> C & D` is in `mermaid-crossings.md`.)

```mermaid
graph TD
    a --> b & c --> d
```

## Redefinition

Expected: the label is the last explicit text, `New`, and a bare `A` keeps it.

```mermaid
graph LR
    A[Old] --> B
    A[New] --> C
    A --> D
```

## Comments and directives

Expected: three boxes in a row. A `%%` line, a trailing `%%`, and a `%%{ }%%`
directive over several lines have no effect.

```mermaid
%%{init: {
  "theme": "dark"
}}%%
graph LR
    %% a comment
    A[One] --> B[Two] %% trailing
    B --> C[Three]
```

## Front matter

Expected: a bold, centred title above the diagram. `config:` has no effect.

```mermaid
---
title: A short pipeline
config:
  theme: forest
  markdownAutoWrap: false
---
graph LR
    A --> B
```

## Wide title

Expected: the title is wider than the diagram, which is centred under it.

```mermaid
---
title: "A title much wider than the two boxes below it"
---
graph TD
    A --> B
```

## Understood, but no visual effect

Expected: the diagram is just `A --> B` and `B --> C`. Styling, classes, links,
accessibility text, edge ids and edge properties are all read and dropped.

```mermaid
graph TD
    accTitle: A title
    accDescr: A description
    A:::big --> B
    A e1@--> C
    e1@{ animate: true }
    classDef big fill:#f9f,stroke:#333
    class B big
    style C fill:#bbf
    linkStyle 0 stroke:#ff3
    click A callback "Tooltip"
    click B href "https://example.com" _blank
```

## Multi-line accessibility description

```mermaid
graph LR
    accDescr {
        Two boxes; the first
        points at the second.
    }
    A --> B
```

## Ids

Expected: ids may hold non-ASCII letters, `.` and `-`.

```mermaid
graph LR
    café --> a.b
    a.b --> x-y
```
