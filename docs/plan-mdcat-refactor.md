# Plan: Split mdcat.cpp into modules

## Context

`src/mdcat.cpp` is 5,336 lines (as of 605d898): every part of the renderer sits in one
anonymous namespace. That includes terminal probing, remote-image trust, LaTeX math, inline
parsing, the image and graphics-protocol plumbing, mermaid embedding, block layout, tables,
the block parser, the output sink, and `main`. The goal is to split it into a set of `.cpp`
files with thin headers, the same way `gmore_core.h` was split (see `plan-gmore-refactor.md`).

**This is a pure move. Nothing about behaviour changes.** Every step must leave the output of
`mdcat` byte-for-byte the same. Function bodies are moved verbatim. The only edits allowed are
the ones needed to compile across translation units: declarations, `#include`s, `extern`,
removing default arguments from definitions, and removing forward declarations that are no
longer needed.

The steps are sized so that each one moves a single module (≤ ~700 lines) and fits in one
Sonnet session: extract, build, verify, commit.

## Target structure

Line ranges refer to `src/mdcat.cpp` at 605d898. They will shift as earlier steps remove code,
so **find each section by its function names, not by line number**. The ranges show where a
section starts and how big it is.

| File | Contents (from mdcat.cpp) | ~Lines |
|------|---------------------------|-------:|
| `mdcat_style.h` | Header only. `kEsc`, `kMathAtomStart/End`, `kBoldOn/Off`, `kItalicOn/Off`, `kCodeOff`, `kReset`, `kQuoteBar`, `kMathGroupOn/Off` as `inline const`. `extern std::string kCodeOn, kLightGray;` (mutable: set by `initTheme`) (83–121) | 45 |
| `mdcat_state.h/.cpp` | Option and render-context globals: `gWidthOverride`, `gForceGraphics`, `gForcedBackend`, `MermaidMode` + `gMermaidMode`, `gFileDir`, `gSourceName`, `gBlockLine`, `gOutputFile` (123–157), `gIndent`, `gListDepth`, `gListNesting`, `gTightItem`, `gRenderWidth`, `gEmitProvenance` (374–415). Header: `extern` declarations with their comments. `.cpp`: definitions | 90 |
| `mdcat_text.h/.cpp` | UTF-8 and display-width utilities (917–1046: `utf8SequenceLength`, `decodeUtf8`, `displayWidth`, `horizontalLine`, `padTo`, `Align`, `padAligned`, `trim`), `appendUtf8` (1269), `toLowerCopy` (177), `isSpaceCh` (2197), reflow machinery (3370–3569: `unitLength`, `closeStylesAtLineBreaks`, `reflow`, `stripMathAtoms`), `splitOnNewlines` (3948) | 420 |
| `mdcat_math.h/.cpp` | LaTeX → Unicode and stacked math (1047–1772). Public: `MathStyle`, `renderMath`, `MathBox`, `renderMathStacked`. Everything else goes in an anonymous namespace | 725 |
| `mdcat_inline.h/.cpp` | `InlineRenderer` (anonymous namespace) and the public `renderInline` (1773–2149) | 380 |
| `mdcat_terminal.h/.cpp` | Width and terminal probes (417–916: `fullTerminalWidth`, `terminalWidth`, `GraphicsBackend`, `probeGraphics`, `graphicsBackend`, `terminalSupportsGraphics`, `mmdcAvailable`, `CellMetrics`, `cellMetrics`, `queryWindowOp`, `queryBackgroundColor`, `darkBackground`, `initTheme`, `envInt`), `isITerm` and `imageXOffset` (3211–3227), and the definitions of `kCodeOn` / `kLightGray`. `ProbeResult`, `probeGraphics`, `TwoInts` and `queryWindowOp` stay private | 520 |
| `mdcat_remote.h/.cpp` | Remote-image trust and fetch (159–372). Public: `curateRemoteImageUrl`, `fetchRemoteImage`. The rest is private | 215 |
| `mdcat_protocol.h/.cpp` | Pure byte handling for the Kitty and sixel graphics protocols: `nextKittyId`, `kKittyIdDefer`, `kittyRewriteId`, `kittyIsFirstChunk` (private), `kittyRenumberAll`, `kittyRewriteFootprint`, `kittyQ2`, `pngSize`, `kittyImageSize`, `sixelPixelSize` (2494–2737); `isSixelImage`, `isKittyImage`, `isImageBlock` (3143–3158); `base64Encode` (3193); the Kitty APC builder for a PNG (3228–3256) | 340 |
| `mdcat_pool.h` | Header only. The `ThreadPool` class (2742–2799) and a declaration of `ThreadPool& imagePool();` | 65 |
| `mdcat_image.h/.cpp` | Image tags and timg (2150–2493: `isSupportedImageExt`, `readImageSize`, `pngWidth`, `scanMdImageAt`, `parseMdImage`, `parseImgTag`, `completeGeom`, `runTimg`, `runTimgKitty`), the definition of `imagePool()` (2800–2807), `renderImageBlock` (2808–2999), `replaySixel`, `imageParagraphBytes`, `emitImageParagraph` (3159–3192) | 620 |
| `mdcat_slot_sink.h/.cpp` | `SlotSink` (4904–5068) with `asSlotSink` / `slotDeferImage` (5069–5075). The class goes in the header, out-of-line bodies in the `.cpp`. Delete the forward-declaration block at 3302–3309 | 175 |
| `mdcat_mermaid.h/.cpp` | `renderMermaidBlock` (3000–3142), the diagram framing helper (3257–3301), `emitBuiltinMermaid` (3311–3369), `mermaidMarkdownLines` (3962–3992) | 330 |
| `mdcat_blocks.h/.cpp` | `emitParagraph`, `renderLeadingImageBlock`, `emitHeading`, `emitThematicBreak` (3570–3887) | 320 |
| `mdcat_table.h/.cpp` | `isTableDelimiterRow`, `splitTableRow`, `parseTableAlignment`, `emitTable` (3888–4300, without the `splitOnNewlines` and `mermaidMarkdownLines` pieces moved earlier) | 380 |
| `mdcat_render.h/.cpp` | The block parser and document driver (4301–4903): block predicates, list-marker parsing, `emitList`, `emitBlockQuote`, `render`, `splitLines`. Public: `render`, `splitLines`, and whatever `emitTable` and `mdcat.cpp` need. Everything else is private | 600 |
| `mdcat.cpp` | `usage`, `error`, `parseMermaidMode`, `parseOptions`, `renderPaged`, `renderAll`, `main` | 270 |

