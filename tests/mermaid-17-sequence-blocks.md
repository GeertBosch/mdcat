# Built-in Mermaid sequence diagrams: blocks

Step 4 of `docs/plan-mermaid-sequence.md`. Checked by `mermaid-kitty.sh` at width 80.

## One of each keyword

`loop`, `opt` and `break` are frames with a tab for the keyword and their text in
brackets beside it. A frame is as wide as the lifelines its events touch, and two
columns beyond them.

```mermaid
sequenceDiagram
    participant A
    participant B
    loop every minute
        A->>B: ping
        B-->>A: pong
    end
    opt cache is cold
        A->>B: warm up
    end
    break on error
        B--xA: abort
    end
```

## Sections: alt, par, critical

`alt` with two `else` sections, `par` with `and`, and `critical` with `option`.
A divider is a dotted line across the frame with its section's text on the row
below it.

```mermaid
sequenceDiagram
    participant A
    participant B
    participant C
    alt is a hit
        A->>B: read
    else is a miss
        A->>C: fetch
    else is stale
        A->>C: refresh
        C-->>A: new copy
    end
    par to B
        A->>B: one
    and to C
        A->>C: two
    end
    critical connect
        A->>B: open
    option timeout
        A->>A: give up
    option denied
        B-->>A: refused
    end
```

## Nested frames

A loop inside a `par` section, as in the migration protocol: the inner frame is one
column inside the outer one, on both sides, and one row below its header. The second
section's text is long, and wraps where the frame is narrow.

```mermaid
sequenceDiagram
    participant C as Coordinator
    participant D as Donor
    participant R as Recipient
    par Transfer data
        loop Clone
            R->>D: Get batch
            D->>R: Batch
        end
    and Wait for the clone to finish and then stop the writes on the donor
        D->>R: Status?
        R->>D: Progress
    end
    D->>C: Done
```

## A frame over some of the lifelines

Its events touch only the middle two of four participants, so it stops short of the
outer two, and their lifelines pass outside it.

```mermaid
sequenceDiagram
    participant A
    participant B
    participant C
    participant D
    A->>D: begin
    loop poll
        B->>C: status?
        C-->>B: busy
    end
    D->>A: end
```

## A frame with a note

The frame reaches around the note, and a note to the right of the last lifeline
widens the picture. A header text of its own line breaks sits next to the tab.

```mermaid
sequenceDiagram
    participant A
    participant B
    opt first<br/>second
        A->>B: request
        note right of B: remember<br/>this
    end
    alt busy
        note over A,B: both of them
    else idle
        note left of A: left edge
    end
```

## A frame with no text, and an empty section

```mermaid
sequenceDiagram
    participant A
    participant B
    loop
        A->>B: tick
    end
    alt
        A->>B: x
    else
    end
```

## Not drawn

A block with nothing in it has no size in Mermaid either.

```mermaid
sequenceDiagram
    A->>B: hello
    loop nothing
    end
```

```mermaid
sequenceDiagram
    A->>B: hello
    alt nothing in this one
        loop
        end
    end
```

A divider text that would pass in front of an activated lifeline is left to
`overlap`, as a message text is.

```mermaid
sequenceDiagram
    participant A
    participant B
    A->>+B: start
    alt B is busy and its answer takes very long to come
        B-->>A: wait
    end
```
