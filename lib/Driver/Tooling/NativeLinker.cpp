#include "chtholly/Driver/NativeLinker.h"

#include "chtholly/Driver/ProcessRunner.h"
#include "chtholly/Support/FileSystem.h"
#include "chtholly/ToolingRules/TargetToolchainContractRules.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>

namespace chtholly {

namespace {

std::string windowsLibraryName(std::string library) {
  if (library.size() < 4 || library.substr(library.size() - 4) != ".lib") {
    library += ".lib";
  }
  return library;
}

bool isLibraryFilePath(const std::string &library) {
  const std::filesystem::path path(library);
  const auto extension = path.extension().string();
  return path.is_absolute() || path.has_parent_path() || extension == ".lib" ||
         extension == ".a";
}

bool ensureOutputParent(const std::string &output_path, std::string &error) {
  std::error_code file_error;
  const auto parent = pathForFileSystem(output_path).parent_path();
  if (!parent.empty())
    std::filesystem::create_directories(parent, file_error);
  if (!file_error)
    return true;
  error = "failed to create native output directory for '" + output_path +
          "': " + file_error.message();
  return false;
}

#if defined(_WIN32)

bool replaceLinkedExecutable(const std::string &source,
                             const std::string &destination,
                             std::error_code &error) {
  constexpr int access_denied = 5;
  constexpr int sharing_violation = 32;
  constexpr int retry_count = 20;
  for (int attempt = 0; attempt < retry_count; ++attempt) {
    if (replaceFile(source, destination, error))
      return true;
    if (error.value() != access_denied && error.value() != sharing_violation)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

std::string executableSearchPath() {
  char *value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, "PATH") != 0 || value == nullptr) {
    return {};
  }
  std::string result(value);
  std::free(value);
  return result;
}

std::optional<std::string> findExecutableOnPath(std::string_view name) {
  const auto path_value = executableSearchPath();
  if (path_value.empty()) {
    return std::nullopt;
  }
  constexpr char separator = ';';
  std::string_view paths(path_value);
  std::size_t begin = 0;
  while (begin <= paths.size()) {
    const auto end = paths.find(separator, begin);
    auto directory =
        paths.substr(begin, end == std::string_view::npos ? paths.size() - begin
                                                          : end - begin);
    if (directory.size() >= 2 && directory.front() == '"' &&
        directory.back() == '"') {
      directory.remove_prefix(1);
      directory.remove_suffix(1);
    }
    if (!directory.empty()) {
      std::error_code error;
      const auto candidate =
          std::filesystem::path(std::string(directory)) / std::string(name);
      if (std::filesystem::is_regular_file(candidate, error) && !error) {
        return candidate.string();
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  return std::nullopt;
}

#endif

} // namespace

std::string defaultNativeLinkerForTarget(const TargetConfig &target) {
  if (!target.linker_path.empty()) {
    return target.linker_path;
  }
#if defined(_WIN32)
  if (!target.is_host_compatible) {
    return {};
  }
  if (const auto linker = findExecutableOnPath("lld-link.exe")) {
    return *linker;
  }
  return "link.exe";
#else
  if (!target.is_host_compatible) {
    return {};
  }
  return "cc";
#endif
}

namespace {

bool linkerUsesDashOptions(const std::string &linker) {
  const auto style = TargetToolchainContractResolver::classifyLinker(linker);
  return TargetToolchainContractResolver::linkerUsesDashOptions(style);
}

bool linkerSupportsTargetFlag(const std::string &linker) {
  const auto style = TargetToolchainContractResolver::classifyLinker(linker);
  return TargetToolchainContractResolver::linkerSupportsTargetFlag(linker,
                                                                   style);
}

} // namespace

void appendUniqueLinkValue(std::vector<std::string> &values,
                           const std::string &value) {
  if (value.empty()) {
    return;
  }
  for (const auto &existing : values) {
    if (existing == value) {
      return;
    }
  }
  values.push_back(value);
}

void appendHostedRuntimeSystemLibraries(std::vector<std::string> &libraries,
                                        const TargetConfig &target) {
  if (target.info.triple.find("windows") != std::string::npos) {
    appendUniqueLinkValue(libraries, "ws2_32");
  }
}

std::string temporaryObjectPathForExecutable(const std::string &output_path,
                                             const TargetConfig &target) {
#ifdef _WIN32
  static std::atomic<std::uint64_t> counter{0};
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto sequence = counter.fetch_add(1, std::memory_order_relaxed);
  const auto identity = std::to_string(std::hash<std::string>{}(output_path)) +
                        "-" + std::to_string(stamp) + "-" +
                        std::to_string(sequence);
  return (std::filesystem::temp_directory_path() /
          ("chtholly-link-" + identity + "." + target.object_extension))
      .string();
#else
  return output_path + "." + target.object_extension;
#endif
}

bool linkNativeExecutable(const std::vector<std::string> &object_paths,
                          const std::string &output_path,
                          const std::vector<std::string> &library_search_paths,
                          const std::vector<std::string> &libraries,
                          const TargetConfig &target, std::string &error) {
  if (object_paths.empty()) {
    error = "native executable linking requires at least one object";
    return false;
  }
  const auto linker = defaultNativeLinkerForTarget(target);
  if (linker.empty()) {
    error = "cannot link target '" + target.info.triple +
            "' without an explicit --linker; cross executable linking is not "
            "configured";
    return false;
  }
  if (!target.sysroot_path.empty() && isMsvcStyleLinker(linker)) {
    error = "linker '" + linker +
            "' does not support --sysroot; use a cc/clang-style linker or "
            "remove sysroot";
    return false;
  }
  if (!target.is_host_compatible && !isMsvcStyleLinker(linker) &&
      !linkerSupportsTargetFlag(linker)) {
    error = "cross linking target '" + target.info.triple +
            "' requires a clang-style linker that accepts --target";
    return false;
  }
  if (!ensureOutputParent(output_path, error))
    return false;

  std::string link_output_path = output_path;
#ifdef _WIN32
  link_output_path = object_paths.front() + ".linked.exe";
#endif

  std::vector<std::string> arguments;
  if (linkerUsesDashOptions(linker)) {
    if (!target.info.triple.empty() && linkerSupportsTargetFlag(linker)) {
      arguments.push_back("--target=" + target.info.triple);
    }
    // LLVM emits absolute references for the hosted entry object. Ubuntu's
    // GCC defaults to PIE, which rejects those relocations; keep the native
    // executable ABI stable by explicitly selecting the non-PIE link mode.
    if (target.info.triple.find("linux") != std::string::npos)
      arguments.push_back("-no-pie");
    for (const auto &object_path : object_paths) {
      arguments.push_back(pathForExternalTool(object_path));
    }
    arguments.push_back("-o");
    arguments.push_back(pathForExternalTool(link_output_path));
    if (!target.sysroot_path.empty()) {
      arguments.push_back("--sysroot=" +
                          pathForExternalTool(target.sysroot_path));
    }
    if (target.debug_info)
      arguments.push_back("-g");
    for (const auto &path : library_search_paths) {
      arguments.push_back("-L" + pathForExternalTool(path));
    }
    for (const auto &library : libraries) {
      arguments.push_back(isLibraryFilePath(library)
                              ? pathForExternalTool(library)
                              : "-l" + library);
    }
#if !defined(_WIN32)
#if defined(CHTHOLLY_NATIVE_LINK_SANITIZER_ADDRESS_UNDEFINED)
    arguments.push_back("-fsanitize=address,undefined");
    arguments.push_back("-shared-libsan");
#elif defined(CHTHOLLY_NATIVE_LINK_SANITIZER_ADDRESS)
    arguments.push_back("-fsanitize=address");
    arguments.push_back("-shared-libsan");
#elif defined(CHTHOLLY_NATIVE_LINK_SANITIZER_THREAD)
    arguments.push_back("-fsanitize=thread");
    arguments.push_back("-shared-libsan");
#endif
#endif
  } else {
    arguments = {"/nologo"};
    for (const auto &object_path : object_paths) {
      arguments.push_back(pathForExternalTool(object_path));
    }
    arguments.insert(arguments.end(),
                     {"/subsystem:console",
                      "/out:" + pathForExternalTool(link_output_path)});
    if (target.debug_info) {
      auto pdb_path = std::filesystem::path(output_path);
      pdb_path.replace_extension(".pdb");
      arguments.push_back("/debug");
      arguments.push_back("/pdb:" + pathForExternalTool(pdb_path.string()));
    }
    for (const auto &path : library_search_paths) {
      arguments.push_back("/LIBPATH:" + pathForExternalTool(path));
    }
    for (const auto &library : libraries) {
      arguments.push_back(isLibraryFilePath(library)
                              ? pathForExternalTool(library)
                              : windowsLibraryName(library));
    }
  }

  std::string process_error;
  auto result = runProcess(linker, arguments, process_error);
  if (!result) {
    error = "failed to start native linker '" + linker + "': " + process_error;
#ifdef _WIN32
    std::error_code remove_error;
    removeFile(link_output_path, remove_error);
#endif
    return false;
  }
  if (result->exit_code != 0) {
    error = summarizeCommandFailure("native linker '" + linker + "'", *result);
#ifdef _WIN32
    std::error_code remove_error;
    removeFile(link_output_path, remove_error);
#endif
    return false;
  }
#ifdef _WIN32
  std::error_code file_error;
  if (!replaceLinkedExecutable(link_output_path, output_path, file_error)) {
    error = "failed to move linked executable to '" + output_path +
            "': " + file_error.message();
    std::error_code remove_error;
    removeFile(link_output_path, remove_error);
    return false;
  }
#endif
  return true;
}

bool linkNativeExecutable(const std::string &object_path,
                          const std::string &output_path,
                          const std::vector<std::string> &library_search_paths,
                          const std::vector<std::string> &libraries,
                          const TargetConfig &target, std::string &error) {
  return linkNativeExecutable(std::vector<std::string>{object_path},
                              output_path, library_search_paths, libraries,
                              target, error);
}

bool linkNativeSharedLibrary(
    const std::vector<std::string> &object_paths,
    const std::string &output_path,
    const std::vector<std::string> &library_search_paths,
    const std::vector<std::string> &libraries, const TargetConfig &target,
    std::string &error, std::span<const std::string> exported_symbols) {
  if (object_paths.empty()) {
    error = "native shared-library linking requires at least one object";
    return false;
  }
  const auto linker = defaultNativeLinkerForTarget(target);
  if (linker.empty()) {
    error = "cannot link target '" + target.info.triple +
            "' without an explicit --linker; cross shared-library linking is "
            "not configured";
    return false;
  }
  if (!target.sysroot_path.empty() && isMsvcStyleLinker(linker)) {
    error = "linker '" + linker +
            "' does not support --sysroot; use a cc/clang-style linker or "
            "remove sysroot";
    return false;
  }
  if (!target.is_host_compatible && !isMsvcStyleLinker(linker) &&
      !linkerSupportsTargetFlag(linker)) {
    error = "cross linking target '" + target.info.triple +
            "' requires a clang-style linker that accepts --target";
    return false;
  }
  if (!ensureOutputParent(output_path, error))
    return false;

  std::vector<std::string> arguments;
  std::string export_map_path;
  if (linkerUsesDashOptions(linker)) {
    if (!target.info.triple.empty() && linkerSupportsTargetFlag(linker)) {
      arguments.push_back("--target=" + target.info.triple);
    }
    arguments.push_back("-shared");
    if (!exported_symbols.empty() &&
        target.info.triple.find("linux") != std::string::npos) {
      std::string map = "{\n  global:\n";
      for (const auto &symbol : exported_symbols)
        map += "    " + symbol + ";\n";
      map += "  local: *;\n};\n";
      export_map_path = output_path + ".exports.map";
      if (!writeTextFile(export_map_path, map, error))
        return false;
      arguments.push_back("-Wl,--version-script=" +
                          pathForExternalTool(export_map_path));
      arguments.push_back("-Wl,--exclude-libs,ALL");
    }
    for (const auto &object_path : object_paths) {
      arguments.push_back(pathForExternalTool(object_path));
    }
    arguments.push_back("-o");
    arguments.push_back(pathForExternalTool(output_path));
    if (!target.sysroot_path.empty()) {
      arguments.push_back("--sysroot=" +
                          pathForExternalTool(target.sysroot_path));
    }
    for (const auto &path : library_search_paths) {
      arguments.push_back("-L" + pathForExternalTool(path));
    }
    for (const auto &library : libraries) {
      arguments.push_back(isLibraryFilePath(library)
                              ? pathForExternalTool(library)
                              : "-l" + library);
    }
#if !defined(_WIN32)
#if defined(CHTHOLLY_NATIVE_LINK_SANITIZER_ADDRESS_UNDEFINED)
    arguments.push_back("-fsanitize=address,undefined");
#elif defined(CHTHOLLY_NATIVE_LINK_SANITIZER_ADDRESS)
    arguments.push_back("-fsanitize=address");
#elif defined(CHTHOLLY_NATIVE_LINK_SANITIZER_THREAD)
    arguments.push_back("-fsanitize=thread");
#endif
#endif
  } else {
    arguments = {"/nologo", "/dll"};
    for (const auto &object_path : object_paths) {
      arguments.push_back(pathForExternalTool(object_path));
    }
    arguments.push_back("/out:" + pathForExternalTool(output_path));
    arguments.push_back("msvcrt.lib");
    const auto import_library =
        pathForFileSystem(output_path).replace_extension(".lib").string();
    arguments.push_back("/implib:" + pathForExternalTool(import_library));
    for (const auto &path : library_search_paths) {
      arguments.push_back("/LIBPATH:" + pathForExternalTool(path));
    }
    for (const auto &library : libraries) {
      arguments.push_back(isLibraryFilePath(library)
                              ? pathForExternalTool(library)
                              : windowsLibraryName(library));
    }
  }

  std::string process_error;
  auto result = runProcess(linker, arguments, process_error);
  if (!export_map_path.empty()) {
    std::error_code remove_error;
    removeFile(export_map_path, remove_error);
  }
  if (!result) {
    error = "failed to start native linker '" + linker + "': " + process_error;
    return false;
  }
  if (result->exit_code != 0) {
    error = summarizeCommandFailure("native shared linker '" + linker + "'",
                                    *result);
    return false;
  }
  return true;
}

} // namespace chtholly
