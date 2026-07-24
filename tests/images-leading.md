# TEST: headings/paragraphs that start with an image

A heading or paragraph whose text STARTS with a Markdown image, e.g.
`# ![Logo](logo.png) Title`, renders as a two-column layout: the image on the
left, the rest of the text vertically centered to its right — instead of
falling back to alt text (the old behavior for any image mixed with other
text). The image is capped to a few text-lines' height so a large logo cannot
balloon a single heading into a huge block.

**Expected on a graphics-capable TTY:** each `#`/`##` heading below shows the
chess-knight image on the left with its title text beside it, and the
standalone paragraph likewise shows the image with its blurb wrapped beside
it. None of the three images should be visibly taller than about 3 text lines.

**Expected on Apple_Terminal or any piped run:** no image is drawn; each
heading/paragraph falls back to plain text, with the alt text ("knight")
run into the title/blurb the same way any other mixed image+text renders.
Force this path with
`TERM_PROGRAM=Apple_Terminal ./mdcat tests/images-leading.md`.

---

# ![knight](chess-piece.png) A Heading With A Logo

## ![knight](chess-piece.png) A Level-2 Heading With A Logo

![knight](chess-piece.png) A paragraph that starts with an image, followed by
enough text to wrap onto a second line beside it.

A plain heading and paragraph, for comparison — no image, ordinary rendering:

## Plain Heading, No Image

Plain paragraph, no image, for comparison.
