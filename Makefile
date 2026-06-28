CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall
# mdcat's image worker pool (ADR 0003) needs threads; -pthread is appended unconditionally so an
# overridden CXXFLAGS can't drop it.
PTHREAD := -pthread

# Where `make install` copies the binaries. Override with `make install PREFIX=~/.local`.
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

all: mdcat gmore

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

install: mdcat gmore
	mkdir -p $(DESTDIR)$(BINDIR)
	install -m 755 mdcat gmore $(DESTDIR)$(BINDIR)
	@echo "Installed mdcat and gmore to $(DESTDIR)$(BINDIR)"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/mdcat $(DESTDIR)$(BINDIR)/gmore

# Configure gmore as git's pager (global). gmore pages git's colored diff/log output natively,
# including sixel images and OSC 8 hyperlinks. Run after `make install` so gmore is on $PATH.
install-git-pager:
	git config --global core.pager gmore
	@echo "Set git's global core.pager to gmore."

.PHONY: all check clean install uninstall install-git-pager
