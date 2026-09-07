add_executable(chtholly-test main.cpp)
chtholly_configure_target(chtholly-test)
target_link_libraries(chtholly-test PRIVATE chtholly_testing)
install(TARGETS chtholly-test RUNTIME DESTINATION bin COMPONENT full)
