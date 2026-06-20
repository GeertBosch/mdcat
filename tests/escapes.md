# TEST: backslash escapes

Per the GFM spec (https://github.github.com/gfm/#backslash-escapes), any ASCII
punctuation character may be backslash-escaped to render as a literal character,
suppressing any special Markdown meaning it would otherwise have. A backslash
before any other character (letters, digits, whitespace, non-ASCII) is left
alone as a literal backslash.

**Expected:** the punctuation line below shows every ASCII punctuation character
once, with no stray backslashes. Escaped markup characters lose their meaning:
no emphasis, code, or links form where the markers are escaped. Backslashes
before non-punctuation survive verbatim.

---

All ASCII punctuation, escaped — should print each character once, no
backslashes:

\!\"\#\$\%\&\'\(\)\*\+\,\-\.\/\:\;\<\=\>\?\@\[\\\]\^\_\`\{\|\}\~

Escaped markers stay literal: \*not emphasized\*, \`not code\`, and
\[not a link\](/foo) all keep their markup characters visible.

A literal asterisk between words a\*b\*c does not start emphasis.

A backslash before a non-punctuation character is itself literal: \a \3 and a
backslash before a space \ stay as written.

Escapes still work where markup is real: *foo\*bar* is italic with a literal
asterisk inside, and \\*foo* is a literal backslash followed by italic foo.

Inside a code span backslashes are literal, not escapes: `\*` `\\` `\]`.
