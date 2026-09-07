add_library(chtholly_next_runtime_v1 STATIC
  src/Host/next_host_v1.c
  src/Host/next_reference_adapters_v1.c
  src/Host/next_sync_v1.c
  src/Host/next_sync_guard_v1.c
  src/Host/next_channel_v1.c
  src/Host/next_typed_channel_v1.c
  src/Host/next_hosted_async_v1.c
  src/Core/next_runtime_v1.c
  src/Core/next_path.c
  src/Task/next_task_v1.c)

add_library(chtholly_next_container_v1 STATIC
  src/Core/next_container_v1.c)
target_include_directories(chtholly_next_container_v1
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>")
set_target_properties(chtholly_next_container_v1 PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
  OUTPUT_NAME chtholly_next_container_v1
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/share/chtholly/runtime")
chtholly_configure_target(chtholly_next_container_v1)
chtholly_configure_warnings(chtholly_next_container_v1)
if(CHTHOLLY_BUILD_TESTS)
  target_compile_definitions(chtholly_next_container_v1 PRIVATE
    CHTHOLLY_RUNTIME_TESTING=1)
endif()
if(MSVC)
  set_property(TARGET chtholly_next_container_v1 PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  set_property(TARGET chtholly_next_container_v1 PROPERTY
    POSITION_INDEPENDENT_CODE ON)
endif()

add_library(chtholly_next_runtime_v2 STATIC
  src/Host/next_host_v2.c
  src/Core/next_runtime_v2.c)
target_include_directories(chtholly_next_runtime_v2
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>")
set_target_properties(chtholly_next_runtime_v2 PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
  OUTPUT_NAME chtholly_next_runtime_v2
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/share/chtholly/runtime")
if(MSVC)
  target_link_libraries(chtholly_next_runtime_v2 PUBLIC ws2_32)
  set_property(TARGET chtholly_next_runtime_v2 PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  set_property(TARGET chtholly_next_runtime_v2 PROPERTY
    POSITION_INDEPENDENT_CODE ON)
  find_package(Threads REQUIRED)
  target_link_libraries(chtholly_next_runtime_v2 PUBLIC Threads::Threads)
endif()
chtholly_configure_target(chtholly_next_runtime_v2)
chtholly_configure_warnings(chtholly_next_runtime_v2)

# ABI-2 resource leases are deliberately isolated from the published v1
# runtime link manifest until the component protocol is fully specified.
add_library(chtholly_next_resource_lease_v2 STATIC
  src/Host/next_resource_lease_v2.c)
target_include_directories(chtholly_next_resource_lease_v2 PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:include>")
set_target_properties(chtholly_next_resource_lease_v2 PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON
  OUTPUT_NAME chtholly_next_resource_lease_v2
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/share/chtholly/runtime")
chtholly_configure_target(chtholly_next_resource_lease_v2)
chtholly_configure_warnings(chtholly_next_resource_lease_v2)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(chtholly_next_resource_lease_v2 PUBLIC Threads::Threads)
  target_compile_definitions(chtholly_next_resource_lease_v2 PRIVATE _POSIX_C_SOURCE=200809L)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(chtholly_next_runtime_v2 PRIVATE
    _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE=1)
elseif(APPLE)
  target_compile_definitions(chtholly_next_runtime_v2 PRIVATE _DARWIN_C_SOURCE=1)
endif()

add_library(chtholly_component_loader_v1 STATIC
  src/component_loader_v1.cpp
  ../lib/Support/Digest.cpp
  ../lib/Support/FileSystem.cpp)
target_include_directories(chtholly_component_loader_v1 PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:include>")
if(NOT WIN32)
  target_link_libraries(chtholly_component_loader_v1 PRIVATE ${CMAKE_DL_LIBS})
endif()
chtholly_configure_target(chtholly_component_loader_v1)

# ABI-2 loader is experimental and deliberately isolated from the v1 loader
# and published v1 runtime link manifest.
add_library(chtholly_component_loader_v2 STATIC
  src/component_loader_v2.cpp
  ../lib/Support/FileSystem.cpp)
target_include_directories(chtholly_component_loader_v2 PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:include>")
target_link_libraries(chtholly_component_loader_v2 PUBLIC
  chtholly_semir chtholly_next_resource_lease_v2)
if(NOT WIN32)
  target_link_libraries(chtholly_component_loader_v2 PRIVATE ${CMAKE_DL_LIBS})
endif()
chtholly_configure_target(chtholly_component_loader_v2)

if(CHTHOLLY_BUILD_TESTS)
  target_compile_definitions(chtholly_next_runtime_v1 PRIVATE
    CHTHOLLY_RUNTIME_TESTING=1)
endif()

target_include_directories(chtholly_next_runtime_v1
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>")

set_target_properties(chtholly_next_runtime_v1 PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
  OUTPUT_NAME chtholly_next_runtime_v1
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/share/chtholly/runtime")
if(MSVC)
  set_property(TARGET chtholly_next_runtime_v1 PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
foreach(config Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${config}" config_upper)
  set_property(TARGET chtholly_next_runtime_v1 PROPERTY
    "ARCHIVE_OUTPUT_DIRECTORY_${config_upper}"
    "${CMAKE_BINARY_DIR}/share/chtholly/runtime")
endforeach()

chtholly_configure_warnings(chtholly_next_runtime_v1)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(chtholly_next_runtime_v1 PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE=1)
elseif(APPLE)
  target_compile_definitions(chtholly_next_runtime_v1 PRIVATE
    _DARWIN_C_SOURCE=1)
endif()

if(MSVC)
  target_link_libraries(chtholly_next_runtime_v1 PUBLIC ws2_32)
  target_compile_options(chtholly_next_runtime_v1 PRIVATE
    /experimental:c11atomics)
else()
  set_property(TARGET chtholly_next_runtime_v1 PROPERTY
    POSITION_INDEPENDENT_CODE ON)
  find_package(Threads REQUIRED)
  target_link_libraries(chtholly_next_runtime_v1 PUBLIC Threads::Threads)
endif()

set(chtholly_runtime_resource_dir
    "${CMAKE_BINARY_DIR}/share/chtholly/runtime")
set(chtholly_runtime_link_manifest "version 1\n")
string(APPEND chtholly_runtime_link_manifest
  "symbol host_open chtholly_next_host_v1_open\n"
  "symbol host_open_read chtholly_next_host_v1_open_read\n"
  "symbol host_create chtholly_next_host_v1_create\n"
  "symbol host_read chtholly_next_host_v1_read_ref\n"
  "symbol host_write chtholly_next_host_v1_write_ref\n"
  "symbol host_close chtholly_next_host_v1_close\n"
  "symbol accept chtholly_next_host_v1_net_accept_ref\n"
  "symbol close chtholly_next_host_v1_net_close\n"
  "symbol listen chtholly_next_host_v1_net_listen\n"
  "symbol read chtholly_next_host_v1_net_read_ref\n"
  "symbol write chtholly_next_host_v1_net_write_ref\n"
  "symbol mutex_close chtholly_next_host_v1_sync_mutex_close\n"
  "symbol mutex_init chtholly_next_host_v1_sync_mutex_init\n"
  "symbol mutex_lock chtholly_next_host_v1_sync_mutex_lock_ref\n"
  "symbol mutex_unlock chtholly_next_host_v1_sync_mutex_unlock_ref\n"
  "symbol condvar_close chtholly_next_host_v1_sync_condvar_close\n"
  "symbol condvar_init chtholly_next_host_v1_sync_condvar_init\n"
  "symbol condvar_notify_all chtholly_next_host_v1_sync_condvar_notify_all_ref\n"
  "symbol condvar_notify_one chtholly_next_host_v1_sync_condvar_notify_one_ref\n"
  "symbol condvar_wait chtholly_next_host_v1_sync_condvar_wait_ref\n"
  "symbol sync_guard_acquire chtholly_next_host_v1_sync_guard_acquire\n"
  "symbol sync_guard_release chtholly_next_host_v1_sync_guard_release\n"
  "symbol channel_close chtholly_next_host_v1_channel_close\n"
  "symbol channel_init chtholly_next_host_v1_channel_init\n"
  "symbol channel_receive chtholly_next_host_v1_channel_receive_ref\n"
  "symbol channel_send chtholly_next_host_v1_channel_send_ref\n"
  "symbol typed_channel_init chtholly_next_host_v1_typed_channel_init\n"
  "symbol typed_channel_send_prepare chtholly_next_host_v1_typed_channel_send_prepare\n"
  "symbol typed_channel_send_commit chtholly_next_host_v1_typed_channel_send_commit\n"
  "symbol typed_channel_send_cancel chtholly_next_host_v1_typed_channel_send_cancel\n"
  "symbol typed_channel_receive_acquire chtholly_next_host_v1_typed_channel_receive_acquire\n"
  "symbol typed_channel_receive_commit chtholly_next_host_v1_typed_channel_receive_commit\n"
  "symbol typed_channel_receive_cancel chtholly_next_host_v1_typed_channel_receive_cancel\n"
  "symbol typed_channel_close chtholly_next_host_v1_typed_channel_close\n"
  "symbol monotonic_now chtholly_next_host_v1_monotonic_now\n"
  "symbol task_spawn chtholly_next_host_v1_task_spawn\n"
  "symbol task_poll chtholly_next_host_v1_task_poll_ref\n"
  "symbol task_cancel chtholly_next_host_v1_task_cancel_ref\n"
  "symbol task_wake chtholly_next_host_v1_task_wake_ref\n"
  "symbol task_join chtholly_next_host_v1_task_join\n"
  "symbol env_arg_count chtholly_next_runtime_v1_arg_count\n"
  "symbol env_arg_data chtholly_next_runtime_v1_arg_data\n"
  "symbol env_arg_size chtholly_next_runtime_v1_arg_size\n"
  "symbol console_write chtholly_next_runtime_v1_console_write\n"
  "symbol fs_exists chtholly_next_runtime_v1_fs_exists\n"
  "symbol fs_write chtholly_next_runtime_v1_fs_write\n"
  "symbol fs_remove chtholly_next_runtime_v1_fs_remove\n"
  "symbol allocator_allocate chtholly_next_runtime_v1_allocate\n"
  "symbol allocator_deallocate chtholly_next_runtime_v1_deallocate\n")
if(NOT WIN32)
  string(APPEND chtholly_runtime_link_manifest "system pthread\n")
else()
  string(APPEND chtholly_runtime_link_manifest "system ws2_32\n")
endif()
file(GENERATE
  OUTPUT "${chtholly_runtime_resource_dir}/chtholly_next_runtime_v1.links"
  CONTENT "${chtholly_runtime_link_manifest}")

set(chtholly_runtime_v2_link_manifest "version 2\n")
string(APPEND chtholly_runtime_v2_link_manifest
  "symbol allocator_allocate chtholly_next_runtime_v2_allocate\n"
  "symbol allocator_deallocate chtholly_next_runtime_v2_deallocate\n"
  "symbol net_resolve chtholly_next_host_v2_resolve\n"
  "symbol net_connect chtholly_next_host_v2_connect\n"
  "symbol net_bind chtholly_next_host_v2_bind\n"
  "symbol net_accept chtholly_next_host_v2_accept\n"
  "symbol net_read chtholly_next_host_v2_read\n"
  "symbol net_write chtholly_next_host_v2_write\n"
  "symbol net_shutdown chtholly_next_host_v2_shutdown\n"
  "symbol net_close chtholly_next_host_v2_close\n")
if(NOT WIN32)
  string(APPEND chtholly_runtime_v2_link_manifest "system pthread\n")
endif()
file(GENERATE
  OUTPUT "${chtholly_runtime_resource_dir}/chtholly_next_runtime_v2.links"
  CONTENT "${chtholly_runtime_v2_link_manifest}")

set(chtholly_container_link_manifest "version 1\n")
string(APPEND chtholly_container_link_manifest
  "symbol container_validate chtholly_next_container_v1_validate_vtable\n"
  "symbol container_init chtholly_next_container_v1_init\n"
  "symbol container_create chtholly_next_container_v1_create\n"
  "symbol container_size chtholly_next_container_v1_size\n"
  "symbol container_capacity chtholly_next_container_v1_capacity\n"
  "symbol container_generation chtholly_next_container_v1_generation\n"
  "symbol container_borrow_valid chtholly_next_container_v1_borrow_is_valid\n"
  "symbol container_destroy chtholly_next_container_v1_destroy\n"
  "symbol container_reserve chtholly_next_container_v1_reserve\n"
  "symbol container_find chtholly_next_container_v1_find\n"
  "symbol container_insert chtholly_next_container_v1_insert\n"
  "symbol container_erase chtholly_next_container_v1_erase\n"
  "symbol container_clear chtholly_next_container_v1_clear\n")
file(GENERATE
  OUTPUT "${chtholly_runtime_resource_dir}/chtholly_next_container_v1.links"
  CONTENT "${chtholly_container_link_manifest}")

install(TARGETS chtholly_next_runtime_v1
  ARCHIVE DESTINATION share/chtholly/runtime
  COMPONENT minimal)
install(TARGETS chtholly_next_runtime_v1
  ARCHIVE DESTINATION share/chtholly/runtime
  COMPONENT full)
install(TARGETS chtholly_next_runtime_v2
  ARCHIVE DESTINATION share/chtholly/runtime
  COMPONENT full)
install(TARGETS chtholly_next_container_v1
  ARCHIVE DESTINATION share/chtholly/runtime
  COMPONENT full)
install(TARGETS chtholly_component_loader_v1
  ARCHIVE DESTINATION lib
  COMPONENT full)
install(FILES
  "${chtholly_runtime_resource_dir}/chtholly_next_runtime_v1.links"
  DESTINATION share/chtholly/runtime
  COMPONENT minimal)
install(FILES
  "${chtholly_runtime_resource_dir}/chtholly_next_runtime_v1.links"
  DESTINATION share/chtholly/runtime
  COMPONENT full)
install(FILES
  "${chtholly_runtime_resource_dir}/chtholly_next_runtime_v2.links"
  "${chtholly_runtime_resource_dir}/chtholly_next_container_v1.links"
  DESTINATION share/chtholly/runtime
  COMPONENT full)
install(FILES
  include/chtholly/next_host_v1.h
  include/chtholly/next_hosted_async_v1.h
  include/chtholly/next_runtime_v1.h
  include/chtholly/next_container_v1.h
  include/chtholly/next_task_v1.h
  include/chtholly/next_host_v2.h
  include/chtholly/next_runtime_v2.h
  include/chtholly/component_abi_v1.h
  include/chtholly/component_loader_v1.h
  DESTINATION include/chtholly
  COMPONENT full)
