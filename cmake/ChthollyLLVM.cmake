set(CHTHOLLY_LOCAL_LLVM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/llvm/lib/cmake/llvm")
set(CHTHOLLY_LOCAL_CLANG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/llvm/lib/cmake/clang")
set(CHTHOLLY_LOCAL_LLVM_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/third_party/llvm")

function(chtholly_path_is_under path root output_variable)
  file(TO_CMAKE_PATH "${path}" CHTHOLLY_NORMALIZED_PATH)
  file(TO_CMAKE_PATH "${root}" CHTHOLLY_NORMALIZED_ROOT)
  string(TOLOWER "${CHTHOLLY_NORMALIZED_PATH}" CHTHOLLY_NORMALIZED_PATH_LOWER)
  string(TOLOWER "${CHTHOLLY_NORMALIZED_ROOT}" CHTHOLLY_NORMALIZED_ROOT_LOWER)
  string(FIND "${CHTHOLLY_NORMALIZED_PATH_LOWER}/" "${CHTHOLLY_NORMALIZED_ROOT_LOWER}/" CHTHOLLY_PREFIX_INDEX)
  if(CHTHOLLY_PREFIX_INDEX EQUAL 0)
    set(${output_variable} TRUE PARENT_SCOPE)
  else()
    set(${output_variable} FALSE PARENT_SCOPE)
  endif()
endfunction()

function(chtholly_reject_non_windows_local_llvm package_name package_dir)
  if(WIN32 OR NOT package_dir)
    return()
  endif()

  chtholly_path_is_under("${package_dir}" "${CHTHOLLY_LOCAL_LLVM_PREFIX}" CHTHOLLY_USES_LOCAL_LLVM)
  if(CHTHOLLY_USES_LOCAL_LLVM)
    message(FATAL_ERROR
      "Chtholly found ${package_name} in the checkout-local third_party/llvm on a non-Windows build: ${package_dir}. "
      "That layout is reserved for Windows/MSVC packages. Configure WSL/Linux with a system LLVM 18 package "
      "such as -DCMAKE_PREFIX_PATH=/usr/lib/llvm-18, or pass matching Linux LLVM_DIR and Clang_DIR values.")
  endif()
endfunction()

function(chtholly_find_system_library output_variable)
  foreach(CHTHOLLY_LIBRARY_NAME ${ARGN})
    find_library(CHTHOLLY_FOUND_SYSTEM_LIBRARY NAMES "${CHTHOLLY_LIBRARY_NAME}")
    if(CHTHOLLY_FOUND_SYSTEM_LIBRARY)
      set(${output_variable} "${CHTHOLLY_FOUND_SYSTEM_LIBRARY}" PARENT_SCOPE)
      unset(CHTHOLLY_FOUND_SYSTEM_LIBRARY CACHE)
      return()
    endif()
    unset(CHTHOLLY_FOUND_SYSTEM_LIBRARY CACHE)
  endforeach()
  set(${output_variable} "" PARENT_SCOPE)
endfunction()

function(chtholly_define_imported_shared_library_if_missing target_name library_path include_dir)
  if(TARGET ${target_name} OR NOT library_path OR NOT EXISTS "${library_path}")
    return()
  endif()

  add_library(${target_name} SHARED IMPORTED)
  set_target_properties(${target_name} PROPERTIES IMPORTED_LOCATION "${library_path}")
  if(include_dir AND EXISTS "${include_dir}")
    set_target_properties(${target_name} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${include_dir}")
  endif()
endfunction()

function(chtholly_define_system_llvm_dependency_shims)
  if(WIN32 OR NOT CHTHOLLY_ALLOW_SYSTEM_LLVM_IMPORTED_TARGET_SHIMS)
    return()
  endif()

  chtholly_find_system_library(CHTHOLLY_ZSTD_LIBRARY zstd libzstd.so.1)
  chtholly_find_system_library(CHTHOLLY_CURL_LIBRARY curl libcurl.so.4)
  chtholly_find_system_library(CHTHOLLY_LIBEDIT_LIBRARY edit libedit.so.2)

  chtholly_define_imported_shared_library_if_missing(
    zstd::libzstd_shared "${CHTHOLLY_ZSTD_LIBRARY}" "/usr/include")
  chtholly_define_imported_shared_library_if_missing(
    CURL::libcurl "${CHTHOLLY_CURL_LIBRARY}" "")
  chtholly_define_imported_shared_library_if_missing(
    LibEdit::LibEdit "${CHTHOLLY_LIBEDIT_LIBRARY}" "")
endfunction()

function(chtholly_relocate_llvm_dia_sdk)
  if(NOT MSVC OR NOT TARGET LLVMDebugInfoPDB)
    return()
  endif()

  get_target_property(CHTHOLLY_LLVM_PDB_LINK_LIBRARIES
    LLVMDebugInfoPDB INTERFACE_LINK_LIBRARIES)
  if(NOT CHTHOLLY_LLVM_PDB_LINK_LIBRARIES)
    return()
  endif()

  set(CHTHOLLY_LLVM_PDB_REQUIRES_RELOCATION FALSE)
  foreach(CHTHOLLY_LLVM_LINK_LIBRARY IN LISTS CHTHOLLY_LLVM_PDB_LINK_LIBRARIES)
    file(TO_CMAKE_PATH "${CHTHOLLY_LLVM_LINK_LIBRARY}"
      CHTHOLLY_NORMALIZED_LLVM_LINK_LIBRARY)
    if(CHTHOLLY_NORMALIZED_LLVM_LINK_LIBRARY MATCHES
         "/DIA SDK/lib/amd64/diaguids[.]lib$" AND
       NOT EXISTS "${CHTHOLLY_LLVM_LINK_LIBRARY}")
      set(CHTHOLLY_LLVM_PDB_REQUIRES_RELOCATION TRUE)
      break()
    endif()
  endforeach()
  if(NOT CHTHOLLY_LLVM_PDB_REQUIRES_RELOCATION)
    return()
  endif()

  set(CHTHOLLY_DIA_GUIDS_LIBRARY
    "$ENV{VSINSTALLDIR}/DIA SDK/lib/amd64/diaguids.lib")
  file(TO_CMAKE_PATH "${CHTHOLLY_DIA_GUIDS_LIBRARY}"
    CHTHOLLY_DIA_GUIDS_LIBRARY)
  if(NOT EXISTS "${CHTHOLLY_DIA_GUIDS_LIBRARY}")
    message(FATAL_ERROR
      "LLVM's LLVMDebugInfoPDB target refers to a missing DIA SDK. "
      "Configure from an MSVC environment that defines VSINSTALLDIR and "
      "contains DIA SDK/lib/amd64/diaguids.lib.")
  endif()

  set(CHTHOLLY_RELOCATED_LLVM_PDB_LINK_LIBRARIES)
  foreach(CHTHOLLY_LLVM_LINK_LIBRARY IN LISTS CHTHOLLY_LLVM_PDB_LINK_LIBRARIES)
    file(TO_CMAKE_PATH "${CHTHOLLY_LLVM_LINK_LIBRARY}"
      CHTHOLLY_NORMALIZED_LLVM_LINK_LIBRARY)
    if(CHTHOLLY_NORMALIZED_LLVM_LINK_LIBRARY MATCHES
         "/DIA SDK/lib/amd64/diaguids[.]lib$")
      list(APPEND CHTHOLLY_RELOCATED_LLVM_PDB_LINK_LIBRARIES
        "${CHTHOLLY_DIA_GUIDS_LIBRARY}")
    else()
      list(APPEND CHTHOLLY_RELOCATED_LLVM_PDB_LINK_LIBRARIES
        "${CHTHOLLY_LLVM_LINK_LIBRARY}")
    endif()
  endforeach()
  set_target_properties(LLVMDebugInfoPDB PROPERTIES
    INTERFACE_LINK_LIBRARIES "${CHTHOLLY_RELOCATED_LLVM_PDB_LINK_LIBRARIES}")
  message(STATUS "Relocated LLVM DIA SDK dependency: ${CHTHOLLY_DIA_GUIDS_LIBRARY}")
endfunction()

if(WIN32 AND EXISTS "${CHTHOLLY_LOCAL_LLVM_DIR}/LLVMConfig.cmake" AND
   EXISTS "${CHTHOLLY_LOCAL_CLANG_DIR}/ClangConfig.cmake")
  list(PREPEND CMAKE_PREFIX_PATH "${CHTHOLLY_LOCAL_LLVM_PREFIX}")
endif()

chtholly_define_system_llvm_dependency_shims()

find_package(LLVM CONFIG REQUIRED)
find_package(Clang CONFIG REQUIRED)

chtholly_relocate_llvm_dia_sdk()

chtholly_reject_non_windows_local_llvm("LLVM" "${LLVM_DIR}")
chtholly_reject_non_windows_local_llvm("Clang" "${Clang_DIR}")

if(NOT LLVM_VERSION_MAJOR EQUAL 18)
  message(FATAL_ERROR "Chtholly requires LLVM 18, found LLVM ${LLVM_PACKAGE_VERSION}")
endif()

if(NOT WIN32)
  foreach(CHTHOLLY_LLVM_LIB ${LLVM_AVAILABLE_LIBS})
    if(CHTHOLLY_LLVM_LIB MATCHES "\\.lib$")
      message(FATAL_ERROR
        "Chtholly found a Windows LLVM library while configuring a non-Windows build: ${CHTHOLLY_LLVM_LIB}. "
        "Use a Linux/WSL LLVM 18 CMake package instead of the Windows third_party/llvm layout.")
    endif()
  endforeach()
endif()

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}: ${LLVM_DIR}")
message(STATUS "Found Clang: ${Clang_DIR}")

function(chtholly_link_llvm target_name)
  target_include_directories(${target_name} SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_compile_definitions(${target_name} PRIVATE ${LLVM_DEFINITIONS})
  if(LLVM_LINK_LLVM_DYLIB AND TARGET LLVM)
    target_link_libraries(${target_name} PRIVATE LLVM)
    return()
  endif()
  llvm_map_components_to_libnames(CHTHOLLY_LLVM_LIBS ${ARGN})
  target_link_libraries(${target_name} PRIVATE ${CHTHOLLY_LLVM_LIBS})
endfunction()

function(chtholly_link_libclang target_name)
  target_include_directories(${target_name} SYSTEM PRIVATE ${CLANG_INCLUDE_DIRS})
  target_link_libraries(${target_name} PRIVATE libclang)
endfunction()

function(chtholly_copy_libclang_runtime target_name)
  if(WIN32 AND TARGET libclang)
    foreach(CHTHOLLY_LIBCLANG_RUNTIME_DIR ${ARGN})
      add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CHTHOLLY_LIBCLANG_RUNTIME_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:libclang>"
                "${CHTHOLLY_LIBCLANG_RUNTIME_DIR}")
    endforeach()
  endif()
endfunction()

function(chtholly_install_libclang_runtime component_name)
  if(WIN32 AND TARGET libclang)
    set(CHTHOLLY_LIBCLANG_INSTALL_DESTINATION bin)
    if(ARGC GREATER 1)
      set(CHTHOLLY_LIBCLANG_INSTALL_DESTINATION "${ARGV1}")
    endif()
    install(FILES "$<TARGET_FILE:libclang>"
      DESTINATION "${CHTHOLLY_LIBCLANG_INSTALL_DESTINATION}"
      COMPONENT ${component_name})
  endif()
endfunction()
