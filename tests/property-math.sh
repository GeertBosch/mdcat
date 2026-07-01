#!/bin/sh
# Property: LaTeX math ($...$ inline, $$...$$ block) -> Unicode transliteration.
#
# mdcat does a best-effort transliteration of simple LaTeX onto Unicode: Greek
# letters, common operator/relation symbols, super/subscripts of digits and a
# few characters, and \mathrm/\mathbf style wrappers. Anything it can't map is
# left as the literal source, so the output is never worse than the input.
# Prose dollar signs (prices) must not be mistaken for math.
#
# Each case feeds one line of Markdown to mdcat, strips ANSI CSI and OSC 8
# hyperlink escapes from the output to recover the visible text, and compares it
# to the expected rendering.
#
# Usage: tests/property-math.sh [mdcat-binary]
# Exit status: 0 if every case matches, 1 otherwise.

set -eu

here=$(CDPATH= cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
mdcat=${1:-$root/mdcat}

if [ ! -x "$mdcat" ]; then
    echo "property-math: $mdcat is not an executable; run 'make' first" >&2
    exit 2
fi

# Strip ANSI CSI sequences (ESC[ ... letter) and OSC 8 hyperlink sequences
# (ESC] ... ESC\), leaving only the visible characters. Also drop leading spaces,
# which block math ($$...$$) now carries because it renders centered.
strip() {
    perl -pe 's/\e\][^\e]*\e\\//g; s/\e\[[0-9;]*[A-Za-z]//g; s/^ +//'
}

status=0
fail=0
pass=0

# check NAME INPUT EXPECTED — runs INPUT through mdcat, strips escapes, compares.
check() {
    name=$1
    input=$2
    expected=$3
    got=$(printf '%s\n' "$input" | "$mdcat" | strip)
    if [ "$got" = "$expected" ]; then
        pass=$((pass + 1))
    else
        printf 'property-math: FAIL [%s]\n  input:    %s\n  expected: %s\n  got:      %s\n' \
            "$name" "$input" "$expected" "$got" >&2
        fail=$((fail + 1))
        status=1
    fi
}

# Greek letters, lower and upper case.
check greek-lower '$\alpha \beta \gamma \omega$'   'α β γ ω'
check greek-upper '$\Gamma \Delta \Sigma \Omega$'  'Γ Δ Σ Ω'

# Bare Latin letters render in math italic by default (the Mathematical Italic block); digits,
# operators and Greek stay upright. Italic lowercase 'h' is the Planck constant ℎ (U+210E).
check italic-default '$abc$'                        '𝑎𝑏𝑐'
check italic-upper   '$XYZ$'                        '𝑋𝑌𝑍'
check italic-h       '$h$'                          'ℎ'
check italic-mixed   '$f(x) = ax + b$'             '𝑓(𝑥) = 𝑎𝑥 + 𝑏'

# Common operators and relations (letters now italic, symbols upright).
check ops    '$a \times b \cdot c$'                '𝑎 × 𝑏 ⋅ 𝑐'
check rel    '$x \in S, x \perp y$'                '𝑥 ∈ 𝑆, 𝑥 ⊥ 𝑦'
check quant  '$\forall x \exists y$'               '∀ 𝑥 ∃ 𝑦'
check arrows '$x \to y \longrightarrow z$'         '𝑥 → 𝑦 ⟶ 𝑧'
check sets   '$A \subset B \simeq C$'              '𝐴 ⊂ 𝐵 ≃ 𝐶'

# Superscripts and subscripts: braced and single-character (scripts use the dedicated Unicode
# super/subscript glyphs, which have no italic forms; the scripted base letter is italic).
check super-brace '$10^{3}$'                       '10³'
check super-single '$x^2$'                         '𝑥²'
check sub-brace   '$x_{13}$'                       '𝑥₁₃'
check sub-single  '$H_2$'                          '𝐻₂'
check super-dec   '$10^{1.2}$'                      '10¹·²'

# A typical scientific-notation expression (the motivating case).
check scinot '$7.92 \times 10^{3}$'                '7.92 × 10³'

# \mathrm / \text keep letters upright; \mathbf is bold; \mathit is italic.
check mathrm '$\mathrm{km}$'                       'km'
check mathbf '$\mathbf{v} \cdot \mathbf{w}$'       '𝐯 ⋅ 𝐰'
check mathit '$\mathit{ab}$'                        '𝑎𝑏'

# \mathbb maps letters to blackboard-bold Unicode.
check mathbb-r  '$\mathbb{R}$'                      'ℝ'
check mathbb-in '$x \in \mathbb{Z}$'               '𝑥 ∈ ℤ'

# \sqrt / \frac fall back to explicit grouping parens (dimmed via escapes, which the escape-stripping
# in check() removes, so only the parens themselves are compared here). Parens appear ONLY where the
# flattened form would otherwise misgroup: a top-level sum/difference always; an explicit product or
# division only in a denominator (same strength as the bar). A single term, a numerator product, or
# implicit multiplication (2a) stays bare. The bar '/' is normal foreground (never dimmed) and spaced
# on both sides so it reads as a same-strength operator alongside ⋅.
check sqrt-arg      '$\sqrt{a^2 + b^2}$'            '√(𝑎² + 𝑏²)'
check sqrt-term     '$\sqrt{2}$'                     '√2'
check sqrt-bare     '$\sqrt 2$'                      '√ 2'
check frac-terms    '$\frac{a}{b}$'                  '𝑎 / 𝑏'
check frac-sum      '$\frac{a+b}{c+d}$'             '(𝑎+𝑏) / (𝑐+𝑑)'
check frac-implicit '$\frac{x}{2a}$'                '𝑥 / 2𝑎'
check frac-num-prod '$\frac{5*6}{7}$'               '5*6 / 7'
check frac-den-prod '$\frac{5}{6*7}$'               '5 / (6*7)'
check frac-den-cdot '$\frac{5}{6 \cdot 7}$'         '5 / (6 ⋅ 7)'
check frac-den-div  '$\frac{p}{q/r}$'               '𝑝 / (𝑞/𝑟)'
check frac-pm       '$\frac{-b \pm c}{2a}$'         '(-𝑏 ± 𝑐) / 2𝑎'
check frac-sqrt     '$\frac{1}{\sqrt{2}}$'          '1 / √2'
check frac-nomatch  '$\frac{a}$'                     '\frac{a}'

# \quad / \qquad are wide inter-expression gaps: 2 and 4 columns. A whole math expression is one atom
# that reflow never breaks or respaces, so these interior spaces survive verbatim (no collapse). The
# LaTeX space that must follow a control word (\quad b) is inside the atom too, so it survives as one
# more space: \quad -> 2+1, \qquad -> 4+1.
check quad-space  '$a\quad b$'                      '𝑎   𝑏'
check qquad-space '$a\qquad b$'                     '𝑎     𝑏'

# Block math ($$...$$) is transliterated the same way.
check block '$$E = mc^2$$'                         '𝐸 = 𝑚𝑐²'

# Prose dollar amounts are not math: a digit right after a single-$ closer, or a
# space just inside a delimiter, leaves the dollars literal.
check price     'I paid $5 and $10 today.'         'I paid $5 and $10 today.'
check space-in  'cost $ 5 $ here'                   'cost $ 5 $ here'
check empty     'a $$ b'                            'a $$ b'

# Unknown commands and non-mappable scripts fall back to readable literals.
check unknown-cmd '$\foobar$'                       '\foobar'
check sub-letter  '$x_i$'                           '𝑥_𝑖'

# A command with a braced argument that can't be represented is kept verbatim — the WHOLE group,
# braces included — so a LaTeX-savvy reader still sees the original.
check verbatim-unknown '$\unknown{stuff}$'          '\unknown{stuff}'
check verbatim-mathbb  '$\mathbb{1}$'               '\mathbb{1}'
check verbatim-mathrm  '$\mathrm{\foo}$'            '\mathrm{\foo}'

if [ "$status" -eq 0 ]; then
    printf 'property-math: OK (%d cases)\n' "$pass"
else
    printf 'property-math: %d passed, %d failed\n' "$pass" "$fail" >&2
fi
exit $status
