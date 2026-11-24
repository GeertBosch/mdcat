# Built-in Mermaid sequence diagrams: self-messages, notes, activations

Step 3 of `docs/plan-mermaid-sequence.md`. Checked by `mermaid-kitty.sh` at width 80.

## Self-messages

A loop to the right that comes back to the lifeline, with its text above it, in every
arrow style. The text makes room before the next lifeline, and the last participant's
widens the picture.

```mermaid
sequenceDiagram
    participant A
    participant B
    A->>A: solid, arrowhead
    A-->>A: dotted, arrowhead
    A->A: solid, no head
    A-->A: dotted, no head
    A-xA: solid, cross
    A--xA: dotted, cross
    A-)A: solid, async
    A--)A: dotted, async
    B->>B: the last participant<br/>has two lines of text
    B->>B:
    A->>B: an ordinary message
```

## Notes

All four placements. `over` one lifeline is centred on it, over two it reaches past both,
and over three the lifeline between breaks at its border. `left of` the first lifeline
widens the picture to the left, and a long multi-line note `right of` the last widens it
to the right.

```mermaid
sequenceDiagram
    participant A
    participant B
    participant C
    A->>B: before
    Note over B: over one
    Note over A,B: over two
    Note over A,C: over three, past B
    Note over C,A: other order
    Note left of A: left of the<br/>first lifeline
    Note right of A: right of A
    Note left of C: left of C
    Note right of C: a long note<br/>with<br/>four<br/>lines
    C->>A: after
```

A note beside a lifeline makes room between it and its neighbour:

```mermaid
sequenceDiagram
    participant S as Router
    participant C as CSRS
    note over S,C: Initial routing info
    S->>C: getCollectionRoutingInfo
    Note right of S: The routing info tells the<br/>router what to contact
    C->>S: Shard 1
    Note left of C: Since the shard has a newer<br/>version, it says so
```

## Activations

`+` after the arrow activates the receiver, `-` deactivates the sender. A box is a cell
wide on the lifeline, a nested one is shifted half a box to the right, and an arrow meets
the box, not the lifeline. A self-message leaves from the side of the stack.

```mermaid
sequenceDiagram
    participant A
    participant B
    A->>A: self think
    A->>+B: call
    B->>+B: inner
    B-->>-B: back
    B-->>-A: ret
```

Three levels, and arrows from both sides into a box:

```mermaid
sequenceDiagram
    participant A
    participant B
    participant C
    A->>+B: one
    C->>+B: two, from the right
    B->>+B: three
    B-->>-C: back right
    B-->>-B: back
    B-->>-A: out
    C->>A: past B, no box now
```

Standalone `activate` and `deactivate` act on the row of the message before. A box still
open at the end closes there, a note over a box stops it at the note's border, and an
`activate` followed at once by `deactivate` is one row tall.

```mermaid
sequenceDiagram
    participant A
    participant B
    A->>B: one
    activate B
    B->>A: two
    Note over A,B: A note over a box
    A->>B: three
    deactivate B
    A->>B: four
    activate A
    deactivate A
    B->>+B: never closed
```

## Falls back

A `-` with nothing to close, `deactivate` with nothing open, and a message whose text passes in
front of an activated lifeline are not drawn.

```mermaid
sequenceDiagram
    A->>B: hello
    B-->>-A: nothing to close
```

```mermaid
sequenceDiagram
    A->>B: hello
    deactivate B
```

```mermaid
sequenceDiagram
    A->>+B: hello
    C->>A: a message that passes in front of B
    B-->>-A: bye
```
