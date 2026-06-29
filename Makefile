CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall
# mdcat's image worker pool (ADR 0003) needs threads; -pthread is appended unconditionally so an
# overridden CXXFLAGS can't drop it.
PTHREAD := -pthread

# Where `make install` copies the binaries. Override with `make install PREFIX=~/.local`.
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

S := src
B := build

all: $(B)/mdcat $(B)/gmore mdcat gmore

$(B):
	mkdir -p $(B)

GMORE_OBJS = $(B)/gmore_attrs.o $(B)/gmore_emulator.o $(B)/gmore_run.o

$(B)/gmore_attrs.o: $(S)/gmore_attrs.cpp $(S)/gmore_attrs.h $(S)/gmore_types.h | $(B)
	$(CXX) $(CXXFLAGS) -I$(S) -c -o $@ $<

$(B)/gmore_emulator.o: $(S)/gmore_emulator.cpp $(S)/gmore_emulator.h $(S)/gmore_attrs.h $(S)/gmore_types.h | $(B)
	$(CXX) $(CXXFLAGS) -I$(S) -c -o $@ $<

$(B)/gmore_run.o: $(S)/gmore_run.cpp $(S)/gmore_run.h $(S)/gmore_emulator.h $(S)/gmore_attrs.h $(S)/gmore_types.h | $(B)
	$(CXX) $(CXXFLAGS) -I$(S) -c -o $@ $<

$(B)/highlight.o: $(S)/highlight.cpp $(S)/highlight.h | $(B)
	$(CXX) $(CXXFLAGS) -I$(S) -c -o $@ $<

$(B)/mdcat: $(S)/mdcat.cpp $(S)/highlight.h $(S)/gmore_run.h $(GMORE_OBJS) $(B)/highlight.o | $(B)
	$(CXX) $(CXXFLAGS) $(PTHREAD) -I$(S) -o $@ $(S)/mdcat.cpp $(B)/highlight.o $(GMORE_OBJS)

$(B)/gmore: $(S)/gmore.cpp $(S)/gmore_run.h $(GMORE_OBJS) | $(B)
	$(CXX) $(CXXFLAGS) -I$(S) -o $@ $(S)/gmore.cpp $(GMORE_OBJS)

# Hard links in the project root so tests (and ad-hoc use) can run ./mdcat and ./gmore.
# Hard links rather than symlinks so the root copies remain valid even if build/ is removed.
mdcat: $(B)/mdcat
	ln -f $(B)/mdcat $@

gmore: $(B)/gmore
	ln -f $(B)/gmore $@

TESTS := \
	tests/property-concat.sh \
	tests/property-width.sh \
	tests/property-blank-lines.sh \
	tests/property-highlight-escapes.sh \
	tests/property-escapes.sh \
	tests/property-hard-breaks.sh \
	tests/property-math.sh \
	tests/unicode-width.sh \
	tests/mdcat-kitty.sh \
	tests/gmore-emulator.sh \
	tests/gmore-sixel.sh \
	tests/gmore-nav.sh \
	tests/gmore-search.sh \
	tests/gmore-repaint.sh \
	tests/gmore-links.sh

CLANG_FORMAT ?= clang-format
CLANG_FORMAT_REQUIRED_MAJOR ?= 20
CLANG_FORMAT_PIP_SPEC ?= clang-format==20.1.8
CLANG_FORMAT_VENV ?= .tooling/venv
CLANG_FORMAT_PINNED := $(CLANG_FORMAT_VENV)/bin/clang-format

ifneq ($(wildcard $(CLANG_FORMAT_PINNED)),)
CLANG_FORMAT := $(CLANG_FORMAT_PINNED)
endif

FMT_SRCS := \
	$(S)/gmore_attrs.cpp \
	$(S)/gmore_attrs.h \
	$(S)/gmore_emulator.cpp \
	$(S)/gmore_emulator.h \
	$(S)/gmore_run.cpp \
	$(S)/gmore_run.h \
	$(S)/gmore_types.h \
	$(S)/highlight.cpp \
	$(S)/highlight.h \
	$(S)/mdcat.cpp \
	$(S)/gmore.cpp

