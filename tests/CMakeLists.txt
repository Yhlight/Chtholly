if(NOT TARGET chthollyc OR NOT TARGET chtholly-test)
  return()
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_executable(chtholly_condition_tests chtholly_condition_tests.cpp)
chtholly_configure_target(chtholly_condition_tests)
chtholly_configure_executable_artifacts(chtholly_condition_tests)
target_link_libraries(chtholly_condition_tests PRIVATE chtholly_pipeline)
set(TARGET_CONDITION_TESTS "$<TARGET_FILE:chtholly_condition_tests>")

add_executable(chtholly_abi2_resource_tests chtholly_abi2_resource_tests.cpp)
chtholly_configure_target(chtholly_abi2_resource_tests)
chtholly_configure_executable_artifacts(chtholly_abi2_resource_tests)
target_link_libraries(chtholly_abi2_resource_tests PRIVATE chtholly_next_resource_lease_v2)
set(TARGET_ABI2_RESOURCE "$<TARGET_FILE:chtholly_abi2_resource_tests>")


add_executable(chtholly_check_fixture chtholly_check_fixture.cpp)
chtholly_configure_target(chtholly_check_fixture)
chtholly_configure_executable_artifacts(chtholly_check_fixture)
set(TARGET_CHECK_FIXTURE "$<TARGET_FILE:chtholly_check_fixture>")

# Keep semantic fixtures target-neutral while preserving explicit cross-target
# identity tests. The macro is private to tests and never enters compiler ABI.
if(WIN32)
  add_compile_definitions(CHTHOLLY_TEST_TARGET_TRIPLE="x86_64-pc-windows-msvc")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_compile_definitions(CHTHOLLY_TEST_TARGET_TRIPLE="x86_64-unknown-linux-gnu")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  add_compile_definitions(CHTHOLLY_TEST_TARGET_TRIPLE="aarch64-apple-darwin")
endif()

add_executable(chtholly_artifact_load_benchmark
  ../tools/artifact_load_benchmark.cpp)
chtholly_configure_target(chtholly_artifact_load_benchmark)
chtholly_configure_executable_artifacts(chtholly_artifact_load_benchmark)
target_link_libraries(chtholly_artifact_load_benchmark PRIVATE chtholly_driver_compiler)

add_executable(chtholly_artifact_dispatch_tests
  chtholly_artifact_dispatch_tests.cpp fuzz/chtholly_artifact_fuzz.cpp)
chtholly_configure_target(chtholly_artifact_dispatch_tests)
target_include_directories(chtholly_artifact_dispatch_tests PRIVATE fuzz)
target_link_libraries(chtholly_artifact_dispatch_tests PRIVATE chtholly_semir)

add_executable(chtholly_source_dispatch_tests
  chtholly_source_dispatch_tests.cpp fuzz/chtholly_source_fuzz.cpp)
chtholly_configure_target(chtholly_source_dispatch_tests)
target_include_directories(chtholly_source_dispatch_tests PRIVATE fuzz)
target_link_libraries(chtholly_source_dispatch_tests PRIVATE
  chtholly_pipeline)

add_executable(chtholly_cfdl_resource_flow_tests chtholly_cfdl_resource_flow_tests.cpp)
chtholly_configure_target(chtholly_cfdl_resource_flow_tests)
target_link_libraries(chtholly_cfdl_resource_flow_tests PRIVATE chtholly_semir)

add_executable(chtholly_host_runtime_tests chtholly_host_runtime_tests.cpp)
chtholly_configure_target(chtholly_host_runtime_tests)
target_compile_definitions(chtholly_host_runtime_tests PRIVATE CHTHOLLY_RUNTIME_TESTING=1)
target_link_libraries(chtholly_host_runtime_tests PRIVATE chtholly_next_runtime_v1)

add_executable(chtholly_component_abi2_design_tests
  chtholly_component_abi2_design_tests.cpp)
