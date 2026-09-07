add_executable(chtholly-toolchain main.cpp)

chtholly_configure_target(chtholly-toolchain)
chtholly_configure_executable_artifacts(chtholly-toolchain)
target_link_libraries(chtholly-toolchain PRIVATE chtholly_toolchain_tooling)

install(TARGETS chtholly-toolchain RUNTIME DESTINATION bin COMPONENT full)
