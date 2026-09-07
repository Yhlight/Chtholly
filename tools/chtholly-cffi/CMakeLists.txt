add_executable(chtholly-cffi main.cpp)
chtholly_configure_target(chtholly-cffi)
chtholly_configure_executable_artifacts(chtholly-cffi)
target_link_libraries(chtholly-cffi PRIVATE chtholly_cffi_tooling)
chtholly_copy_libclang_runtime(chtholly-cffi "$<TARGET_FILE_DIR:chtholly-cffi>")

install(PROGRAMS "$<TARGET_FILE:chtholly-cffi>"
  DESTINATION bin
  COMPONENT full)