The `mdcat_` prefix follows the existing `gmore_*` and `mermaid_*` naming.

### Dependency graph (headers)

```
mdcat_style.h  mdcat_state.h  mdcat_pool.h            (leaves)
      ↑              ↑
mdcat_text.h ← mdcat_math.h ← mdcat_inline.h
mdcat_terminal.h   mdcat_remote.h   mdcat_protocol.h
      ↑                 ↑                ↑
      └───────── mdcat_image.h ──────────┘      mdcat_slot_sink.h (→ protocol)
                       ↑                               ↑
                 mdcat_mermaid.h ──────────────────────┘
                       ↑
          mdcat_blocks.h   mdcat_table.h
                       ↑
                 mdcat_render.h
                       ↑
                    mdcat.cpp
```

Headers only declare. A `.cpp` may include whatever it needs, so the mutual recursion between
`render` and `emitList`/`emitBlockQuote` stays inside `mdcat_render.cpp`. Calls that go "up"
(for example `emitTable` → `renderImageBlock`, `emitBuiltinMermaid` → `slotDeferImage`) are
`.cpp` includes and never create header cycles. If a header seems to need another header just
for a type in a signature, forward-declare the type instead.

## Conventions (apply in every step)

1. **Namespace.** All mdcat code lives in `namespace mdcat { ... }`. A helper used by only one
   `.cpp` goes in an anonymous namespace inside it (or keeps its existing `static`). The other
   libraries are already namespaced (`gmore::`, `mermaid::`, `uw::`), so they can't clash.
2. **Move code with a script, don't retype it.** Pull each range out with `sed -n 'A,Bp'` (or a
   small Python script that cuts between two marker lines) into the new file, then delete it
   from `mdcat.cpp` the same way. Retyping a 400-line function is slow and lets drift in. The
   moved code, including its comments, must be byte-identical.
3. **Comments move with the code.** A function's doc comment goes into the header above the
   declaration. Leave a one-line pointer above the definition only if it helps.
4. **Default arguments appear only in the header declaration.** Remove them from the definition
   (for example `renderMath`'s `style = MathStyle::Italic` and `renderImageBlock`'s `kittyId`).
5. **Constants.** Immutable `std::string`/`char` constants become `inline const` /
   `inline constexpr` in the header (C++17). Mutable globals become `extern` in the header and
   are defined exactly once in a `.cpp`.
6. **Function-local `static` caches** (`graphicsBackend`, `cellMetrics`, `trustedHosts`, and so
   on) move with their function unchanged. Don't hoist them.
