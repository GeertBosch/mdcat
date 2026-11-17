# Mermaid diagrams the built-in renderer declines

With `--mermaid builtin`, each of these shows its source as a code panel, and
`MDCAT_MERMAID_DEBUG=1` names the reason on stderr. Checked by `mermaid-kitty.sh`.

## Sequence diagram

```mermaid
sequenceDiagram
    Alice->>Bob: Hello
    Bob-->>Alice: Hi
```

## Pie chart

```mermaid
pie title Pets
    "Dogs" : 3
    "Cats" : 2
```

## Subgraph

```mermaid
graph TD
    subgraph one
        A --> B
    end
```

## Subgraph with a direction

```mermaid
graph TD
    subgraph one
        direction LR
        A --> B
    end
```

## Direction statement

```mermaid
graph TD
    direction LR
    A --> B
```

## Reserved word as an id

```mermaid
graph TD
    A --> end
```

## Mismatched link ends

```mermaid
graph LR
    A o--> B
```

## Unknown direction

```mermaid
graph XY
    A --> B
```
