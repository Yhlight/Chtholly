add_executable(chtholly-registry-witnessd main.cpp)
chtholly_configure_target(chtholly-registry-witnessd)
chtholly_configure_executable_artifacts(chtholly-registry-witnessd)
target_link_libraries(chtholly-registry-witnessd
  PRIVATE chtholly_registry_server httplib::httplib)

install(TARGETS chtholly-registry-witnessd
  RUNTIME DESTINATION bin
  COMPONENT full)