7. **Delete forward declarations** once they're redundant (`// defined below` declarations and
   the `class SlotSink;` block). Resolve them by including the right header.
8. **Each new `.cpp` includes its own header first**, then the standard headers it uses, then
   the project headers. Don't copy the whole 30-line include list from mdcat.cpp. Include
   what's used. At the end, trim `mdcat.cpp`'s includes the same way.
9. **Makefile.** Use one pattern rule for the mdcat objects, and make every mdcat object depend
   on all mdcat headers (simple and conservative):

   ```makefile
   MDCAT_HDRS = $(wildcard $(S)/mdcat_*.h)
   MDCAT_OBJS = $(B)/mdcat_state.o $(B)/mdcat_text.o ...   # grows each step

   $(MDCAT_OBJS): $(B)/%.o: $(S)/%.cpp $(MDCAT_HDRS) $(S)/unicode_width.h $(S)/unicode_width_table.h \
                  $(S)/width_probe.h $(S)/highlight.h $(S)/mermaid.h $(S)/tty_guard.h | $(B)
   	$(CXX) $(CXXFLAGS) $(PTHREAD) -I$(S) -c -o $@ $<
   ```

   Add `$(MDCAT_OBJS)` (and `$(MDCAT_HDRS)`) to the `$(B)/mdcat` prerequisites and link line.
   Add each new file to **`FMT_SRCS`** (headers and `.cpp`), and each new `.cpp` to **`SRCS`**
   (used for `compile_commands.json`; mdcat files already get `$(PTHREAD)` there via the
   `*mdcat*` case).
10. **Formatting.** Run `make format` with the pinned clang-format (`make setup-clang-format`
    once). `make check` runs `format-check` first, and CI uses clang-format 20.
11. **No new warnings.** Build with the default `-Wall`. If you get an `unused function`
    warning, the function is in the wrong file or it's dead code. Don't silence it: move it, or
    mention it in the commit for a separate follow-up.

## Verification harness (step 0, reused by every step)

Run this once before any code moves, and save it as `build/snapshot.sh`. `build/` is
gitignored, so the script isn't committed.

```sh
#!/bin/bash
# usage: build/snapshot.sh OUTDIR   — render every sample in several modes into OUTDIR
set -u
out=$1; rm -rf "$out"; mkdir -p "$out"
unset MDCAT_GRAPHICS MDCAT_MERMAID MDCAT_WIDTH
for f in README.md tests/*.md; do
  b=$(basename "$f" .md)
  for w in 40 80 120; do
    ./mdcat -w $w --img=none "$f" > "$out/$b.w$w.none" 2>&1
  done
  ./mdcat -w 100 --img=kitty --mermaid builtin "$f" > "$out/$b.kitty"  2>&1
  ./mdcat -w 100 --img=sixel --mermaid source  "$f" > "$out/$b.sixel"  2>&1
done
./mdcat -w 80 --img=none tests/headings.md tests/lists.md > "$out/multi" 2>&1
```

Step 0:

1. `make` on a clean tree, then `build/snapshot.sh build/golden` twice (to `build/golden` and
   `build/golden2`) and `diff -r` the two. Any file that differs between runs of the *same*
   binary is nondeterministic (for example a remote badge fetch). Delete it from
   `build/golden` and note it, so later steps don't chase false alarms.
2. Record a performance baseline. Splitting translation units stops cross-file inlining of hot
   helpers (`displayWidth`, `unitLength`, `decodeUtf8`), and this project has been bitten by
   performance bugs before:

   ```sh
   for i in $(seq 50); do cat README.md; done > build/big.md
   time ./mdcat -w 100 --img=none build/big.md > /dev/null
   ```

**After every step:**

```sh
make 2>&1 | grep -i warning          # must print nothing
build/snapshot.sh build/now && diff -r build/golden build/now   # must print nothing
make check                           # all ✅
```

Any diff means the step changed behaviour: fix it before committing. Re-time `build/big.md` at
step 8 and at the end. A slowdown of more than ~10% is worth investigating. Usually the fix is
to move a tiny hot helper into its header as `inline`.

## Steps

Work on a branch (`git switch -c split-mdcat`). After each step, verify as above and make one
commit with a single-line message of at most 80 characters and no co-author line. Don't push
until the final step.

### Step 0: Baseline
Set up `build/snapshot.sh`, `build/golden` and the timing baseline, as described above. No
commit.

