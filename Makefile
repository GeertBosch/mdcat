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

all: $(B)/mdcat $(B)/gmore

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

$(B)/mdcat: $(S)/mdcat.cpp $(S)/highlight.h $(S)/gmore.h $(GMORE_OBJS) $(B)/highlight.o | $(B)
	$(CXX) $(CXXFLAGS) $(PTHREAD) -I$(S) -o $@ $(S)/mdcat.cpp $(B)/highlight.o $(GMORE_OBJS)

$(B)/gmore: $(S)/gmore.cpp $(S)/gmore.h $(GMORE_OBJS) | $(B)
	$(CXX) $(CXXFLAGS) -I$(S) -o $@ $(S)/gmore.cpp $(GMORE_OBJS)

# Convenience symlinks in the project root so tests can run ./mdcat and ./gmore
mdcat: $(B)/mdcat
	ln -sf $(B)/mdcat $@

gmore: $(B)/gmore
	ln -sf $(B)/gmore $@

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

check: mdcat gmore
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

clean:
	rm -rf $(B) mdcat gmore

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

.PHONY: all check clean install uninstall install-git-pager
