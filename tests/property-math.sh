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
# (ESC] ... ESC\), leaving only the visible characters.
strip() {
    perl -pe 's/\e\][^\e]*\e\\//g; s/\e\[[0-9;]*[A-Za-z]//g'
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

# Common operators and relations.
check ops    '$a \times b \cdot c$'                'a × b ⋅ c'
check rel    '$x \in S, x \perp y$'                'x ∈ S, x ⊥ y'
check quant  '$\forall x \exists y$'               '∀ x ∃ y'
check arrows '$x \to y \longrightarrow z$'         'x → y ⟶ z'
check sets   '$A \subset B \simeq C$'              'A ⊂ B ≃ C'

# Superscripts and subscripts: braced and single-character.
check super-brace '$10^{3}$'                       '10³'
check super-single '$x^2$'                         'x²'
check sub-brace   '$x_{13}$'                       'x₁₃'
check sub-single  '$H_2$'                          'H₂'
check super-dec   '$10^{1.2}$'                      '10¹·²'

# A typical scientific-notation expression (the motivating case).
check scinot '$7.92 \times 10^{3}$'                '7.92 × 10³'

# \mathrm / \mathbf strip the styling and keep the contents.
check mathrm '$\mathrm{km}$'                       'km'
check mathbf '$\mathbf{v} \cdot \mathbf{w}$'       'v ⋅ w'

# \mathbb maps letters to blackboard-bold Unicode.
check mathbb-r  '$\mathbb{R}$'                      'ℝ'
check mathbb-in '$x \in \mathbb{Z}$'               'x ∈ ℤ'

# Block math ($$...$$) is transliterated the same way.
check block '$$E = mc^2$$'                         'E = mc²'

# Prose dollar amounts are not math: a digit right after a single-$ closer, or a
# space just inside a delimiter, leaves the dollars literal.
check price     'I paid $5 and $10 today.'         'I paid $5 and $10 today.'
check space-in  'cost $ 5 $ here'                   'cost $ 5 $ here'
check empty     'a $$ b'                            'a $$ b'

# Unknown commands and non-mappable scripts fall back to readable literals.
check unknown-cmd '$\foobar$'                       '\foobar'
check sub-letter  '$x_i$'                           'x_i'

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