setup-clang-format:
	python3 -m venv $(CLANG_FORMAT_VENV)
	$(CLANG_FORMAT_VENV)/bin/python -m pip install --upgrade pip
	$(CLANG_FORMAT_VENV)/bin/python -m pip install $(CLANG_FORMAT_PIP_SPEC)

format-tool-check:
	@major=`$(CLANG_FORMAT) --version 2>/dev/null | sed -E 's/.*version ([0-9]+).*/\1/'`; \
	if [ -z "$$major" ]; then \
		echo "clang-format not found at $(CLANG_FORMAT). Run 'make setup-clang-format' or set CLANG_FORMAT."; \
		exit 1; \
	fi; \
	if [ "$$major" != "$(CLANG_FORMAT_REQUIRED_MAJOR)" ]; then \
		echo "clang-format major $$major does not match required $(CLANG_FORMAT_REQUIRED_MAJOR)."; \
		echo "Use 'make setup-clang-format' for the pinned formatter, or set CLANG_FORMAT explicitly."; \
		exit 1; \
	fi

format: format-tool-check
	$(CLANG_FORMAT) -i $(FMT_SRCS)

format-check: format-tool-check
	$(CLANG_FORMAT) --dry-run --Werror $(FMT_SRCS)

check: format-check mdcat gmore
	@fail=0; \
	for t in $(TESTS); do \
		name=$$(basename $$t .sh); \
		if out=$$(./$$t 2>&1); then \
			printf '✅ %s\n' "$$name"; \
		else \
			printf '❌ %s\n' "$$name"; \
			printf '%s\n' "$$out" | sed 's/^/    /'; \
			fail=1; \
		fi; \
	done; \
	exit $$fail

SRCS := \
	$(S)/gmore_attrs.cpp \
	$(S)/gmore_emulator.cpp \
	$(S)/gmore_run.cpp \
	$(S)/highlight.cpp \
	$(S)/mdcat.cpp \
	$(S)/gmore.cpp

ABS := $(abspath .)
CLANGD_TARGET := $(shell $(CXX) -print-target-triple 2>/dev/null)
CLANGD_SYSROOT := $(shell xcrun --show-sdk-path 2>/dev/null)
CLANGD_DB_FLAGS :=
ifneq ($(strip $(CLANGD_TARGET)),)
CLANGD_DB_FLAGS += -target $(CLANGD_TARGET)
endif
ifneq ($(strip $(CLANGD_SYSROOT)),)
CLANGD_DB_FLAGS += -isysroot $(CLANGD_SYSROOT)
endif

compile_commands.json: Makefile
	@printf '[\n' > $@
	@first=1; \
	for f in $(SRCS); do \
		flags="$(CXXFLAGS) $(CLANGD_DB_FLAGS) -I$(ABS)/$(S)"; \
		case $$f in *mdcat*) flags="$$flags $(PTHREAD)";; esac; \
		[ $$first -eq 0 ] && printf ',\n' >> $@; \
		printf '  { "directory": "%s", "file": "%s/%s", "command": "$(CXX) %s -c %s/%s" }' \
			"$(ABS)" "$(ABS)" "$$f" "$$flags" "$(ABS)" "$$f" >> $@; \
		first=0; \
	done; \
	printf '\n]\n' >> $@
	@echo "wrote compile_commands.json"

clean:
	rm -rf $(B)

install: $(B)/mdcat $(B)/gmore
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 755 $(B)/mdcat $(B)/gmore $(DESTDIR)$(BINDIR)
	@echo "Installed mdcat and gmore to $(DESTDIR)$(BINDIR)"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/mdcat $(DESTDIR)$(BINDIR)/gmore

# Configure gmore as git's pager (global). gmore pages git's colored diff/log output natively,
# including sixel images and OSC 8 hyperlinks. Run after `make install` so gmore is on $PATH.
install-git-pager:
	git config --global core.pager gmore
	@echo "Set git's global core.pager to gmore."

.PHONY: all check clean install uninstall install-git-pager compile_commands.json format format-check setup-clang-format format-tool-check
