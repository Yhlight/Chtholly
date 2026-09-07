cmake_minimum_required(VERSION 3.28)

if (MSVC)
    add_compile_options(/utf-8)
endif()

if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
endif()

set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")

project(Chtholly
  VERSION 0.2.0
  DESCRIPTION "The Chtholly language compiler"
  LANGUAGES C CXX)

# Static compiler/runtime libraries are linked into the native component
# fixtures on ELF hosts. Keep those archives position independent so shared
# ABI-2 providers can be linked by Clang/GCC without platform-specific target
# exceptions. MSVC does not require a PIC switch.
if(NOT MSVC)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY
      "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CHTHOLLY_VERSION_PRERELEASE "preview" CACHE STRING
    "Semantic version prerelease identifier; leave empty for a stable release")
set(CHTHOLLY_VERSION_FULL "${PROJECT_VERSION}")
if(CHTHOLLY_VERSION_PRERELEASE)
  string(APPEND CHTHOLLY_VERSION_FULL "-${CHTHOLLY_VERSION_PRERELEASE}")
endif()
set(CHTHOLLY_SOURCE_COMMIT "" CACHE STRING
    "Full lowercase 40-hex source commit for builds outside a Git checkout")
if(CHTHOLLY_SOURCE_COMMIT)
  string(LENGTH "${CHTHOLLY_SOURCE_COMMIT}" CHTHOLLY_SOURCE_COMMIT_LENGTH)
  if(NOT CHTHOLLY_SOURCE_COMMIT_LENGTH EQUAL 40 OR
     NOT CHTHOLLY_SOURCE_COMMIT MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
      "CHTHOLLY_SOURCE_COMMIT must be a full lowercase 40-hex commit")
  endif()
else()
  execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE CHTHOLLY_GIT_RESULT
    OUTPUT_VARIABLE CHTHOLLY_DETECTED_SOURCE_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  string(LENGTH "${CHTHOLLY_DETECTED_SOURCE_COMMIT}"
         CHTHOLLY_DETECTED_SOURCE_COMMIT_LENGTH)
  if(CHTHOLLY_GIT_RESULT EQUAL 0 AND
     CHTHOLLY_DETECTED_SOURCE_COMMIT_LENGTH EQUAL 40 AND
     CHTHOLLY_DETECTED_SOURCE_COMMIT MATCHES "^[0-9a-f]+$")
    set(CHTHOLLY_SOURCE_COMMIT "${CHTHOLLY_DETECTED_SOURCE_COMMIT}")
  else()
    set(CHTHOLLY_SOURCE_COMMIT "unknown")
  endif()
