add_executable(chthollyc main.cpp)

chtholly_configure_target(chthollyc)
chtholly_configure_executable_artifacts(chthollyc)
target_link_libraries(chthollyc PRIVATE chtholly_driver_compiler)
add_dependencies(chthollyc chtholly_resources chtholly_next_runtime_v1
  chtholly_next_runtime_v2)

install(PROGRAMS "$<TARGET_FILE:chthollyc>"
  DESTINATION libexec/chtholly
  RENAME "chthollyc-driver${CMAKE_EXECUTABLE_SUFFIX}"
  COMPONENT minimal)
install(PROGRAMS "$<TARGET_FILE:chthollyc>"
  DESTINATION libexec/chtholly
  RENAME "chthollyc-driver${CMAKE_EXECUTABLE_SUFFIX}"
  COMPONENT full)
