add_library(chtholly_support
  Digest.cpp
  FileSystem.cpp)

chtholly_configure_target(chtholly_support)
target_link_libraries(chtholly_support PUBLIC chtholly_basic)