endif()
set(CHTHOLLY_RELEASE_ID
    "${CHTHOLLY_VERSION_FULL}+${CHTHOLLY_SOURCE_COMMIT}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(CHTHOLLY_BUILD_TOOLS "Build Chtholly command line tools" ON)
option(CHTHOLLY_BUILD_TESTS "Build the Chtholly test suite" ON)
option(CHTHOLLY_USE_SCCACHE "Use sccache as the C++ compiler launcher when available" OFF)
option(CHTHOLLY_FULL_DEBUG_ARTIFACTS "Keep per-executable linker debug artifacts for local debugging" OFF)
option(CHTHOLLY_ALLOW_SYSTEM_LLVM_IMPORTED_TARGET_SHIMS "Allow non-Windows system LLVM package dependency target shims" ON)
option(CHTHOLLY_WARNINGS_AS_ERRORS "Treat warnings in Chtholly targets as errors" OFF)
option(CHTHOLLY_BUILD_FUZZERS "Build Clang LibFuzzer runtime targets" OFF)
option(CHTHOLLY_BUILD_VSCODE_EXTENSION
       "Build the Chtholly VS Code VSIX package" OFF)
set(CHTHOLLY_INSTALL_PROFILE "full" CACHE STRING
    "Install profile selected for packaging (minimal or full)")
set_property(CACHE CHTHOLLY_INSTALL_PROFILE PROPERTY STRINGS minimal full)
if(NOT CHTHOLLY_INSTALL_PROFILE STREQUAL "minimal" AND
   NOT CHTHOLLY_INSTALL_PROFILE STREQUAL "full")
  message(FATAL_ERROR
    "CHTHOLLY_INSTALL_PROFILE must be either minimal or full")
endif()
set(CHTHOLLY_SANITIZER "" CACHE STRING
    "Runtime sanitizer configuration: address, address,undefined, or thread")

if(CHTHOLLY_SANITIZER)
  if(MSVC)
    if(NOT CHTHOLLY_SANITIZER STREQUAL "address")
      message(FATAL_ERROR "MSVC supports only CHTHOLLY_SANITIZER=address")
    endif()
    add_compile_options(/fsanitize=address /Oy-
      /D_DISABLE_VECTOR_ANNOTATION /D_DISABLE_STRING_ANNOTATION)
    add_link_options(/fsanitize=address)
  elseif(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU" OR
         NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "CHTHOLLY_SANITIZER requires MSVC, Clang, or GCC")
  elseif(CHTHOLLY_SANITIZER STREQUAL "address")
    # Keep one shared sanitizer runtime across the executable and dynamically
    # loaded ABI-2 provider.  Clang's spelling is shared-libasan; using the
    # generic/incorrect shared-libsan spelling silently falls back to a
    # second -lasan in the provider and makes dlopen fail at runtime.
    set(CHTHOLLY_SANITIZER_FLAGS -fsanitize=address -shared-libasan)
    add_compile_definitions(CHTHOLLY_NATIVE_LINK_SANITIZER_ADDRESS)
  elseif(CHTHOLLY_SANITIZER STREQUAL "address,undefined")
    set(CHTHOLLY_SANITIZER_FLAGS "-fsanitize=address,undefined")
    add_compile_definitions(CHTHOLLY_NATIVE_LINK_SANITIZER_ADDRESS_UNDEFINED)
  elseif(CHTHOLLY_SANITIZER STREQUAL "thread")
    set(CHTHOLLY_SANITIZER_FLAGS "-fsanitize=thread")
    add_compile_definitions(CHTHOLLY_NATIVE_LINK_SANITIZER_THREAD)
  else()
    message(FATAL_ERROR
      "CHTHOLLY_SANITIZER must be empty, address,undefined, or thread")
  endif()
  if(NOT MSVC)
    add_compile_options(${CHTHOLLY_SANITIZER_FLAGS} -fno-omit-frame-pointer)
    add_link_options(${CHTHOLLY_SANITIZER_FLAGS})
  endif()
endif()

if(CHTHOLLY_BUILD_FUZZERS AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "CHTHOLLY_BUILD_FUZZERS requires Clang")
endif()

function(chtholly_configure_msvc_exe_linker_debug_artifacts flag_variable)
  if(NOT DEFINED ${flag_variable})
    return()
  endif()
  set(CHTHOLLY_ORIGINAL_FLAG_VARIABLE "CHTHOLLY_ORIGINAL_${flag_variable}")
  if(NOT DEFINED ${CHTHOLLY_ORIGINAL_FLAG_VARIABLE})
    set(${CHTHOLLY_ORIGINAL_FLAG_VARIABLE} "${${flag_variable}}" CACHE INTERNAL "Original ${flag_variable}")
  endif()
  set(CHTHOLLY_LINK_FLAGS "${${CHTHOLLY_ORIGINAL_FLAG_VARIABLE}}")
  if(CHTHOLLY_FULL_DEBUG_ARTIFACTS)
    set(${flag_variable} "${CHTHOLLY_LINK_FLAGS}" CACHE STRING "Chtholly executable linker flags" FORCE)
    return()
  endif()
  string(REGEX REPLACE "(^| )[/-][Dd][Ee][Bb][Uu][Gg](:[^ ]*)?( |$)" " " CHTHOLLY_LINK_FLAGS "${CHTHOLLY_LINK_FLAGS}")
  string(REGEX REPLACE "(^| )[/-][Ii][Nn][Cc][Rr][Ee][Mm][Ee][Nn][Tt][Aa][Ll]( |$)" " " CHTHOLLY_LINK_FLAGS "${CHTHOLLY_LINK_FLAGS}")
  string(REGEX REPLACE " +" " " CHTHOLLY_LINK_FLAGS "${CHTHOLLY_LINK_FLAGS}")
  string(STRIP "${CHTHOLLY_LINK_FLAGS}" CHTHOLLY_LINK_FLAGS)
  set(${flag_variable} "${CHTHOLLY_LINK_FLAGS}" CACHE STRING "Lean Chtholly executable linker flags" FORCE)
endfunction()

if(MSVC)
  chtholly_configure_msvc_exe_linker_debug_artifacts(CMAKE_EXE_LINKER_FLAGS_DEBUG)
  chtholly_configure_msvc_exe_linker_debug_artifacts(CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO)
endif()

if(CHTHOLLY_USE_SCCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
  find_program(CHTHOLLY_SCCACHE_PROGRAM sccache)
  if(CHTHOLLY_SCCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CHTHOLLY_SCCACHE_PROGRAM}" CACHE STRING "C++ compiler launcher" FORCE)
  endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(unofficial-sodium CONFIG QUIET)
if(NOT TARGET unofficial-sodium::sodium)
  find_path(CHTHOLLY_SODIUM_INCLUDE_DIR sodium.h REQUIRED)
  find_library(CHTHOLLY_SODIUM_LIBRARY NAMES sodium libsodium REQUIRED)
  add_library(unofficial-sodium::sodium UNKNOWN IMPORTED)
  set_target_properties(unofficial-sodium::sodium PROPERTIES
    IMPORTED_LOCATION "${CHTHOLLY_SODIUM_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${CHTHOLLY_SODIUM_INCLUDE_DIR}")
endif()
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(httplib CONFIG QUIET)
if(NOT TARGET httplib::httplib)
  find_path(CHTHOLLY_HTTPLIB_INCLUDE_DIR httplib.h REQUIRED)
  find_library(CHTHOLLY_HTTPLIB_LIBRARY NAMES cpp-httplib httplib)
  find_package(Threads REQUIRED)
  add_library(httplib::httplib INTERFACE IMPORTED)
  set_target_properties(httplib::httplib PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CHTHOLLY_HTTPLIB_INCLUDE_DIR}"
    INTERFACE_COMPILE_DEFINITIONS CPPHTTPLIB_OPENSSL_SUPPORT
    INTERFACE_LINK_LIBRARIES "OpenSSL::SSL;OpenSSL::Crypto;Threads::Threads")
  if(CHTHOLLY_HTTPLIB_LIBRARY)
    set_property(TARGET httplib::httplib APPEND PROPERTY
      INTERFACE_LINK_LIBRARIES "${CHTHOLLY_HTTPLIB_LIBRARY}")
  endif()
endif()
find_package(unofficial-sqlite3 CONFIG QUIET)
if(NOT TARGET unofficial::sqlite3::sqlite3)
  find_path(CHTHOLLY_SQLITE3_INCLUDE_DIR sqlite3.h REQUIRED)
  find_library(CHTHOLLY_SQLITE3_LIBRARY NAMES sqlite3 REQUIRED)
  add_library(unofficial::sqlite3::sqlite3 UNKNOWN IMPORTED)
  set_target_properties(unofficial::sqlite3::sqlite3 PROPERTIES
    IMPORTED_LOCATION "${CHTHOLLY_SQLITE3_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${CHTHOLLY_SQLITE3_INCLUDE_DIR}")
endif()
include(ChthollyLLVM)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/chtholly/Config/Version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/include/chtholly/Config/Version.h"
  @ONLY)

