# Plan: Refactor gmore_core.h into modules

## Context
`gmore_core.h` is a 1,923-line header-only file containing the entire gmore pager
implementation. All code is `static inline` to avoid ODR violations when the file
is included in two translation units: `gmore.cpp` (the standalone pager) and
`mdcat.cpp` (which calls `gmore::run()` when output is a tty). The goal is to split
implementation into `.cpp` files, making headers thin declarations. This will give
faster incremental builds and a clearer module structure.

The only public API is `gmore::run()` — everything else is internal.

## Recommended File Structure

### New files

| File | Contents |
|------|----------|
| `gmore_types.h` | Pure data types: `Attr`, `AttrHash`, `Cell`, `Image`, `Highlight`; type tags/flags; tiny `inline` member functions (`pal`, `tru`, `heightCells`, `pxPerRow`, `Highlight::at`) |
| `gmore_attrs.h` | `extern` declarations of `gAttrs`, `gUris` globals; prototypes for `internAttr`, `internUri`, `sgrFor` |
| `gmore_attrs.cpp` | Defines the four global containers; implements `internAttr`, `internUri`, `sgrFor` (no `static`) |
| `gmore_emulator.h` | `Emulator` class declaration: member types (`St` enum, `Span`), all data member fields, public method prototypes; includes `gmore_types.h` |
| `gmore_emulator.cpp` | All `Emulator` method bodies; sixel helpers (`decodeSixel`, `encodeSixel`, `replaySixel`) and kitty helpers (`kittyB64DecodePrefix`, `kittyPngSize`, `kittyParse`, `kittyTransmitOnly`, `kittyPlace`) as anonymous-namespace functions (no header declarations needed — called only from Emulator methods); includes `gmore_emulator.h`, `gmore_attrs.h` |
| `gmore_run.h` | Single declaration: `int run(std::string, bool, bool, bool, bool, int)` |
| `gmore_run.cpp` | `Nav` struct (file-local), `Search` struct + `findPos` template (file-local; template body stays here since all instantiation sites are in this file), TTY globals (`gTtyFd`, `gSaved`, `gRaw`), TTY functions (`enterRaw`, `restoreTty`, `onSignal`), terminal query functions (`getWinsize`, `queryCsiT`, `queryCellSize`), full `run()` body; includes `gmore_run.h`, `gmore_emulator.h`, `gmore_attrs.h` |
| `gmore.h` | Thin public header — just re-includes `gmore_run.h` (or re-declares `run()`); this is what `mdcat.cpp` and `gmore.cpp` will include |

### Deleted file
- `gmore_core.h`

### Unchanged files (only `#include` line changes)
- `gmore.cpp`: `#include "gmore_core.h"` → `#include "gmore.h"`
- `mdcat.cpp`: `#include "gmore_core.h"` → `#include "gmore.h"`

## Dependency Graph

```
gmore_types.h          (no gmore deps)
    ↑
gmore_attrs.h          (includes gmore_types.h)
gmore_attrs.cpp        (implements gmore_attrs.h)
    ↑
gmore_emulator.h       (includes gmore_types.h)
gmore_emulator.cpp     (includes gmore_emulator.h + gmore_attrs.h;
                        sixel/kitty helpers anonymous-namespace here)
    ↑
gmore_run.h            (no includes — just declares run())
gmore_run.cpp          (includes gmore_run.h, gmore_emulator.h, gmore_attrs.h)
    ↑
gmore.h                (includes gmore_run.h)

mdcat.cpp / gmore.cpp  (include only gmore.h)
```

No cycles.

## Key Design Decisions

**Sixel/kitty helpers stay private in gmore_emulator.cpp** — they have zero callers outside
`Emulator`, so making them separate modules would only add header overhead with no gain.

**Nav, Search, TTY functions stay in gmore_run.cpp** — they are only used by `run()`.
Separating them would create three tiny files whose only consumer is `gmore_run.cpp`.

**`static` keyword removed from all functions/globals** — the workaround was needed only
because everything lived in a shared header. With separate TUs it is wrong.

**`Search::findPos` template stays in `gmore_run.cpp`** — its `GetSpans` parameter is always
a lambda defined at call sites inside the same file, preventing separate instantiation.
A template body only called within one TU can live in the `.cpp`.

## Makefile Changes

```makefile
GMORE_OBJS = gmore_attrs.o gmore_emulator.o gmore_run.o

gmore_attrs.o: gmore_attrs.cpp gmore_attrs.h gmore_types.h
	$(CXX) $(CXXFLAGS) -c -o $@ gmore_attrs.cpp

gmore_emulator.o: gmore_emulator.cpp gmore_emulator.h gmore_attrs.h gmore_types.h
	$(CXX) $(CXXFLAGS) -c -o $@ gmore_emulator.cpp

gmore_run.o: gmore_run.cpp gmore_run.h gmore_emulator.h gmore_attrs.h gmore_types.h
	$(CXX) $(CXXFLAGS) -c -o $@ gmore_run.cpp

mdcat: mdcat.cpp highlight.cpp highlight.h gmore.h $(GMORE_OBJS)
	$(CXX) $(CXXFLAGS) $(PTHREAD) -o $@ mdcat.cpp highlight.cpp $(GMORE_OBJS)

gmore: gmore.cpp gmore.h $(GMORE_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ gmore.cpp $(GMORE_OBJS)
```

## Implementation Sequence

1. Create `gmore_types.h` — copy in all pure structs/enums/constants; keep tiny inline members
2. Create `gmore_attrs.h` — `extern` globals + function prototypes
3. Create `gmore_attrs.cpp` — globals defined once, implement the three functions (no `static`)
4. Create `gmore_emulator.h` — `Emulator` class with data members + public method prototypes
5. Create `gmore_emulator.cpp` — all method bodies + sixel/kitty helpers as anonymous-namespace
6. Create `gmore_run.h` — one-line `run()` declaration
7. Create `gmore_run.cpp` — Nav, Search, TTY state, TTY functions, terminal queries, `run()` body
8. Create `gmore.h` — thin public header
9. Update `mdcat.cpp` and `gmore.cpp` `#include` lines
10. Update `Makefile`
11. Build (`make`), fix any compile errors, run `make check`
12. Delete `gmore_core.h`
13. Commit

## Verification

```sh
make clean && make          # must build both gmore and mdcat without error or warning
make check                  # all tests must pass
echo "# Hello" | ./mdcat   # smoke test mdcat
echo "line one\nline two" | ./gmore  # smoke test gmore
```
