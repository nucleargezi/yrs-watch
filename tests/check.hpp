#pragma once

#include <cstdio>
#include <cstdlib>

// Always evaluated, unlike assert(), so the tests stay meaningful under -DNDEBUG.
[[noreturn]] inline void check_failed(const char* expression, const char* file, int line) {
    std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, expression);
    std::abort();
}

#define CHECK(expression) ((expression) ? void(0) : check_failed(#expression, __FILE__, __LINE__))