function(chtholly_configure_warnings target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE
      /W4
      "$<$<COMPILE_LANGUAGE:CXX>:/permissive->"
      "$<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>")
    if(CHTHOLLY_WARNINGS_AS_ERRORS)
      target_compile_options(${target_name} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    if(CHTHOLLY_WARNINGS_AS_ERRORS)
      target_compile_options(${target_name} PRIVATE -Werror)
    endif()
  endif()
endfunction()

function(chtholly_configure_target target_name)
  target_include_directories(${target_name}
    PUBLIC
      "${PROJECT_SOURCE_DIR}/include"
      "${PROJECT_BINARY_DIR}/include")
  chtholly_configure_warnings(${target_name})
endfunction()

function(chtholly_configure_executable_artifacts target_name)
  if(MSVC AND NOT CHTHOLLY_FULL_DEBUG_ARTIFACTS)
    target_link_options(${target_name}
      PRIVATE
        "$<$<CONFIG:Debug,RelWithDebInfo>:/DEBUG:NONE>"
        "$<$<CONFIG:Debug,RelWithDebInfo>:/INCREMENTAL:NO>"
        "$<$<CONFIG:Debug,RelWithDebInfo>:/OPT:REF>"
        "$<$<CONFIG:Debug,RelWithDebInfo>:/OPT:ICF>")
  endif()
endfunction()

add_subdirectory(third_party/miniz)
add_subdirectory(lib)
add_subdirectory(runtime)

add_custom_target(chtholly_resources ALL
  COMMAND "${CMAKE_COMMAND}" -E rm -rf
          "${PROJECT_BINARY_DIR}/share/chtholly/stdlib"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${PROJECT_BINARY_DIR}/share/chtholly/stdlib"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${PROJECT_SOURCE_DIR}/stdlib"
          "${PROJECT_BINARY_DIR}/share/chtholly/stdlib"
  COMMENT "Staging Chtholly standard library resources")

if(CHTHOLLY_BUILD_VSCODE_EXTENSION)
  find_program(CHTHOLLY_POWERSHELL_PROGRAM NAMES pwsh powershell REQUIRED)
  set(CHTHOLLY_VSCODE_VSIX
      "${PROJECT_BINARY_DIR}/share/chtholly/vscode/chtholly-vscode-${CHTHOLLY_VERSION_FULL}.vsix")
  add_custom_command(
    OUTPUT "${CHTHOLLY_VSCODE_VSIX}"
    COMMAND "${CHTHOLLY_POWERSHELL_PROGRAM}" -NoProfile -File
            "${PROJECT_SOURCE_DIR}/scripts/package-vscode-extension.ps1"
            -OutputPath "${CHTHOLLY_VSCODE_VSIX}"
    DEPENDS
      "${PROJECT_SOURCE_DIR}/editors/vscode/package.json"
      "${PROJECT_SOURCE_DIR}/editors/vscode/package-lock.json"
      "${PROJECT_SOURCE_DIR}/editors/vscode/extension.js"
      "${PROJECT_SOURCE_DIR}/editors/vscode/syntaxes/chtholly.tmLanguage.json"
    VERBATIM)
  add_custom_target(chtholly-vscode-vsix ALL DEPENDS "${CHTHOLLY_VSCODE_VSIX}")
  install(FILES "${CHTHOLLY_VSCODE_VSIX}"
          DESTINATION share/chtholly/vscode
          COMPONENT full)
endif()

if(CHTHOLLY_BUILD_TOOLS)
  add_subdirectory(tools)
  chtholly_install_libclang_runtime(full)
endif()

if(CHTHOLLY_BUILD_TESTS)
  add_library(chtholly_sqlite_safety_provider STATIC
    examples/sqlite-safety/native/provider.c)
  chtholly_configure_target(chtholly_sqlite_safety_provider)
  target_link_libraries(chtholly_sqlite_safety_provider PRIVATE
    unofficial::sqlite3::sqlite3)
  add_subdirectory(examples/component-host/host)
  add_subdirectory(examples/telemetry-pipeline/host)
  add_subdirectory(tests)
endif()

install(FILES
  "${PROJECT_SOURCE_DIR}/LICENSE"
  "${PROJECT_SOURCE_DIR}/LICENSE-APACHE"
  "${PROJECT_SOURCE_DIR}/LICENSE-MIT"
  "${PROJECT_SOURCE_DIR}/README.md"
  DESTINATION share/chtholly
  COMPONENT minimal)
install(FILES
  "${PROJECT_SOURCE_DIR}/LICENSE"
  "${PROJECT_SOURCE_DIR}/LICENSE-APACHE"
  "${PROJECT_SOURCE_DIR}/LICENSE-MIT"
  "${PROJECT_SOURCE_DIR}/README.md"
  DESTINATION share/chtholly
  COMPONENT full)

install(FILES
  "${PROJECT_SOURCE_DIR}/CONTRIBUTING.md"
  DESTINATION share/chtholly
  COMPONENT full)

install(DIRECTORY
  "${PROJECT_SOURCE_DIR}/docs"
  DESTINATION share/chtholly
  COMPONENT full
  FILES_MATCHING
  PATTERN "*.md"
  PATTERN "internal" EXCLUDE)

install(FILES
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.1.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.2.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.3.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.4.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.5.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.6.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.7.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.8.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.9.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.10.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-feature-evidence.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-cffi-tier1.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-branding-trace-policy.toml"
  "${PROJECT_SOURCE_DIR}/support/supply-chain-lock.json"
  DESTINATION share/chtholly/support
  COMPONENT minimal)
install(FILES
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.1.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.2.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.3.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.4.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.5.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.6.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.7.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.8.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.9.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-v1.10.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-feature-evidence.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-cffi-tier1.toml"
  "${PROJECT_SOURCE_DIR}/support/chtholly-branding-trace-policy.toml"
  "${PROJECT_SOURCE_DIR}/support/supply-chain-lock.json"
  DESTINATION share/chtholly/support
  COMPONENT full)

install(DIRECTORY
  "${PROJECT_SOURCE_DIR}/stdlib/"
  DESTINATION share/chtholly/stdlib
  COMPONENT minimal
  FILES_MATCHING
  PATTERN "*.cns"
  PATTERN "*.cfdl"
  PATTERN "manifest.toml")
install(DIRECTORY
  "${PROJECT_SOURCE_DIR}/stdlib/"
  DESTINATION share/chtholly/stdlib
  COMPONENT full
  FILES_MATCHING
  PATTERN "*.cns"
  PATTERN "*.cfdl"
  PATTERN "manifest.toml")

install(DIRECTORY
  "${PROJECT_SOURCE_DIR}/editors/vscode/"
  DESTINATION share/chtholly/vscode/source
  COMPONENT full
  PATTERN "node_modules" EXCLUDE
  )

set(CPACK_PACKAGE_NAME "Chtholly")
set(CPACK_PACKAGE_VENDOR "Chtholly Project")
if(WIN32)
  set(CHTHOLLY_CPACK_PLATFORM "windows-x64")
elseif(APPLE)
  set(CHTHOLLY_CPACK_PLATFORM "macos-arm64")
else()
  set(CHTHOLLY_CPACK_PLATFORM "linux-x64")
endif()
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Chtholly 0.2.0 developer preview for ${CHTHOLLY_CPACK_PLATFORM}")
set(CPACK_PACKAGE_VERSION "${CHTHOLLY_VERSION_FULL}")
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_FILE_NAME
    "chtholly-${CHTHOLLY_VERSION_FULL}-${CHTHOLLY_CPACK_PLATFORM}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
# Keep component selection meaningful for ZIP packages. Without this switch,
# CPack's ZIP generator includes every component even when
# CPACK_COMPONENTS_ALL requests a single profile.
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
# Keep CPack's default archive aligned with the requested packaging profile.
# Full repeats the minimal component rules so its archive remains a complete
# backward-compatible release tree; explicit `cmake --install --component`
# selection remains available for CI inventory checks.
set(CPACK_COMPONENTS_ALL "${CHTHOLLY_INSTALL_PROFILE}")
include(CPack)
