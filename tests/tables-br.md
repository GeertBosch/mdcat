# TEST: <br> hard breaks in table cells

GitHub renders an inline `<br>` tag inside a table cell as a line break,
stacking the cell over multiple rows. mdcat does the same: a cell with `<br>`
becomes a multi-line cell, the row grows as tall as its tallest cell, and every
other cell aligns to the top of that band.

**Expected:** the `Notes` column wraps onto two or three lines exactly where each
`<br>` sits, the `Piece` and `Value` cells stay on the first line of their band,
and the column rules fall below the full band. The `<br/>` and `<br />` forms
behave identically to `<br>`.

---

| Piece  | Value | Notes                                |
| :----- | ----: | :----------------------------------- |
| Pawn   | 1     | Moves forward<br>captures diagonally |
| Knight | 3     | Jumps in an L<br/>ignores blockers   |
| Queen  | 9     | Moves any distance<br />any direction |
