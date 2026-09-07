#include "chtholly/Driver/ResourceLocator.h"

#include "chtholly/Driver/CompilerInvocation.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace chtholly {

namespace {

std::string runtimeArchiveName() {
#if defined(_WIN32)
  return "chtholly_next_runtime_v1.lib";
#else
  return "libchtholly_next_runtime_v1.a";
#endif
}

std::string containerRuntimeArchiveName() {
#if defined(_WIN32)
  return "chtholly_next_container_v1.lib";
#else
  return "libchtholly_next_container_v1.a";
#endif
}

constexpr std::string_view kRuntimeLinkManifestName =
    "chtholly_next_runtime_v1.links";

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

bool isSafeSystemLibraryName(std::string_view name) {
  if (name.empty()) {
    return false;
  }
  for (const char character : name) {
    const bool accepted =
        (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '_' ||
        character == '-' || character == '+' || character == '.';
    if (!accepted) {
      return false;
    }
  }
  return true;
}

bool isSafeRuntimeSymbol(std::string_view name) {
  if (name.empty())
    return false;
  for (const char character : name) {
    const bool accepted =
        (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '_' ||
        character == ':';
    if (!accepted)
      return false;
  }
  return true;
}

bool isPathWithin(const std::filesystem::path &root,
                  const std::filesystem::path &candidate) {
  auto root_it = root.begin();
  auto candidate_it = candidate.begin();
  for (; root_it != root.end(); ++root_it, ++candidate_it) {
    if (candidate_it == candidate.end() || *root_it != *candidate_it) {
      return false;
    }
  }
  return true;
}

bool parseRuntimeLinkManifest(const std::filesystem::path &runtime_dir,
                              ResourceLayout &layout, std::string &error) {
  const auto manifest = runtime_dir / kRuntimeLinkManifestName;
  std::error_code ec;
  if (!std::filesystem::exists(manifest, ec) || ec) {
    return true;
  }
  if (!std::filesystem::is_regular_file(manifest, ec) || ec) {
    error = "runtime link manifest is not a regular file: '" +
            manifest.string() + "'";
    return false;
  }
  const auto size = std::filesystem::file_size(manifest, ec);
  if (ec || size > 64U * 1024U) {
    error = "runtime link manifest exceeds the 64 KiB format limit: '" +
            manifest.string() + "'";
    return false;
  }

  std::ifstream input(manifest);
  if (!input) {
    error = "failed to read runtime link manifest '" + manifest.string() +
            "'";
    return false;
  }
  const auto canonical_runtime =
      std::filesystem::weakly_canonical(runtime_dir, ec);
  if (ec) {
    error = "failed to resolve runtime resource directory '" +
            runtime_dir.string() + "'";
    return false;
  }

  bool saw_version = false;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    line = trim(std::move(line));
    if (line.empty() || line.front() == '#') {
      continue;
    }
    std::istringstream fields(line);
    std::string kind;
    std::string value;
    std::string extra;
    fields >> kind >> value;
    if (kind == "version" && value == "1" && !(fields >> extra) &&
        !saw_version && layout.runtime_link_libraries.empty()) {
      saw_version = true;
      continue;
    }
    if (!saw_version) {
      error = "runtime link manifest must begin with 'version 1'";
      return false;
    }
    if (kind == "system") {
      if (value.empty() || (fields >> extra)) {
        error = "invalid runtime link manifest entry at line " +
                std::to_string(line_number);
        return false;
      }
      if (!isSafeSystemLibraryName(value)) {
        error = "unsafe system library name in runtime link manifest at line " +
                std::to_string(line_number);
        return false;
      }
      layout.runtime_link_libraries.push_back(std::move(value));
      continue;
    }
    if (kind == "symbol") {
      std::string runtime_symbol;
      if (!(fields >> runtime_symbol) || (fields >> extra) ||
          !isSafeRuntimeSymbol(value) || !isSafeRuntimeSymbol(runtime_symbol)) {
        error = "invalid runtime symbol mapping at line " +
                std::to_string(line_number);
        return false;
      }
      for (const auto &mapping : layout.runtime_symbol_mappings) {
        if (mapping.source_symbol == value ||
            mapping.runtime_symbol == runtime_symbol) {
          error = "duplicate runtime symbol mapping at line " +
                  std::to_string(line_number);
          return false;
        }
      }
      layout.runtime_symbol_mappings.push_back(
          RuntimeSymbolMapping{std::move(value), std::move(runtime_symbol)});
      continue;
    }
    if (kind != "file") {
      error = "unknown runtime link manifest entry '" + kind +
              "' at line " + std::to_string(line_number);
      return false;
    }

    if (value.empty() || (fields >> extra)) {
      error = "invalid runtime link manifest entry at line " +
              std::to_string(line_number);
      return false;
    }

    const std::filesystem::path relative(value);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
        value.find('\\') != std::string::npos) {
      error = "runtime link file must be a portable relative path at line " +
              std::to_string(line_number);
      return false;
    }
    for (const auto &component : relative) {
      if (component == ".." || component == ".") {
        error = "runtime link file escapes its resource directory at line " +
                std::to_string(line_number);
        return false;
      }
    }
    const auto resolved =
        std::filesystem::weakly_canonical(runtime_dir / relative, ec);
    if (ec || !isPathWithin(canonical_runtime, resolved) ||
        !std::filesystem::is_regular_file(resolved, ec) || ec) {
      error = "runtime link file is missing or outside its resource directory "
              "at line " +
              std::to_string(line_number) + ": '" + value + "'";
      return false;
    }
    layout.runtime_link_libraries.push_back(resolved.string());
  }
  if (!saw_version) {
    error = "runtime link manifest must begin with 'version 1'";
    return false;
  }
  layout.runtime_link_manifest_path = manifest.string();
  return true;
}

std::optional<ResourceLayout>
validateResourceDir(const std::filesystem::path &resource_dir,
                    std::string *error = nullptr) {
  std::error_code ec;
  const auto stdlib = resource_dir / "stdlib";
  const auto runtime = resource_dir / "runtime" / runtimeArchiveName();
  if (!std::filesystem::is_directory(stdlib, ec) || ec) {
    return std::nullopt;
  }
  ec.clear();
  if (!std::filesystem::is_regular_file(runtime, ec) || ec) {
    return std::nullopt;
  }
  const auto canonical = std::filesystem::weakly_canonical(resource_dir, ec);
  const auto root = (ec ? resource_dir : canonical).lexically_normal();
  ResourceLayout layout{root.string(), (root / "stdlib").string(),
                        (root / "runtime" / runtimeArchiveName()).string(),
                        {}, {}, {}};
  std::string manifest_error;
  if (!parseRuntimeLinkManifest(root / "runtime", layout, manifest_error)) {
    if (error != nullptr) {
      *error = std::move(manifest_error);
    }
    return std::nullopt;
  }
  const auto container_runtime =
      root / "runtime" / containerRuntimeArchiveName();
  ec.clear();
  if (std::filesystem::is_regular_file(container_runtime, ec))
    layout.runtime_link_libraries.push_back(container_runtime.string());
  return layout;
}

std::filesystem::path executablePath(const CompilerInvocation &invocation) {
  std::error_code ec;
  std::filesystem::path path(invocation.executable_path);
  if (!path.empty()) {
    if (!path.is_absolute()) {
      path = std::filesystem::absolute(path, ec);
    }
    if (!ec && std::filesystem::is_regular_file(path, ec) && !ec) {
      const auto canonical = std::filesystem::weakly_canonical(path, ec);
      return (ec ? path : canonical).lexically_normal();
    }
  }
  path.clear();
#if defined(_WIN32)
  std::wstring buffer(32768, L'\0');
  const auto length = GetModuleFileNameW(
      nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length != 0 && length < buffer.size()) {
    buffer.resize(length);
    path = std::filesystem::path(buffer);
  }
#elif defined(__linux__)
  std::error_code link_error;
  path = std::filesystem::read_symlink("/proc/self/exe", link_error);
  if (link_error) {
    path.clear();
  }
#elif defined(__APPLE__)
  std::uint32_t size = 0;
  (void)_NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size);
  if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
    path = std::filesystem::path(buffer.data());
  }