chtholly_configure_target(chtholly_component_abi2_design_tests)
target_include_directories(chtholly_component_abi2_design_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/runtime/include")
target_link_libraries(chtholly_component_abi2_design_tests PRIVATE chtholly_semir)

add_executable(chtholly_task_runtime_tests chtholly_task_runtime_tests.cpp)
chtholly_configure_target(chtholly_task_runtime_tests)
target_compile_definitions(chtholly_task_runtime_tests PRIVATE
  CHTHOLLY_RUNTIME_TESTING=1)
target_link_libraries(chtholly_task_runtime_tests PRIVATE chtholly_next_runtime_v1)

add_executable(chtholly_container_abi_tests chtholly_container_abi_tests.cpp)
chtholly_configure_target(chtholly_container_abi_tests)
target_compile_definitions(chtholly_container_abi_tests PRIVATE CHTHOLLY_RUNTIME_TESTING=1)
target_link_libraries(chtholly_container_abi_tests PRIVATE chtholly_next_container_v1)

add_executable(chtholly_container_benchmark chtholly_container_benchmark.cpp)
chtholly_configure_target(chtholly_container_benchmark)
target_link_libraries(chtholly_container_benchmark PRIVATE chtholly_next_container_v1)

add_executable(chtholly_host_runtime_v2_tests chtholly_host_runtime_v2_tests.cpp)
chtholly_configure_target(chtholly_host_runtime_v2_tests)
target_link_libraries(chtholly_host_runtime_v2_tests PRIVATE chtholly_next_runtime_v2)

add_executable(chtholly_stdlib_llvm_fixture_tests chtholly_stdlib_llvm_fixture_tests.cpp)
chtholly_configure_target(chtholly_stdlib_llvm_fixture_tests)
target_link_libraries(chtholly_stdlib_llvm_fixture_tests PRIVATE chtholly_pipeline)
target_compile_definitions(chtholly_stdlib_llvm_fixture_tests PRIVATE
  CHTHOLLY_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

add_executable(chtholly_container_ownership_fixture_tests
  chtholly_container_ownership_fixture_tests.cpp)
chtholly_configure_target(chtholly_container_ownership_fixture_tests)
target_link_libraries(chtholly_container_ownership_fixture_tests PRIVATE chtholly_pipeline)
target_compile_definitions(chtholly_container_ownership_fixture_tests PRIVATE
  CHTHOLLY_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

add_executable(chtholly_callable_contract_tests chtholly_callable_contract_tests.cpp)
chtholly_configure_target(chtholly_callable_contract_tests)
target_link_libraries(chtholly_callable_contract_tests PRIVATE chtholly_pipeline)

add_executable(chtholly_deferred_definition_worklist_tests
  chtholly_deferred_definition_worklist_tests.cpp)
chtholly_configure_target(chtholly_deferred_definition_worklist_tests)
target_link_libraries(chtholly_deferred_definition_worklist_tests PRIVATE chtholly_frontend)

add_executable(chtholly_diagnostic_format_tests
  chtholly_diagnostic_format_tests.cpp)
chtholly_configure_target(chtholly_diagnostic_format_tests)
target_link_libraries(chtholly_diagnostic_format_tests PRIVATE
  chtholly_frontend chtholly_driver_support)

add_executable(chtholly_debug_type_tests chtholly_debug_type_tests.cpp)
chtholly_configure_target(chtholly_debug_type_tests)
target_include_directories(chtholly_debug_type_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/lib/Compiler/Lowering")
target_link_libraries(chtholly_debug_type_tests PRIVATE chtholly_llvm)

add_executable(chtholly_semantic_support_tests
  chtholly_semantic_support_tests.cpp)
chtholly_configure_target(chtholly_semantic_support_tests)
target_include_directories(chtholly_semantic_support_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/lib/Compiler/Semantics"
  "${PROJECT_SOURCE_DIR}/lib/Compiler/IR")
target_link_libraries(chtholly_semantic_support_tests PRIVATE
  chtholly_pipeline)

add_executable(chtholly_open_addressing_tests
  chtholly_open_addressing_tests.cpp)
chtholly_configure_target(chtholly_open_addressing_tests)
target_link_libraries(chtholly_open_addressing_tests PRIVATE chtholly_support)

add_executable(chtholly_contract_artifact_tests chtholly_contract_artifact_tests.cpp)
chtholly_configure_target(chtholly_contract_artifact_tests)
target_link_libraries(chtholly_contract_artifact_tests PRIVATE chtholly_pipeline)

add_executable(chtholly_module_alias_tests chtholly_module_alias_tests.cpp)
chtholly_configure_target(chtholly_module_alias_tests)
target_link_libraries(chtholly_module_alias_tests PRIVATE chtholly_pipeline)

add_executable(chtholly_labeled_loop_tests chtholly_labeled_loop_tests.cpp)
chtholly_configure_target(chtholly_labeled_loop_tests)
target_link_libraries(chtholly_labeled_loop_tests PRIVATE chtholly_pipeline)

add_executable(chtholly_environment_callable_abi_tests
  chtholly_environment_callable_abi_tests.cpp)
chtholly_configure_target(chtholly_environment_callable_abi_tests)
target_link_libraries(chtholly_environment_callable_abi_tests PRIVATE chtholly_pipeline)

add_executable(chtholly_sysv_abi_tests chtholly_sysv_abi_tests.cpp)
chtholly_configure_target(chtholly_sysv_abi_tests)
target_link_libraries(chtholly_sysv_abi_tests PRIVATE chtholly_pipeline)

add_executable(chtholly_component_loader_tests
  chtholly_component_loader_tests.cpp component_abi_c_compile.c)
chtholly_configure_target(chtholly_component_loader_tests)
target_link_libraries(chtholly_component_loader_tests PRIVATE
  chtholly_component_loader_v1 chtholly_semir chtholly_support)

add_library(chtholly_component_abi2_rejection_fixture SHARED
  component_abi2_rejection_fixture.c)
chtholly_configure_target(chtholly_component_abi2_rejection_fixture)
target_include_directories(chtholly_component_abi2_rejection_fixture PRIVATE
  "${PROJECT_SOURCE_DIR}/runtime/include")

add_library(chtholly_component_abi2_pending_fixture SHARED
  component_abi2_pending_fixture.cpp)
chtholly_configure_target(chtholly_component_abi2_pending_fixture)
target_include_directories(chtholly_component_abi2_pending_fixture PRIVATE
  "${PROJECT_SOURCE_DIR}/runtime/include")
target_link_libraries(chtholly_component_abi2_pending_fixture PRIVATE
  chtholly_semir chtholly_next_resource_lease_v2)

add_executable(chtholly_component_abi2_rejection_tests
  chtholly_component_abi2_rejection_tests.cpp)
chtholly_configure_target(chtholly_component_abi2_rejection_tests)
target_link_libraries(chtholly_component_abi2_rejection_tests PRIVATE
  chtholly_component_loader_v1)

add_executable(chtholly_component_loader_v2_tests
  chtholly_component_loader_v2_tests.cpp)
chtholly_configure_target(chtholly_component_loader_v2_tests)
target_link_libraries(chtholly_component_loader_v2_tests PRIVATE
  chtholly_component_loader_v2)

add_executable(chtholly_resource_lease_runtime_tests
  chtholly_resource_lease_runtime_tests.cpp)
chtholly_configure_target(chtholly_resource_lease_runtime_tests)
target_link_libraries(chtholly_resource_lease_runtime_tests PRIVATE
  chtholly_next_resource_lease_v2)

add_executable(chtholly_payload_transport_v2_tests
  chtholly_payload_transport_v2_tests.cpp)
chtholly_configure_target(chtholly_payload_transport_v2_tests)
target_link_libraries(chtholly_payload_transport_v2_tests PRIVATE
  chtholly_next_resource_lease_v2)

add_executable(chtholly_component_payload_cross_package_tests
  chtholly_component_payload_cross_package_tests.cpp)
chtholly_configure_target(chtholly_component_payload_cross_package_tests)
target_link_libraries(chtholly_component_payload_cross_package_tests PRIVATE
  chtholly_semir chtholly_next_resource_lease_v2)

add_executable(chtholly_component_abi2_unload_reload_soak
  chtholly_component_abi2_unload_reload_soak.cpp)
chtholly_configure_target(chtholly_component_abi2_unload_reload_soak)
target_link_libraries(chtholly_component_abi2_unload_reload_soak PRIVATE
  chtholly_component_loader_v2)

add_executable(chtholly_artifact_budget_tests chtholly_artifact_budget_tests.cpp)
chtholly_configure_target(chtholly_artifact_budget_tests)
target_include_directories(chtholly_artifact_budget_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/lib/Compiler/IR")
target_link_libraries(chtholly_artifact_budget_tests PRIVATE chtholly_semir)

add_executable(chtholly_outcome_tests chtholly_outcome_tests.cpp)
chtholly_configure_target(chtholly_outcome_tests)
target_link_libraries(chtholly_outcome_tests PRIVATE chtholly_semir)

add_library(chtholly_cfdl_epoch9_provider STATIC cfdl_epoch9_provider.c)
chtholly_configure_target(chtholly_cfdl_epoch9_provider)
target_include_directories(chtholly_cfdl_epoch9_provider PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}")

add_library(chtholly_cffi_real_library_provider STATIC
  cfdl_real_library_provider.c)
chtholly_configure_target(chtholly_cffi_real_library_provider)
target_include_directories(chtholly_cffi_real_library_provider PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(chtholly_package_artifact_interop_tests package_artifact_interop_tests.cpp)
chtholly_configure_target(chtholly_package_artifact_interop_tests)
target_link_libraries(chtholly_package_artifact_interop_tests PRIVATE
  chtholly_package_tooling chtholly_driver_compiler chtholly_pipeline)
target_compile_definitions(chtholly_package_artifact_interop_tests PRIVATE
  CHTHOLLY_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  CHTHOLLY_BINARY_DIR="${PROJECT_BINARY_DIR}"
  CHTHOLLY_CFDL_EPOCH9_PROVIDER="$<TARGET_FILE:chtholly_cfdl_epoch9_provider>")
add_dependencies(chtholly_package_artifact_interop_tests
  chtholly_resources chtholly_next_runtime_v1 chtholly_cfdl_epoch9_provider)

add_custom_target(chtholly_cffi_tool_tests_target
  DEPENDS chtholly-cffi chtholly_cfdl_epoch9_provider
          chtholly_cffi_real_library_provider)

add_executable(chtholly_artifact_seed_generator
  fuzz/chtholly_artifact_seed_generator.cpp)
chtholly_configure_target(chtholly_artifact_seed_generator)
target_link_libraries(chtholly_artifact_seed_generator PRIVATE chtholly_driver_compiler)

add_executable(chtholly_toolchain_space_tests
  chtholly_toolchain_space_tests.cpp)
chtholly_configure_target(chtholly_toolchain_space_tests)
target_include_directories(chtholly_toolchain_space_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/lib/Driver/Toolchain")
target_link_libraries(chtholly_toolchain_space_tests PRIVATE
  chtholly_toolchain_tooling)

if(CHTHOLLY_BUILD_FUZZERS)
  add_executable(chtholly_artifact_fuzz fuzz/chtholly_artifact_fuzz.cpp)
  add_dependencies(chtholly_artifact_fuzz chtholly_artifact_seed_generator)
  chtholly_configure_target(chtholly_artifact_fuzz)
  target_compile_options(chtholly_artifact_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(chtholly_artifact_fuzz PRIVATE -fsanitize=fuzzer)
  target_include_directories(chtholly_artifact_fuzz PRIVATE fuzz)
  target_link_libraries(chtholly_artifact_fuzz PRIVATE chtholly_semir)

  add_executable(chtholly_source_fuzz fuzz/chtholly_source_fuzz.cpp)
  chtholly_configure_target(chtholly_source_fuzz)
  target_compile_options(chtholly_source_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(chtholly_source_fuzz PRIVATE -fsanitize=fuzzer)
  target_include_directories(chtholly_source_fuzz PRIVATE fuzz)
  target_link_libraries(chtholly_source_fuzz PRIVATE chtholly_pipeline)

  add_executable(chtholly_container_fuzz fuzz/chtholly_container_fuzz.cpp)
  chtholly_configure_target(chtholly_container_fuzz)
  target_compile_options(chtholly_container_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_options(chtholly_container_fuzz PRIVATE -fsanitize=fuzzer)
  target_link_libraries(chtholly_container_fuzz PRIVATE chtholly_next_container_v1)
endif()

if(TARGET chtholly-lsp)
  set(CHTHOLLY_TEST_LSP "$<TARGET_FILE:chtholly-lsp>")
else()
  set(CHTHOLLY_TEST_LSP "")
endif()

set(CHTHOLLY_TEST_SOURCE_DIR "${PROJECT_SOURCE_DIR}")
set(CHTHOLLY_TEST_BUILD_DIR "${PROJECT_BINARY_DIR}")
file(TO_CMAKE_PATH "${Python3_EXECUTABLE}" CHTHOLLY_TEST_PYTHON)
file(TO_CMAKE_PATH "${CMAKE_COMMAND}" CHTHOLLY_TEST_CMAKE)
file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}" CHTHOLLY_TEST_SOURCE_DIR)
file(TO_CMAKE_PATH "${PROJECT_BINARY_DIR}" CHTHOLLY_TEST_BUILD_DIR)
set(CHTHOLLY_TEST_TARGET_CHTHOLLYC "$<TARGET_FILE:chthollyc>")
set(CHTHOLLY_TEST_TARGET_ARTIFACT_BENCHMARK
    "$<TARGET_FILE:chtholly_artifact_load_benchmark>")
set(CHTHOLLY_TEST_TARGET_ARTIFACT_DISPATCH
    "$<TARGET_FILE:chtholly_artifact_dispatch_tests>")
set(CHTHOLLY_TEST_TARGET_SOURCE_DISPATCH
    "$<TARGET_FILE:chtholly_source_dispatch_tests>")
set(CHTHOLLY_TEST_TARGET_CFDL "$<TARGET_FILE:chtholly_cfdl_resource_flow_tests>")
set(CHTHOLLY_TEST_TARGET_HOST_RUNTIME "$<TARGET_FILE:chtholly_host_runtime_tests>")
set(CHTHOLLY_TEST_TARGET_COMPONENT_ABI2_DESIGN
    "$<TARGET_FILE:chtholly_component_abi2_design_tests>")
set(CHTHOLLY_TEST_TARGET_TASK_RUNTIME
    "$<TARGET_FILE:chtholly_task_runtime_tests>")
set(CHTHOLLY_TEST_TARGET_HOST_RUNTIME_V2
    "$<TARGET_FILE:chtholly_host_runtime_v2_tests>")
set(CHTHOLLY_TEST_TARGET_STDLIB_LLVM "$<TARGET_FILE:chtholly_stdlib_llvm_fixture_tests>")
set(CHTHOLLY_TEST_TARGET_CONTAINER_OWNERSHIP
    "$<TARGET_FILE:chtholly_container_ownership_fixture_tests>")
set(CHTHOLLY_TEST_TARGET_CALLABLE_CONTRACT
    "$<TARGET_FILE:chtholly_callable_contract_tests>")
set(CHTHOLLY_TEST_TARGET_WORKLIST
    "$<TARGET_FILE:chtholly_deferred_definition_worklist_tests>")
set(CHTHOLLY_TEST_TARGET_DIAGNOSTIC_FORMAT
    "$<TARGET_FILE:chtholly_diagnostic_format_tests>")
set(CHTHOLLY_TEST_TARGET_DEBUG_TYPES
    "$<TARGET_FILE:chtholly_debug_type_tests>")
set(CHTHOLLY_TEST_TARGET_SEMANTIC_SUPPORT
    "$<TARGET_FILE:chtholly_semantic_support_tests>")
set(CHTHOLLY_TEST_TARGET_OPEN_ADDRESSING
    "$<TARGET_FILE:chtholly_open_addressing_tests>")
set(CHTHOLLY_TEST_TARGET_CONTAINER_ABI
    "$<TARGET_FILE:chtholly_container_abi_tests>")
set(CHTHOLLY_TEST_TARGET_CONTAINER_BENCHMARK
    "$<TARGET_FILE:chtholly_container_benchmark>")
set(CHTHOLLY_TEST_TARGET_CONTRACT_ARTIFACT
    "$<TARGET_FILE:chtholly_contract_artifact_tests>")
set(CHTHOLLY_TEST_TARGET_MODULE_ALIAS "$<TARGET_FILE:chtholly_module_alias_tests>")
set(CHTHOLLY_TEST_TARGET_LABELED_LOOP "$<TARGET_FILE:chtholly_labeled_loop_tests>")
set(CHTHOLLY_TEST_TARGET_ENVIRONMENT_CALLABLE
    "$<TARGET_FILE:chtholly_environment_callable_abi_tests>")
set(CHTHOLLY_TEST_TARGET_SYSV_ABI
    "$<TARGET_FILE:chtholly_sysv_abi_tests>")
set(CHTHOLLY_TEST_TARGET_COMPONENT_LOADER
    "$<TARGET_FILE:chtholly_component_loader_tests>")
set(CHTHOLLY_TEST_TARGET_COMPONENT_ABI2_REJECTION
    "$<TARGET_FILE:chtholly_component_abi2_rejection_tests>")
set(CHTHOLLY_TEST_TARGET_COMPONENT_ABI2_REJECTION_FIXTURE
    "$<TARGET_FILE:chtholly_component_abi2_rejection_fixture>")
set(CHTHOLLY_TEST_TARGET_COMPONENT_LOADER_V2
    "$<TARGET_FILE:chtholly_component_loader_v2_tests>")
set(CHTHOLLY_TEST_TARGET_COMPONENT_ABI2_PENDING_FIXTURE
    "$<TARGET_FILE:chtholly_component_abi2_pending_fixture>")
set(CHTHOLLY_TEST_TARGET_RESOURCE_LEASE_RUNTIME
    "$<TARGET_FILE:chtholly_resource_lease_runtime_tests>")
set(CHTHOLLY_TEST_TARGET_PAYLOAD_TRANSPORT_V2
    "$<TARGET_FILE:chtholly_payload_transport_v2_tests>")
set(CHTHOLLY_TEST_TARGET_COMPONENT_PAYLOAD_CROSS_PACKAGE
    "$<TARGET_FILE:chtholly_component_payload_cross_package_tests>")
set(CHTHOLLY_TEST_TARGET_ABI2_UNLOAD_RELOAD_SOAK
    "$<TARGET_FILE:chtholly_component_abi2_unload_reload_soak>")
set(CHTHOLLY_TEST_TARGET_COMPONENT_HOST
    "$<TARGET_FILE:chtholly_component_host_example>")
set(CHTHOLLY_TEST_TARGET_TELEMETRY_COMPONENT_HOST
    "$<TARGET_FILE:chtholly_telemetry_component_host>")
set(CHTHOLLY_TEST_TARGET_TELEMETRY_INGEST_HOST
    "$<TARGET_FILE:chtholly_telemetry_ingest_host>")
set(CHTHOLLY_TEST_TARGET_COMPONENT_DEPLOY
    "$<TARGET_FILE:chtholly-component-deploy>")
set(CHTHOLLY_TEST_TARGET_ARTIFACT_BUDGET
    "$<TARGET_FILE:chtholly_artifact_budget_tests>")
set(CHTHOLLY_TEST_TARGET_OUTCOME "$<TARGET_FILE:chtholly_outcome_tests>")
set(CHTHOLLY_TEST_TARGET_PACKAGE_INTEROP
    "$<TARGET_FILE:chtholly_package_artifact_interop_tests>")
set(CHTHOLLY_TEST_TARGET_ARTIFACT_SEED
    "$<TARGET_FILE:chtholly_artifact_seed_generator>")
set(CHTHOLLY_TEST_TARGET_LSP "${CHTHOLLY_TEST_LSP}")
set(CHTHOLLY_TEST_TARGET_CFFI "$<TARGET_FILE:chtholly-cffi>")
set(CHTHOLLY_TEST_C_COMPILER "${CMAKE_C_COMPILER}")
set(CHTHOLLY_TEST_CFFI_PROVIDER "$<TARGET_FILE:chtholly_cfdl_epoch9_provider>")
set(CHTHOLLY_TEST_CFFI_REAL_PROVIDER
    "$<TARGET_FILE:chtholly_cffi_real_library_provider>")
set(CHTHOLLY_TEST_SQLITE_INCLUDE
    "$<TARGET_PROPERTY:unofficial::sqlite3::sqlite3,INTERFACE_INCLUDE_DIRECTORIES>")
set(CHTHOLLY_TEST_SQLITE_LIBRARY "$<TARGET_FILE:unofficial::sqlite3::sqlite3>")
set(CHTHOLLY_TEST_ZLIB_LIBRARY "$<TARGET_FILE:ZLIB::ZLIB>")
set(CHTHOLLY_TEST_CURL_LIBRARY "$<TARGET_FILE:CURL::libcurl>")
set(CHTHOLLY_TEST_SQLITE_PROVIDER
    "$<TARGET_FILE:chtholly_sqlite_safety_provider>")

# Names used by the source manifest template. Generator expressions remain
# unresolved until file(GENERATE) evaluates the final build configuration.
set(PYTHON "${CHTHOLLY_TEST_PYTHON}")
set(CMAKE "${CHTHOLLY_TEST_CMAKE}")
set(SOURCE_DIR "${CHTHOLLY_TEST_SOURCE_DIR}")
set(BUILD_DIR "${CHTHOLLY_TEST_BUILD_DIR}")
set(TARGET_CHTHOLLYC "${CHTHOLLY_TEST_TARGET_CHTHOLLYC}")
set(TARGET_TEST_RUNNER "$<TARGET_FILE:chtholly-test>")
set(TARGET_TOOLCHAIN "$<TARGET_FILE:chtholly-toolchain>")
set(TARGET_TOOLCHAIN_SPACE "$<TARGET_FILE:chtholly_toolchain_space_tests>")
set(TARGET_ARTIFACT_BENCHMARK "${CHTHOLLY_TEST_TARGET_ARTIFACT_BENCHMARK}")
set(TARGET_ARTIFACT_DISPATCH "${CHTHOLLY_TEST_TARGET_ARTIFACT_DISPATCH}")
set(TARGET_SOURCE_DISPATCH "${CHTHOLLY_TEST_TARGET_SOURCE_DISPATCH}")
set(TARGET_CFDL "${CHTHOLLY_TEST_TARGET_CFDL}")
set(TARGET_HOST_RUNTIME "${CHTHOLLY_TEST_TARGET_HOST_RUNTIME}")
set(TARGET_COMPONENT_ABI2_DESIGN
    "${CHTHOLLY_TEST_TARGET_COMPONENT_ABI2_DESIGN}")
set(TARGET_TASK_RUNTIME "${CHTHOLLY_TEST_TARGET_TASK_RUNTIME}")
set(TARGET_HOST_RUNTIME_V2 "${CHTHOLLY_TEST_TARGET_HOST_RUNTIME_V2}")
set(TARGET_STDLIB_LLVM "${CHTHOLLY_TEST_TARGET_STDLIB_LLVM}")
set(TARGET_CONTAINER_OWNERSHIP "${CHTHOLLY_TEST_TARGET_CONTAINER_OWNERSHIP}")
set(TARGET_CALLABLE_CONTRACT "${CHTHOLLY_TEST_TARGET_CALLABLE_CONTRACT}")
set(TARGET_WORKLIST "${CHTHOLLY_TEST_TARGET_WORKLIST}")
set(TARGET_DIAGNOSTIC_FORMAT "${CHTHOLLY_TEST_TARGET_DIAGNOSTIC_FORMAT}")
set(TARGET_DEBUG_TYPES "${CHTHOLLY_TEST_TARGET_DEBUG_TYPES}")
set(TARGET_SEMANTIC_SUPPORT "${CHTHOLLY_TEST_TARGET_SEMANTIC_SUPPORT}")
set(TARGET_OPEN_ADDRESSING "${CHTHOLLY_TEST_TARGET_OPEN_ADDRESSING}")
set(TARGET_CONTAINER_ABI "${CHTHOLLY_TEST_TARGET_CONTAINER_ABI}")
set(TARGET_CONTAINER_BENCHMARK "${CHTHOLLY_TEST_TARGET_CONTAINER_BENCHMARK}")
set(TARGET_CONTRACT_ARTIFACT "${CHTHOLLY_TEST_TARGET_CONTRACT_ARTIFACT}")
set(TARGET_MODULE_ALIAS "${CHTHOLLY_TEST_TARGET_MODULE_ALIAS}")
set(TARGET_LABELED_LOOP "${CHTHOLLY_TEST_TARGET_LABELED_LOOP}")
set(TARGET_ENVIRONMENT_CALLABLE "${CHTHOLLY_TEST_TARGET_ENVIRONMENT_CALLABLE}")
set(TARGET_SYSV_ABI "${CHTHOLLY_TEST_TARGET_SYSV_ABI}")
set(TARGET_COMPONENT_LOADER "${CHTHOLLY_TEST_TARGET_COMPONENT_LOADER}")
set(TARGET_COMPONENT_ABI2_REJECTION
    "${CHTHOLLY_TEST_TARGET_COMPONENT_ABI2_REJECTION}")
set(TARGET_COMPONENT_ABI2_REJECTION_FIXTURE
    "${CHTHOLLY_TEST_TARGET_COMPONENT_ABI2_REJECTION_FIXTURE}")
set(TARGET_COMPONENT_LOADER_V2
    "${CHTHOLLY_TEST_TARGET_COMPONENT_LOADER_V2}")
set(TARGET_COMPONENT_ABI2_PENDING_FIXTURE
    "${CHTHOLLY_TEST_TARGET_COMPONENT_ABI2_PENDING_FIXTURE}")
set(TARGET_RESOURCE_LEASE_RUNTIME
    "${CHTHOLLY_TEST_TARGET_RESOURCE_LEASE_RUNTIME}")
set(TARGET_PAYLOAD_TRANSPORT_V2
    "${CHTHOLLY_TEST_TARGET_PAYLOAD_TRANSPORT_V2}")
set(TARGET_COMPONENT_PAYLOAD_CROSS_PACKAGE
    "${CHTHOLLY_TEST_TARGET_COMPONENT_PAYLOAD_CROSS_PACKAGE}")
set(TARGET_ABI2_UNLOAD_RELOAD_SOAK
    "${CHTHOLLY_TEST_TARGET_ABI2_UNLOAD_RELOAD_SOAK}")
set(TARGET_ABI2_EVIDENCE_SCRIPT
    "${PROJECT_SOURCE_DIR}/scripts/abi2-evidence.py")
set(TARGET_COMPONENT_HOST "${CHTHOLLY_TEST_TARGET_COMPONENT_HOST}")
set(TARGET_TELEMETRY_COMPONENT_HOST
    "${CHTHOLLY_TEST_TARGET_TELEMETRY_COMPONENT_HOST}")
set(TARGET_TELEMETRY_INGEST_HOST
    "${CHTHOLLY_TEST_TARGET_TELEMETRY_INGEST_HOST}")
set(TARGET_COMPONENT_DEPLOY "${CHTHOLLY_TEST_TARGET_COMPONENT_DEPLOY}")
if(WIN32)
  set(TARGET_COMPONENT_TRIPLE "x86_64-pc-windows-msvc")
else()
  set(TARGET_COMPONENT_TRIPLE "x86_64-unknown-linux-gnu")
endif()
set(TARGET_ARTIFACT_BUDGET "${CHTHOLLY_TEST_TARGET_ARTIFACT_BUDGET}")
set(TARGET_OUTCOME "${CHTHOLLY_TEST_TARGET_OUTCOME}")
set(TARGET_PACKAGE_INTEROP "${CHTHOLLY_TEST_TARGET_PACKAGE_INTEROP}")
set(TARGET_ARTIFACT_SEED "${CHTHOLLY_TEST_TARGET_ARTIFACT_SEED}")
set(TARGET_LSP "${CHTHOLLY_TEST_TARGET_LSP}")
set(TARGET_CFFI "${CHTHOLLY_TEST_TARGET_CFFI}")
set(C_COMPILER "${CHTHOLLY_TEST_C_COMPILER}")
set(CFFI_PROVIDER "${CHTHOLLY_TEST_CFFI_PROVIDER}")
set(CFFI_REAL_PROVIDER "${CHTHOLLY_TEST_CFFI_REAL_PROVIDER}")
set(SQLITE_INCLUDE "${CHTHOLLY_TEST_SQLITE_INCLUDE}")
set(SQLITE_LIBRARY "${CHTHOLLY_TEST_SQLITE_LIBRARY}")
set(ZLIB_LIBRARY "${CHTHOLLY_TEST_ZLIB_LIBRARY}")
set(CURL_LIBRARY "${CHTHOLLY_TEST_CURL_LIBRARY}")
set(SQLITE_PROVIDER "${CHTHOLLY_TEST_SQLITE_PROVIDER}")

if(CHTHOLLY_BUILD_FUZZERS)
  set(CHTHOLLY_TEST_FUZZ_BLOCK
"[[test]]
name = \"chtholly_artifact_fuzz_smoke\"
kind = \"process\"
labels = [\"compiler\", \"architecture\", \"fuzz\", \"sanitizer\"]
command = [\"$<TARGET_FILE:chtholly_artifact_fuzz>\", \"-runs=1000\", \"-max_len=67108865\", \"-rss_limit_mb=2048\", \"${CHTHOLLY_TEST_SOURCE_DIR}/tests/fuzz/corpus/chtholly_artifact\"]
working_directory = \"${CHTHOLLY_TEST_BUILD_DIR}/tests\"
timeout_seconds = 60
serial = true

[[test]]
name = \"chtholly_source_fuzz_smoke\"
kind = \"process\"
labels = [\"compiler\", \"frontend\", \"semantics\", \"fuzz\", \"sanitizer\"]
command = [\"$<TARGET_FILE:chtholly_source_fuzz>\", \"-runs=1000\", \"-max_len=100000\", \"-rss_limit_mb=2048\", \"${CHTHOLLY_TEST_SOURCE_DIR}/tests/fuzz/corpus/chtholly_source\"]
working_directory = \"${CHTHOLLY_TEST_BUILD_DIR}/tests\"
timeout_seconds = 60
serial = true

[[test]]
name = \"chtholly_container_fuzz_smoke\"
kind = \"process\"
labels = [\"runtime\", \"collections\", \"fuzz\", \"sanitizer\"]
command = [\"$<TARGET_FILE:chtholly_container_fuzz>\", \"-runs=1000\", \"-max_len=4096\", \"-rss_limit_mb=2048\", \"${CHTHOLLY_TEST_SOURCE_DIR}/tests/fuzz/corpus/chtholly_artifact\"]
working_directory = \"${CHTHOLLY_TEST_BUILD_DIR}/tests\"
timeout_seconds = 60
serial = true
")
else()
  set(CHTHOLLY_TEST_FUZZ_BLOCK "")
endif()
set(FUZZ_TEST "${CHTHOLLY_TEST_FUZZ_BLOCK}")

if(CHTHOLLY_BUILD_VSCODE_EXTENSION)
  set(CHTHOLLY_TEST_INSTALL_VSIX_ARG
      ", \"--expect-vsix\"")
else()
  set(CHTHOLLY_TEST_INSTALL_VSIX_ARG "")
endif()
set(INSTALL_VSIX_ARG "${CHTHOLLY_TEST_INSTALL_VSIX_ARG}")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/tests/chtholly-tests.toml.in"
  "${PROJECT_SOURCE_DIR}/tests/chtholly_install_tree_tests.py"
  "${PROJECT_SOURCE_DIR}/tests/artifact_cache_report_tests.py"
  "${PROJECT_SOURCE_DIR}/tests/process_memory_tests.py"
  "${PROJECT_SOURCE_DIR}/tests/release_contract_audit_tests.py"
  "${PROJECT_SOURCE_DIR}/scripts/report-artifact-cache.py"
  "${PROJECT_SOURCE_DIR}/scripts/process-memory.py"
  "${PROJECT_SOURCE_DIR}/scripts/build-performance-baseline.py"
  "${PROJECT_SOURCE_DIR}/scripts/release-contract-audit.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/tests/chtholly_sqlite_safety_tests.py"
  "${PROJECT_SOURCE_DIR}/examples/sqlite-safety/chtholly.toml"
  "${PROJECT_SOURCE_DIR}/examples/sqlite-safety/src/main.cns"
  "${PROJECT_SOURCE_DIR}/examples/sqlite-safety/src/sqlite.cfdl"
  "${PROJECT_SOURCE_DIR}/examples/sqlite-safety/native/provider.c"
  "${PROJECT_SOURCE_DIR}/examples/sqlite-safety/native/provider.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/scripts/current-status-audit.py"
  "${PROJECT_SOURCE_DIR}/docs/current-status.md"
  "${PROJECT_SOURCE_DIR}/support/chtholly-product-status.toml"
  "${PROJECT_SOURCE_DIR}/include/chtholly/Basic/LanguageVersion.h")
file(READ "${PROJECT_SOURCE_DIR}/tests/chtholly-tests.toml.in"
     CHTHOLLY_TEST_MANIFEST_TEMPLATE)
string(CONFIGURE "${CHTHOLLY_TEST_MANIFEST_TEMPLATE}"
               CHTHOLLY_TEST_MANIFEST_CONFIGURED @ONLY)
file(GENERATE
  OUTPUT "${PROJECT_BINARY_DIR}/tests/chtholly-tests.generated.toml"
  CONTENT "${CHTHOLLY_TEST_MANIFEST_CONFIGURED}")

set(CHTHOLLY_TEST_TARGETS
  chtholly_condition_tests
  chtholly_abi2_resource_tests
  chtholly_check_fixture
  chtholly-test chthollyc chthollyc-launcher chtholly-toolchain
  chtholly-cffi chtholly_cfdl_epoch9_provider
  chtholly-component-deploy
  chtholly_cffi_real_library_provider
  chtholly_sqlite_safety_provider
  chtholly-registryd chtholly-registry-signer chtholly-registry-witnessd
  chtholly_artifact_load_benchmark
  chtholly_artifact_dispatch_tests
  chtholly_source_dispatch_tests
  chtholly_cfdl_resource_flow_tests
  chtholly_host_runtime_tests
  chtholly_component_abi2_design_tests
  chtholly_task_runtime_tests
  chtholly_host_runtime_v2_tests
  chtholly_stdlib_llvm_fixture_tests
  chtholly_container_abi_tests
  chtholly_container_benchmark
  chtholly_container_ownership_fixture_tests
  chtholly_callable_contract_tests
  chtholly_deferred_definition_worklist_tests
  chtholly_diagnostic_format_tests
  chtholly_debug_type_tests
  chtholly_semantic_support_tests
  chtholly_open_addressing_tests
  chtholly_contract_artifact_tests
  chtholly_module_alias_tests
  chtholly_labeled_loop_tests
  chtholly_environment_callable_abi_tests
  chtholly_sysv_abi_tests
  chtholly_component_loader_tests
  chtholly_component_loader_v2
  chtholly_component_abi2_rejection_fixture
  chtholly_component_abi2_rejection_tests
  chtholly_component_loader_v2_tests
  chtholly_component_abi2_pending_fixture
  chtholly_resource_lease_runtime_tests
  chtholly_payload_transport_v2_tests
  chtholly_component_payload_cross_package_tests
  chtholly_component_abi2_unload_reload_soak
  chtholly_component_host_example
  chtholly_telemetry_component_host
  chtholly_telemetry_ingest_host
  chtholly_artifact_budget_tests
  chtholly_outcome_tests
  chtholly_package_artifact_interop_tests
  chtholly_artifact_seed_generator
  chtholly_toolchain_space_tests)
if(TARGET chtholly-lsp)
  list(APPEND CHTHOLLY_TEST_TARGETS chtholly-lsp)
endif()
if(CHTHOLLY_BUILD_FUZZERS)
  list(APPEND CHTHOLLY_TEST_TARGETS
    chtholly_artifact_fuzz chtholly_source_fuzz chtholly_container_fuzz)
endif()
add_custom_target(chtholly-test-suite DEPENDS ${CHTHOLLY_TEST_TARGETS})
