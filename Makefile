CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall

all: mdcat gmore

mdcat: mdcat.cpp highlight.cpp highlight.h gmore_core.h
	$(CXX) $(CXXFLAGS) -o $@ mdcat.cpp highlight.cpp

gmore: gmore.cpp gmore_core.h
	$(CXX) $(CXXFLAGS) -o $@ gmore.cpp

TESTS := \
	tests/property-concat.sh \
	tests/property-width.sh \
	tests/property-blank-lines.sh \
	tests/property-highlight-escapes.sh \
	tests/property-escapes.sh \
	tests/property-hard-breaks.sh \
	tests/property-math.sh \
	tests/unicode-width.sh \
	tests/gmore-emulator.sh \
	tests/gmore-sixel.sh \
	tests/gmore-nav.sh \
	tests/gmore-search.sh \
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
	rm -f mdcat gmore

.PHONY: all check clean
