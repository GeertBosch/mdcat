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

$(B)/gmore_attrs.o: $(S)/gmore_attrs.cpp $(S)/gmore_attrs.h $(S)/gmore_types.h $(S)/unicode_width.h $(S)/unicode_width_table.h | $(B)
	$(CXX) $(CXXFLAGS) -I$(S) -c -o $@ $<

$(B)/gmore_emulator.o: $(S)/gmore_emulator.cpp $(S)/gmore_emulator.h $(S)/gmore_attrs.h $(S)/gmore_types.h $(S)/unicode_width.h $(S)/unicode_width_table.h | $(B)
	$(CXX) $(CXXFLAGS) -I$(S) -c -o $@ $<

$(B)/gmore_run.o: $(S)/gmore_run.cpp $(S)/gmore_run.h $(S)/gmore_emulator.h $(S)/gmore_attrs.h $(S)/gmore_types.h $(S)/unicode_width.h $(S)/unicode_width_table.h $(S)/tty_guard.h | $(B)
	$(CXX) $(CXXFLAGS) -I$(S) -c -o $@ $<

$(B)/highlight.o: $(S)/highlight.cpp $(S)/highlight.h | $(B)
	$(CXX) $(CXXFLAGS) -I$(S) -c -o $@ $<

$(B)/png_write.o: $(S)/png_write.cpp $(S)/png_write.h | $(B)
	$(CXX) $(CXXFLAGS) -I$(S) -c -o $@ $<

MERMAID_OBJS = $(B)/mermaid.o $(B)/mermaid_parse.o $(B)/mermaid_layout.o $(B)/mermaid_draw.o

$(MERMAID_OBJS): $(B)/%.o: $(S)/%.cpp $(S)/mermaid.h $(S)/mermaid_model.h $(S)/mermaid_shape.h $(S)/png_write.h | $(B)
	$(CXX) $(CXXFLAGS) -I$(S) -c -o $@ $<

# mdcat is being split into modules (docs/plan-mdcat-refactor.md). Each module adds its object
# here; every object depends on all mdcat headers (simple and conservative).
MDCAT_HDRS = $(wildcard $(S)/mdcat_*.h)
MDCAT_OBJS = $(B)/mdcat_state.o $(B)/mdcat_text.o $(B)/mdcat_math.o $(B)/mdcat_inline.o $(B)/mdcat_terminal.o $(B)/mdcat_remote.o $(B)/mdcat_protocol.o $(B)/mdcat_image.o $(B)/mdcat_slot_sink.o $(B)/mdcat_mermaid.o $(B)/mdcat_blocks.o

$(MDCAT_OBJS): $(B)/%.o: $(S)/%.cpp $(MDCAT_HDRS) $(S)/unicode_width.h $(S)/unicode_width_table.h $(S)/width_probe.h $(S)/highlight.h $(S)/mermaid.h $(S)/tty_guard.h | $(B)
	$(CXX) $(CXXFLAGS) $(PTHREAD) -I$(S) -c -o $@ $<

$(B)/mdcat: $(S)/mdcat.cpp $(MDCAT_HDRS) $(S)/width_probe.h $(S)/unicode_width.h $(S)/unicode_width_table.h $(S)/highlight.h $(S)/mermaid.h $(S)/gmore_run.h $(S)/tty_guard.h $(MDCAT_OBJS) $(GMORE_OBJS) $(B)/highlight.o $(MERMAID_OBJS) $(B)/png_write.o | $(B)
	$(CXX) $(CXXFLAGS) $(PTHREAD) -I$(S) -o $@ $(S)/mdcat.cpp $(MDCAT_OBJS) $(B)/highlight.o $(MERMAID_OBJS) $(B)/png_write.o $(GMORE_OBJS)

$(B)/gmore: $(S)/gmore.cpp $(S)/width_probe.h $(S)/unicode_width.h $(S)/unicode_width_table.h $(S)/tty_guard.h $(S)/gmore_run.h $(GMORE_OBJS) | $(B)
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
	tests/property-provenance.sh \
	tests/property-image-assets.sh \
	tests/unicode-width.sh \
	tests/mdcat-kitty.sh \
	tests/mermaid-kitty.sh \
	tests/mdcat-remote-image.sh \
	tests/gmore-emulator.sh \
	tests/gmore-sixel.sh \
	tests/gmore-options.sh \
	tests/gmore-nav.sh \
	tests/gmore-search.sh \
	tests/gmore-repaint.sh \
	tests/gmore-links.sh \
	tests/gmore-winch.sh

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
	$(S)/mermaid.cpp \
	$(S)/mermaid.h \
	$(S)/mermaid_draw.cpp \
	$(S)/mermaid_layout.cpp \
	$(S)/mermaid_model.h \
	$(S)/mermaid_parse.cpp \
	$(S)/mermaid_shape.h \
	$(S)/png_write.cpp \
	$(S)/png_write.h \
	$(S)/tty_guard.h \
	$(S)/unicode_width.h \
	$(S)/width_probe.h \
	$(S)/mdcat.cpp \
	$(S)/mdcat_math.cpp \
	$(S)/mdcat_inline.cpp \
	$(S)/mdcat_math.h \
	$(S)/mdcat_inline.h \
	$(S)/mdcat_state.cpp \
	$(S)/mdcat_state.h \
	$(S)/mdcat_style.h \
	$(S)/mdcat_text.cpp \
	$(S)/mdcat_terminal.cpp \
	$(S)/mdcat_terminal.h \
	$(S)/mdcat_remote.cpp \
	$(S)/mdcat_remote.h \
	$(S)/mdcat_protocol.cpp \
	$(S)/mdcat_protocol.h \
	$(S)/mdcat_pool.h \
	$(S)/mdcat_image.cpp \
	$(S)/mdcat_image.h \
	$(S)/mdcat_mermaid.cpp \
	$(S)/mdcat_mermaid.h \
	$(S)/mdcat_blocks.cpp \
	$(S)/mdcat_blocks.h \
	$(S)/mdcat_slot_sink.cpp \
	$(S)/mdcat_slot_sink.h \
	$(S)/mdcat_text.h \
	$(S)/gmore.cpp

setup-clang-format:
	python3 -m venv $(CLANG_FORMAT_VENV)
	$(CLANG_FORMAT_VENV)/bin/python -m pip install --upgrade pip
	$(CLANG_FORMAT_VENV)/bin/python -m pip install $(CLANG_FORMAT_PIP_SPEC)

setup-git-hooks:
	git config core.hooksPath .githooks
	@echo "Configured Git hooks at .githooks."

setup-hooks: setup-git-hooks

setup: setup-clang-format setup-git-hooks

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
	$(S)/mermaid.cpp \
	$(S)/mermaid_draw.cpp \
	$(S)/mermaid_layout.cpp \
	$(S)/mermaid_parse.cpp \
	$(S)/png_write.cpp \
	$(S)/mdcat.cpp \
	$(S)/mdcat_math.cpp \
	$(S)/mdcat_inline.cpp \
	$(S)/mdcat_state.cpp \
	$(S)/mdcat_terminal.cpp \
	$(S)/mdcat_remote.cpp \
	$(S)/mdcat_protocol.cpp \
	$(S)/mdcat_image.cpp \
	$(S)/mdcat_mermaid.cpp \
	$(S)/mdcat_blocks.cpp \
	$(S)/mdcat_slot_sink.cpp \
	$(S)/mdcat_text.cpp \
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

.PHONY: all check clean install uninstall install-git-pager compile_commands.json format format-check setup setup-clang-format setup-git-hooks setup-hooks format-tool-check
