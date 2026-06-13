# TEST: basic multi-column table

GFM table layout when nothing needs to wrap: the header row is bold with a
full-width rule under it, columns are separated by two spaces, and each cell is
padded to its column width according to the column's delimiter-row alignment.

**Expected:** a clean three-column table at any normal width. The delimiter row
sets per-column alignment: the first column is left-aligned, the middle column
(`:----:`) is centered, and the third column (`----:`) is right-aligned — so the
single-character `Value` cells hug the right edge of their column.

---

| Piece  | Symbol | Value |
| :----- | :----: | ----: |
| Pawn   | P      | 1     |
| Knight | N      | 3     |
| Bishop | B      | 3     |
| Rook   | R      | 5     |
| Queen  | Q      | 9     |
