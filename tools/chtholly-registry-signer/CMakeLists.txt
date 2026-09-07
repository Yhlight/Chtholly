add_executable(chtholly-registry-signer main.cpp)

chtholly_configure_target(chtholly-registry-signer)
chtholly_configure_executable_artifacts(chtholly-registry-signer)
target_link_libraries(chtholly-registry-signer PRIVATE chtholly_registry_tooling)

install(TARGETS chtholly-registry-signer RUNTIME DESTINATION bin COMPONENT full)
