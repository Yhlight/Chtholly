add_executable(chtholly-lsp main.cpp)

chtholly_configure_target(chtholly-lsp)
chtholly_configure_executable_artifacts(chtholly-lsp)
if(MSVC)
  # LLVM 18 JSON instantiates narrowing optional conversions in MSVC headers.
  target_compile_options(chtholly-lsp PRIVATE /wd4244)
endif()
target_link_libraries(chtholly-lsp PRIVATE chtholly_driver_compiler)
chtholly_link_llvm(chtholly-lsp support)
add_dependencies(chtholly-lsp chtholly_resources chtholly_next_runtime_v1)

install(TARGETS chtholly-lsp RUNTIME DESTINATION bin COMPONENT full)
