# Built-in Mermaid sequence diagrams

Step 2 of `docs/plan-mermaid-sequence.md`: participants, boxes at the top and the
bottom, dotted lifelines and messages. Checked by `mermaid-kitty.sh` at width 80.

## Every arrow kind

```mermaid
sequenceDiagram
    title: Arrow kinds
    A->>B: solid, arrowhead
    B-->>A: dotted, arrowhead
    A->B: solid, no head
    B-->A: dotted, no head
    A-xB: solid, cross
    B--xA: dotted, cross
    A-)B: solid, async
    B--)A: dotted, async
```

## Implicit participants

Names may hold spaces and the case matters: `Generate task script` and
`Generate Task Script` are two participants.

```mermaid
sequenceDiagram
    Task Shut Down ->> Generate task script: If core dumps are present,<br/>generate task config
    Generate Task Script ->> Task Shut Down: Write generated config
    Task Shut Down ->> Generated Task: Use evergreen to generate the task
```

## Aliases and declared participants

The order of first mention is the order on the page, whether a participant is declared
or not. A later `as` renames a participant that a message created.

```mermaid
sequenceDiagram
    participant C as Client
    Participant S as Server<br/>(primary)
    C->>S: request
    S-->>C: response
    participant Z
    Z->>C: ping
    participant S as Server (renamed)
```

## A message that spans lifelines

The lifelines it passes break behind its text.

```mermaid
sequenceDiagram
    participant A
    participant B
    participant C
    participant D
    A->>D: a long message that passes B and C
    D-->>A: and one back, over the same three
    A->>B: next door
    B->>D: skips C
```

## Multi-line text

```mermaid
sequenceDiagram
    A->>B: one<br/>two<br>three
    B->>A: after
    A->>B:
    B-->>A: empty text above
```

## Syntax that is read

`;` and newlines separate statements, `%%` lines and `#` comments are dropped, `#35;`
is an entity, `+` and `-` after an arrow and `activate` / `deactivate` are read and
not drawn yet, and the keywords ignore case.

```mermaid
---
title: Front matter title
---
sequenceDiagram
    %% a comment
    PARTICIPANT A as Alpha; participant B as Beta
    A->>+B: call # a comment
    B-->>-A: return #35;1
    activate A
    deactivate A
    A ->>B: no space after the arrow
    accTitle: not drawn
```

## Not drawn yet

```mermaid
sequenceDiagram
    A->>B: before
    Note over A,B: a note
```

```mermaid
sequenceDiagram
    loop every minute
        A->>B: ping
    end
```

```mermaid
sequenceDiagram
    A->>B: before
    A->>A: think
```

## Not supported

```mermaid
sequenceDiagram
    actor A
    A->>B: hello
```

```mermaid
sequenceDiagram
    box Aqua Group
        participant A
    end
    A->>B: hello
```

```mermaid
sequenceDiagram
    A->>B: hello
    create participant C
    B->>C: new
```

```mermaid
sequenceDiagram
    A->>B: hello
    destroy B
```

```mermaid
sequenceDiagram
    autonumber
    A->>B: hello
```

```mermaid
sequenceDiagram
    rect rgb(0, 255, 0)
        A->>B: hello
    end
```

```mermaid
sequenceDiagram
    link A: Docs @ https://example.com
    A->>B: hello
```

```mermaid
sequenceDiagram
    links A: {"Docs": "https://example.com"}
    A->>B: hello
```

```mermaid
sequenceDiagram
    properties A: {"class": "x"}
    A->>B: hello
```

```mermaid
sequenceDiagram
    details A: {"Docs": "https://example.com"}
    A->>B: hello
```

```mermaid
sequenceDiagram
    A<<->>B: both ways
```

```mermaid
sequenceDiagram
    A->>B
```

```mermaid
sequenceDiagram
    A->>B: hello
    A--|\B: half arrow
```

```mermaid
sequenceDiagram
    A->>B: hello
    activate C
```

```mermaid
sequenceDiagram
    A->>B: hello
    else nothing to else
```

```mermaid
sequenceDiagram
    A->>B: hello
    end
```

```mermaid
sequenceDiagram
    loop never closed
        A->>B: hello
```

```mermaid
sequenceDiagram
    A->>B: <b>bold</b>
```

```mermaid
sequenceDiagram
    title: Nothing happens
```

## Too large

Thirteen participants:

```mermaid
sequenceDiagram
    A->>B: 1
    B->>C: 2
    C->>D: 3
    D->>E: 4
    E->>F: 5
    F->>G: 6
    G->>H: 7
    H->>I: 8
    I->>J: 9
    J->>K: 10
    K->>L: 11
    L->>M: 12
```

A hundred and fifty-one messages:

```mermaid
sequenceDiagram
    A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x
    A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x
    A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x
    A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x
    A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x
    A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x
    A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x
    A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x
    A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x
    A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x
    A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x
    A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x
    A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x
    A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x
    A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x; A->>B: x
    A->>B: x
```