### Step 1: Named namespace, Makefile scaffolding
- In `mdcat.cpp`, rename `namespace {` to `namespace mdcat {` and the closing comment to
  `}  // namespace mdcat`. Put `using namespace mdcat;` just above `int main`. (For now main
  stays outside the namespace.)
- Add the `MDCAT_HDRS` / `MDCAT_OBJS` machinery to the Makefile with an empty `MDCAT_OBJS`.
  Wire it into the `$(B)/mdcat` rule.
- Verify. Commit: `mdcat: move into named namespace, add Makefile scaffolding for split`.

### Step 2: `mdcat_style.h` + `mdcat_state.h/.cpp`
- Create `mdcat_style.h` with the immutable style constants and `extern` declarations for
  `kCodeOn` and `kLightGray`. For now, define those two in `mdcat_state.cpp`. Step 6 moves them
  next to `initTheme`.
- Create `mdcat_state.h` with `MermaidMode` and the `extern` globals, and `mdcat_state.cpp` with
  their definitions and initializers. Keep every comment.
- Have `mdcat.cpp` include both headers.

### Step 3: `mdcat_text.h/.cpp`
Move the text utilities listed in the table. `codePointWidth` comes from
`using uw::codePointWidth;`. Keep that using-declaration inside `namespace mdcat` in the
`.cpp`, and in the header only if a signature needs it. `Align` is a public type in the header.

### Step 4: `mdcat_math.h/.cpp`
Move the LaTeX section and the stacked-math section. The public API is `MathStyle`,
`renderMath` (with its default argument in the header), `MathBox`, and `renderMathStacked`.
Delete the forward declaration of `renderMath` (~line 1392). `superscripts`, `mathSymbols`,
`convertScript`, `fracBox`, `hcat` and the other helpers go in the anonymous namespace. If
`InlineRenderer` (still in mdcat.cpp) turns out to use one of them directly, make that one
public.

### Step 5: `mdcat_inline.h/.cpp`
Move `InlineRenderer` (anonymous namespace) and `renderInline` (public). Check whether
`InlineRenderer` calls the image-syntax scanner (`scanMdImageAt`/`isSpaceCh`). If it does,
temporarily declare what it needs in a header that already exists. Step 9 moves those
functions into `mdcat_image`.

### Step 6: `mdcat_terminal.h/.cpp`
Move the width, probe, theme and cell-metrics section plus `isITerm`/`imageXOffset`, and move
the `kCodeOn`/`kLightGray` definitions here from `mdcat_state.cpp`. `GraphicsBackend` and
`CellMetrics` are public types. `renderPaged` and `main` call `fullTerminalWidth`,
`graphicsBackend`, `cellMetrics`, `queryBackgroundColor` and `initTheme`, so those must be
public.

### Step 7: `mdcat_remote.h/.cpp`
Move the remote-image trust section. Public: `curateRemoteImageUrl`, `fetchRemoteImage`.
`TrustEntry`, `gitRemoteHost`, `trustEntryMatches` and `trustedHosts` are private.
`tests/mdcat-remote-image.sh` exercises this module, so make sure it stays ✅.

### Step 8: `mdcat_protocol.h/.cpp` + `mdcat_pool.h`
- Move the pure Kitty/sixel/PNG byte functions listed in the table. These have the strictest
  correctness and performance requirements: `kittyRenumberAll` and `kittyQ2` must stay linear
  (see commit 605d898). Move them verbatim.
- Move `ThreadPool` into the header-only `mdcat_pool.h` with `ThreadPool& imagePool();`
  declared. For now the definition of `imagePool()` stays in mdcat.cpp. Step 9 moves it.
- `tests/mdcat-kitty.sh` and `tests/mermaid-kitty.sh` cover this code.
- Re-time `build/big.md`.

### Step 9: `mdcat_image.h/.cpp`
Move the image-tag parsing, timg runners, `imagePool()` definition, `renderImageBlock`, and the
replay/emit helpers. `renderImageBlock`'s `kittyId` default goes in the header only. Public:
`parseMdImage`, `parseImgTag`, `renderImageBlock`, `replaySixel`, `imageParagraphBytes`,
`emitImageParagraph`, plus anything that `emitTable`, `emitParagraph` or mermaid still use in
mdcat.cpp. The build will tell you. `scanMdImageAt` stays `static` unless the inline renderer
needs it.

