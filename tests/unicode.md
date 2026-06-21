# Unicode width

A visual sample for fullwidth emoji, CJK text and tricky combining sequences.
Render with `./mdcat tests/unicode.md` (and page it with `./mdcat tests/unicode.md
| ./gmore`) — every table column should line up despite the mix of cell widths.

## Wide characters in a table

| Symbol | Name | Width |
|--------|------|-------|
| 😀 | grinning face | 2 cells |
| 🚀 | rocket | 2 cells |
| 🎉 | party popper | 2 cells |
| 中文 | Chinese | 4 cells |
| 한국어 | Korean | 6 cells |
| ASCII | plain text | 1 each |

## Combining marks

These pairs look identical but are encoded differently — a precomposed character
versus a base letter plus a combining mark. Both must occupy the same width.

| Form | Example |
|------|---------|
| Precomposed é | café |
| Base + combining ´ | café |
| Base + ring å | år |
| Stacked diacritics | o̲̅ |

## Hard clusters

A few sequences that famously break naïve width counting:

- ZWJ family (four people, one glyph): 👨‍👩‍👧‍👦
- Regional-indicator flag: 🇯🇵 🇺🇸 🇪🇺
- Skin-tone modifier: 👋🏽 👍🏿
- Emoji with variation selector: ❤️ ☂️

A paragraph that mixes them so reflow has to measure each cluster: the family
👨‍👩‍👧‍👦 waves 👋🏽 under the 🇯🇵 flag while ❤️ floats by, and the text keeps
wrapping cleanly to the terminal width with no column drift at all, none.
