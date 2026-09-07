add_executable(chtholly-registryd main.cpp)

chtholly_configure_target(chtholly-registryd)
chtholly_configure_executable_artifacts(chtholly-registryd)
target_link_libraries(chtholly-registryd
  PRIVATE chtholly_registry_server httplib::httplib)

install(TARGETS chtholly-registryd RUNTIME DESTINATION bin COMPONENT full)