### Step 10: `mdcat_slot_sink.h/.cpp`
Put the `SlotSink` class definition in the header. Move the larger method bodies to the
`.cpp`, or leave them in-class if they're short. Move the `asSlotSink`/`slotDeferImage`
wrappers too, and delete the forward-declaration block (`class SlotSink;` and the two
prototypes). Every call site that used the wrappers now includes `mdcat_slot_sink.h`. Keep the
wrappers rather than rewriting call sites, so the diff stays small.

### Step 11: `mdcat_mermaid.h/.cpp`
Move `renderMermaidBlock`, the diagram framing helper, `emitBuiltinMermaid` and
`mermaidMarkdownLines`. Delete `mermaidMarkdownLines`'s forward declaration. Public:
`renderMermaidBlock`, `emitBuiltinMermaid`. `mermaidMarkdownLines` is public only if something
outside this file uses it. Check `tests/mermaid-*.sh` and the `tests/expected/mermaid-*`
goldens.

### Step 12: `mdcat_blocks.h/.cpp`
Move `emitParagraph`, `renderLeadingImageBlock`, `emitHeading` and `emitThematicBreak`. Delete
the `// defined below` forward declarations above `emitParagraph`. The `kittyId` default
argument of `renderLeadingImageBlock` goes in the header.

### Step 13: `mdcat_table.h/.cpp`
Move `isTableDelimiterRow`, `splitTableRow`, `parseTableAlignment` and `emitTable`. Public:
`emitTable` and `isTableDelimiterRow` (the block parser uses it to detect tables). Anything
else the parser calls also needs to be public.

### Step 14: `mdcat_render.h/.cpp`
Move everything from `trimTrailingNewline` through `splitLines`. Public: `render` and
`splitLines`. The block predicates, `ListMarker`, `ListItem`, `emitList` and `emitBlockQuote`
go in the anonymous namespace. If `emitTable` calls `render` recursively for cell content, or
uses a predicate from here, declare that function in `mdcat_render.h` and include the header
from `mdcat_table.cpp`.

### Step 15: Tidy `mdcat.cpp`
- What's left should be `usage`, `error`, `parseMermaidMode`, `parseOptions`, `renderPaged`,
  `renderAll` and `main`. Put the first six in an anonymous namespace inside `namespace mdcat`.
  Keep `using namespace mdcat;` for `main`, or call through `mdcat::` instead.
- Cut the include list down to what's actually used.
- Rewrite the file's header comment (lines 1–43). Keep the program overview and usage, and add
  a short "Source map" paragraph listing each `mdcat_*` module in one line.
- Verify, including the timing check.

### Step 16: Docs, memory, CI, push
- Update references to `mdcat.cpp` that now point at code that moved: the comment in
  `tests/property-provenance.sh` (`gEmitProvenance` → `mdcat_state.h`), comments in
  `src/gmore_run.cpp`, `docs/gmore-rendering-notes.md`, and `docs/adr/0003-parallel-image-jobs.md`
  (SlotSink → `mdcat_slot_sink.*`). `grep -rn 'mdcat\.cpp' src tests docs` finds them. Leave the
  historical plan documents as they are.
- Update the memories that mention the single-file layout (`mdcat-overview.md`, and any others
  that name `mdcat.cpp` as the home of a function).
- CI reproduction (CLAUDE.md): on `ssh orb`, do a fresh clone, check out the tip,
  `LANG=C.UTF-8 LC_ALL=C.UTF-8 make setup-clang-format && make check`. GCC on Ubuntu is
  stricter than Apple clang about missing includes (`<cstdint>`, `<functional>`, `<cstring>`),
  so expect a few fixes. Amend them into the step that introduced the file, or add a fixup
  commit if that's easier.
- Rebase onto `main`, and hand the user the `git merge --ff-only split-mdcat` command.

## Risks and how the plan handles them

| Risk | Mitigation |
|------|------------|
| A small behaviour change slips in during the move | Moves are scripted (verbatim), and every step is checked against the golden snapshot across widths and image backends |
| Hot helpers lose inlining across TUs | Timing checks at step 8 and the end. If needed, move tiny helpers into headers as `inline`. `-flto` is a last resort (it complicates the build) |
| ODR or duplicate-definition link errors from header constants | `inline const` for constants, `extern` and a single definition for mutable globals |
| Silent shadowing: a new `mdcat::trim` vs. some other `trim` | All libraries are namespaced, and `-Wall` plus link errors surface real clashes |
| Linux/GCC include strictness | Explicit includes per file, and the `ssh orb` clean-clone CI run before pushing |
| A step gets too big for one session | Each step is a single module. If a step stalls, commit the module half that builds (for example header and helpers) and leave the rest in mdcat.cpp for the next session |
