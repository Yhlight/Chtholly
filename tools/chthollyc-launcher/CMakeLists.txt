add_executable(chthollyc-launcher main.cpp)
set_target_properties(chthollyc-launcher PROPERTIES OUTPUT_NAME chthollyc)
chtholly_configure_target(chthollyc-launcher)
chtholly_configure_executable_artifacts(chthollyc-launcher)
target_link_libraries(chthollyc-launcher PRIVATE chtholly_driver_support)

install(TARGETS chthollyc-launcher RUNTIME DESTINATION bin COMPONENT minimal)
install(TARGETS chthollyc-launcher RUNTIME DESTINATION bin COMPONENT full)
