#pragma once

// The semantic fixtures that exercise native lowering should use the target
// selected by the host build. Tests which intentionally validate cross-target
// rejection keep their explicit target strings instead.
#ifndef CHTHOLLY_TEST_TARGET_TRIPLE
#define CHTHOLLY_TEST_TARGET_TRIPLE "x86_64-pc-windows-msvc"
#endif

namespace chtholly_test {

inline constexpr const char *targetTriple = CHTHOLLY_TEST_TARGET_TRIPLE;

} // namespace chtholly_test