#endif
  if (path.empty()) {
    return {};
  }
  ec.clear();
  if (!path.is_absolute()) {
    path = std::filesystem::absolute(path, ec);
  }
  if (!ec) {
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
      path = canonical;
    }
  }
  return path.lexically_normal();
}

} // namespace

std::optional<ResourceLayout>
locateCompilerResources(const CompilerInvocation &invocation,
                        std::string &error) {
  if (!invocation.resource_dir.empty()) {
    std::string validation_error;
    auto layout = validateResourceDir(invocation.resource_dir,
                                      &validation_error);
    if (!layout) {
      error = validation_error.empty()
                  ? "invalid --resource-dir '" + invocation.resource_dir +
                        "': expected stdlib/ and runtime/" +
                        runtimeArchiveName()
                  : std::move(validation_error);
    }
    return layout;
  }

  const auto executable = executablePath(invocation);
  if (executable.empty()) {
    return std::nullopt;
  }
  const auto executable_dir = executable.parent_path();
  const std::vector<std::filesystem::path> candidates = {
      executable_dir.parent_path().parent_path() / "share" / "chtholly",
      executable_dir.parent_path() / "share" / "chtholly"};
  for (const auto &candidate : candidates) {
    if (auto layout = validateResourceDir(candidate)) {
      return layout;
    }
  }
  return std::nullopt;
}

} // namespace chtholly
