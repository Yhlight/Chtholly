add_library(chtholly_compiler_core
  Interner.cpp
  Metrics.cpp)

chtholly_configure_target(chtholly_compiler_core)
target_include_directories(chtholly_compiler_core SYSTEM PUBLIC
  ${LLVM_INCLUDE_DIRS})
chtholly_link_llvm(chtholly_compiler_core support)
