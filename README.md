# mdcat

A terminal Markdown renderer with inline images, tables, and hyperlinks.
Companion pager `gmore` handles sixel graphics natively.

## Programs

### mdcat

Renders Markdown to the terminal with ANSI styling, sixel images, and OSC 8
hyperlinks.

```
mdcat [--width N] [--img] [--] [file ...]
```

| Flag | Description |
| ---- | ----------- |
| `--width N` / `-w N` | Force render width in columns (overrides `$COLUMNS` and terminal size) |
| `--img` | Emit sixel output even when stdout is not a TTY (for piping into gmore) |
| `--` | End option parsing (allows filenames starting with `-`) |

Reads stdin when no files are given. Multiple files are concatenated.

### gmore

A graphics-aware pager that understands sixel images and OSC 8 hyperlinks.
`mdcat` pipes into it automatically when stdout is a TTY.

```
gmore [--dump] [--dump-images] [file]
```

| Key | Action |
| --- | ------ |
| `Space` / `f` | Page down |
| `b` | Page up |
| `Enter` / `j` | Line down |
| `k` / `y` | Line up |
| `q` | Quit |

## Features

### Headings

Six heading levels with distinct underline styles (heavy → light → dotted →
dashed) so hierarchy is immediately visible. Inline markup works inside
headings: ***bold italic*** and `code` render correctly.

### Inline styling

*Italic*, **bold**, `code spans`, and [hyperlinks](https://example.com) all
render with appropriate terminal styling — markup characters are consumed,
not printed. Hyperlinks use OSC 8 escape sequences, which are clickable in
iTerm2, Kitty, WezTerm, and most modern terminals.

### Tables

GFM tables render with a bold header row, a full-width rule, and column
separators. Long cells wrap within their column; narrow terminals compress
columns gracefully.

| Piece  | Symbol | Value |
| ------ | ------ | ----- |
| Pawn   | P      | 1     |
| Knight | N      | 3     |
| Bishop | B      | 3     |
| Rook   | R      | 5     |
| Queen  | Q      | 9     |

### Images

Inline `<img>` tags render as actual images on sixel-capable terminals
(iTerm2, Kitty, WezTerm, xterm with sixel enabled). Images are scaled to
fit the available column width while preserving aspect ratio. Supported
formats: **PNG**, **JPEG**, **GIF**, **SVG** (via
[timg](https://github.com/hzeller/timg)).

Image paths are resolved relative to the markdown file's directory. On
terminals without sixel support (Apple Terminal, plain xterm, piped output)
the image falls back to its alt text, or the filename if no alt is given.

<img src="tests/chess-piece.png" alt="A black chess knight">

### Block quotes

Block quotes render with a left-rule decoration; nesting is supported.

> A block quote with a left-rule decoration.
>
> > Nested quote one level deeper.
>
> Back to the outer level.

### Lists

Bullet lists use depth-varying glyphs (•, ◦, ▪). Ordered lists honor custom
start numbers. Item bodies reflow and hang under the marker.

- Top level
  - Second level
    - Third level
  - Back to second
- Another top-level item with **bold** and `code` inline

1. Ordered lists work too
2. With correct numbering

### Code blocks

Fenced code blocks render verbatim with a light-gray background panel and no
inline parsing.

```python
def hello():
    print("hello, world")
```

## Requirements

- A C++17 compiler (clang or g++)
- [timg](https://github.com/hzeller/timg) for image rendering
- A sixel-capable terminal for inline images (optional — falls back to text)

## Building

```sh
make
make check   # run the test suite
```

Produces `./mdcat` and `./gmore`.

## Usage examples

```sh
./mdcat README.md                        # render a file
./mdcat --width 80 README.md             # force width
./mdcat --img README.md | ./gmore        # explicit pager
curl -s https://example.com/doc.md | ./mdcat   # render stdin
```
