#pragma once

#include <cstdio>
#include <cstdlib>

namespace chtholly_test {
inline void check(bool condition, const char *expression, const char *file,
                  int line) {
  if (!condition) {
    std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, expression);
    std::fflush(stderr);
    std::exit(97);
  }
}
} // namespace chtholly_test

// Unlike assert, both the operation and its check execute under NDEBUG.
#define CHTHOLLY_TEST_CHECK(...) \
  ::chtholly_test::check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, \
                         __FILE__, __LINE__)
