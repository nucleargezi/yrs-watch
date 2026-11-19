CXX ?= g++
-include build/local-deps.mk
WATCH_CPPFLAGS ?= $(shell pkg-config --cflags gio-2.0 json-glib-1.0)
WATCH_LIBS ?= $(shell pkg-config --libs gio-2.0 json-glib-1.0)
FONT_CPPFLAGS ?= $(shell pkg-config --cflags pangocairo)
FONT_LIBS ?= $(shell pkg-config --libs pangocairo)
override CPPFLAGS += -Isrc $(WATCH_CPPFLAGS)
CXXFLAGS ?= -O2
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
override CXXFLAGS += -std=c++20 $(WARNINGS) -pthread
override LDLIBS += $(WATCH_LIBS) -pthread
provider_sources := providers child_process json_util provider_codex provider_antigravity provider_claude
sources := main core desktop tray quota_layout $(provider_sources)
provider_objects := $(addprefix build/,$(addsuffix .o,$(provider_sources)))
asan_provider_objects := $(addprefix build/asan/,$(addsuffix .o,$(provider_sources)))
objects := $(addprefix build/,$(addsuffix .o,$(sources)))

.PHONY: all test test-asan install uninstall clean local-deps claude-statusline claude-statusline-remove
all: build/yrs-watch build/yrs-watch-font-metrics build/yrs-watch-statusline

local-deps: | build
	sh tools/local-deps.sh

build:
	mkdir -p $@

build/%.o: src/%.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

build/yrs-watch: $(objects)
	$(CXX) $(LDFLAGS) $(filter-out %.hpp,$^) $(LDLIBS) -o $@

build/yrs-watch-statusline: build/statusline.o build/core.o
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

build/yrs-watch-font-metrics: src/font_metrics.cpp | build
	$(CXX) $(FONT_CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $< $(FONT_LIBS) -o $@

build/core_test: tests/core_test.cpp tests/check.hpp build/core.o | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(filter-out %.hpp,$^) $(LDLIBS) -o $@

build/provider_test: tests/provider_test.cpp tests/check.hpp build/core.o $(provider_objects) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(filter-out %.hpp,$^) $(LDLIBS) -o $@

build/schedule_test: tests/schedule_test.cpp tests/check.hpp src/schedule.hpp src/core.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDLIBS) -o $@

build/layout_test: tests/layout_test.cpp tests/check.hpp build/quota_layout.o src/quota_layout.hpp src/tray.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/layout_test.cpp build/quota_layout.o $(LDLIBS) -o $@

test: all build/core_test build/schedule_test build/provider_test build/layout_test
	./build/core_test
	./build/schedule_test
	./build/provider_test
	./build/layout_test
	python3 tests/statusline_test.py ./build/yrs-watch-statusline
	python3 tests/integration.py ./build/yrs-watch

ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1
ASAN_CXXFLAGS := -std=c++20 $(WARNINGS) -pthread $(ASAN_FLAGS)

build/asan:
	mkdir -p $@

build/asan/%.o: src/%.cpp | build/asan
	$(CXX) $(CPPFLAGS) $(ASAN_CXXFLAGS) -MMD -MP -c $< -o $@

build/asan/yrs-watch-font-metrics: src/font_metrics.cpp | build/asan
	$(CXX) $(FONT_CPPFLAGS) $(ASAN_CXXFLAGS) $(LDFLAGS) $< $(FONT_LIBS) -o $@

build/asan/core_test: tests/core_test.cpp tests/check.hpp build/asan/core.o | build/asan
	$(CXX) $(CPPFLAGS) $(ASAN_CXXFLAGS) $(LDFLAGS) $(filter-out %.hpp,$^) $(LDLIBS) -o $@

build/asan/schedule_test: tests/schedule_test.cpp tests/check.hpp src/schedule.hpp src/core.hpp | build/asan
	$(CXX) $(CPPFLAGS) $(ASAN_CXXFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

build/asan/provider_test: tests/provider_test.cpp tests/check.hpp build/asan/core.o $(asan_provider_objects) | build/asan
	$(CXX) $(CPPFLAGS) $(ASAN_CXXFLAGS) $(LDFLAGS) $(filter-out %.hpp,$^) $(LDLIBS) -o $@

build/asan/layout_test: tests/layout_test.cpp tests/check.hpp build/asan/quota_layout.o src/quota_layout.hpp src/tray.hpp | build/asan
	$(CXX) $(CPPFLAGS) $(ASAN_CXXFLAGS) $(LDFLAGS) tests/layout_test.cpp build/asan/quota_layout.o $(LDLIBS) -o $@

test-asan: build/asan/yrs-watch-font-metrics build/asan/core_test build/asan/schedule_test build/asan/provider_test build/asan/layout_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./build/asan/core_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./build/asan/schedule_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./build/asan/provider_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./build/asan/layout_test

install: build/yrs-watch build/yrs-watch-font-metrics build/yrs-watch-statusline
	python3 tools/install.py install $<

claude-statusline:
	python3 tools/install.py claude-hook

claude-statusline-remove:
	python3 tools/install.py claude-unhook

uninstall:
	python3 tools/install.py uninstall

clean:
	$(RM) -r build

-include $(objects:.o=.d) build/statusline.d $(wildcard build/asan/*.d)
