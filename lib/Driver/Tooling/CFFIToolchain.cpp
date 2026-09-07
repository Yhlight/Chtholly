#include "chtholly/Driver/CFFIToolchain.h"

#include "ManifestToml.h"
#include "chtholly/Driver/ProcessRunner.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <unordered_map>

namespace chtholly {
namespace {

std::optional<std::string> environmentValue(std::string_view name) {
#ifdef _WIN32
  char *value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, std::string(name).c_str()) != 0 || !value)
    return std::nullopt;
  std::string result(value);
  std::free(value);
  return result.empty() ? std::nullopt
                        : std::optional<std::string>(std::move(result));
#else
  const auto *value = std::getenv(std::string(name).c_str());
  return value && *value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string firstLine(std::string text) {
  const auto end = text.find_first_of("\r\n");
  if (end != std::string::npos)
    text.resize(end);
  return trim(std::move(text));
}

std::vector<std::string> splitPathList(std::string_view value, char separator) {
  std::vector<std::string> result;
  std::size_t begin = 0;
  while (begin <= value.size()) {
    const auto end = value.find(separator, begin);
    auto item = trim(std::string(
        value.substr(begin, end == std::string_view::npos ? value.size() - begin
                                                          : end - begin)));
    if (!item.empty() && std::ranges::find(result, item) == result.end())
      result.push_back(std::move(item));
    if (end == std::string_view::npos)
      break;
    begin = end + 1;
  }
  return result;
}

std::string canonicalPath(const std::filesystem::path &path) {
  std::error_code error;
  auto result = std::filesystem::weakly_canonical(path, error);
  if (error)
    result = std::filesystem::absolute(path, error);
  return (error ? path : result).lexically_normal().string();
}

void validateComponentPath(CFFIToolchainContract &contract,
                           std::string_view component,
                           const std::filesystem::path &path) {
  std::error_code error;
  const auto normalized = canonicalPath(path);
  if (std::filesystem::is_directory(path, error) && !error) {
    contract.validated_components.push_back(std::string(component) + "=" +
                                            normalized);
  } else {
    contract.missing_components.push_back(std::string(component) + "=" +
                                          normalized);
    contract.discovery_trace.push_back(
        "rejected-component=" + std::string(component) +
        ":missing:" + normalized);
  }
}

std::string canonicalLinuxTriple(std::string_view triple) {
  if (triple.starts_with("x86_64-") &&
      triple.find("linux") != std::string_view::npos)
    return "x86_64-unknown-linux-gnu";
  return std::string(triple);
}

std::optional<std::string> elfMachine(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  unsigned char header[20] = {};
  if (!input.read(reinterpret_cast<char *>(header), sizeof(header)) ||
      header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' ||
      header[3] != 'F' || header[5] != 1)
    return std::nullopt;
  const auto elf_class = header[4];
  const auto machine = static_cast<unsigned>(header[18]) |
                       (static_cast<unsigned>(header[19]) << 8);
  if (elf_class == 2 && machine == 62)
    return "x86_64";
  if (elf_class == 1 && machine == 3)
    return "i386";
  if (elf_class == 2 && machine == 183)
    return "aarch64";
  return "unknown";
}

std::optional<std::string>
findExecutable(std::string_view value,
               const std::optional<std::string> &path_override = std::nullopt) {
  if (value.empty())
    return std::nullopt;
  const std::filesystem::path input = pathForFileSystem(value);
  std::error_code error;
  if (input.has_parent_path()) {
    if (std::filesystem::is_regular_file(input, error) && !error)
      return canonicalPath(input);
    return std::nullopt;
  }
  const auto path = path_override ? path_override : environmentValue("PATH");
  if (!path)
    return std::nullopt;
#ifdef _WIN32
  constexpr char Separator = ';';
  std::vector<std::string> extensions = {""};
  if (!input.has_extension())
    extensions = {".exe", ".cmd", ".bat", ""};
#else
  constexpr char Separator = ':';
  const std::vector<std::string> extensions = {""};
#endif
  for (const auto &directory : splitPathList(*path, Separator))
    for (const auto &extension : extensions) {
      const auto candidate = pathForFileSystem(directory) /
                             pathForFileSystem(std::string(value) + extension);
      error.clear();
      if (std::filesystem::is_regular_file(candidate, error) && !error)
        return canonicalPath(candidate);
    }
  return std::nullopt;
}

void appendCanonical(std::ostringstream &out, std::string_view value) {
  out << value.size() << ':';
  out.write(value.data(), static_cast<std::streamsize>(value.size()));
  out << '\n';
}

void finishIdentity(CFFIToolchainContract &contract) {
  std::ostringstream sdk;
  appendCanonical(sdk, contract.sdk_name);
  appendCanonical(sdk, contract.sdk_version);
  appendCanonical(sdk, contract.sysroot);
  appendCanonical(sdk, contract.resource_dir);
  for (const auto &path : contract.system_include_paths)
    appendCanonical(sdk, canonicalPath(pathForFileSystem(path)));
  for (const auto &path : contract.system_library_paths)
    appendCanonical(sdk, canonicalPath(pathForFileSystem(path)));
  contract.sdk_fingerprint = sha256Hex(sdk.str());

  std::ostringstream identity;
  appendCanonical(identity, "chtholly.cffi.toolchain.v1");
  appendCanonical(identity, cffiCompilerFamilyName(contract.family));
  appendCanonical(identity, contract.target);
  appendCanonical(identity,
                  canonicalPath(pathForFileSystem(contract.compiler)));
  appendCanonical(identity, contract.compiler_version);
  appendCanonical(identity, contract.compiler_target_triple);
  appendCanonical(identity, contract.canonical_target_triple);
  appendCanonical(identity, contract.compiler_multiarch);
  appendCanonical(identity, contract.sysroot_mode);
  appendCanonical(identity, contract.runtime_architecture);
  appendCanonical(identity, contract.runtime_link_probe);
  for (const auto &probe : contract.runtime_file_probes)
    appendCanonical(identity, probe);
  appendCanonical(identity, contract.sdk_fingerprint);
  contract.fingerprint = sha256Hex(identity.str());
}

std::string requestFingerprint(const CFFIToolchainRequest &request) {
  std::ostringstream out;
  appendCanonical(out, "chtholly.cffi.toolchain.request.v1");
  appendCanonical(out, request.target);
  appendCanonical(out, request.compiler);
  appendCanonical(out, request.msvc_install.empty()
                           ? std::string_view{}
                           : std::string_view(canonicalPath(
                                 pathForFileSystem(request.msvc_install))));
  appendCanonical(out, request.sysroot.empty()
                           ? std::string_view{}
                           : std::string_view(canonicalPath(
                                 pathForFileSystem(request.sysroot))));
  constexpr const char *EnvironmentNames[] = {
      "PATH", "INCLUDE", "LIB", "LIBPATH", "VSINSTALLDIR",
      "VCINSTALLDIR", "VCTOOLSINSTALLDIR", "WINDOWSSDKDIR",
      "WINDOWSSDKVERSION", "UCRTVERSION", "CC"};
  for (const auto *name : EnvironmentNames) {
    appendCanonical(out, name);
    appendCanonical(out, environmentValue(name).value_or(std::string{}));
  }
  return sha256Hex(out.str());
}

struct MemoryCacheEntry {
  CFFIToolchainContract contract;
  std::uint64_t last_access = 0;
};
using ToolchainCache = std::map<std::string, MemoryCacheEntry, std::less<>>;

std::mutex &toolchainCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

ToolchainCache &toolchainCache() {
  static ToolchainCache cache;
  return cache;
}

std::uint64_t &cacheAccessClock() {
  static std::uint64_t clock = 0;
  return clock;
}

void metric(CFFIToolchainCacheMetrics *metrics,
            std::uint64_t CFFIToolchainCacheMetrics::*field,
            std::uint64_t amount = 1) {
  if (metrics)
    metrics->*field += amount;
}

void appendList(std::ostringstream &out, std::string_view name,
                const std::vector<std::string> &values) {
  appendCanonical(out, name);
  appendCanonical(out, std::to_string(values.size()));
  for (const auto &value : values)
    appendCanonical(out, value);
}

std::string escapeCacheValue(std::string_view value) {
  std::string result;
  for (const char character : value) {
    if (character == '%')
      result += "%25";
    else if (character == '\r')
      result += "%0D";
    else if (character == '\n')
      result += "%0A";
    else
      result.push_back(character);
  }
  return result;
}

std::optional<std::string> unescapeCacheValue(std::string_view value) {
  std::string result;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] != '%') {
      result.push_back(value[index]);
      continue;
    }
    if (index + 2 >= value.size())
      return std::nullopt;
    const auto code = value.substr(index + 1, 2);
    if (code == "25") result.push_back('%');
    else if (code == "0D") result.push_back('\r');
    else if (code == "0A") result.push_back('\n');
    else return std::nullopt;
    index += 2;
  }
  return result;
}

void appendContract(std::ostringstream &out,
                    const CFFIToolchainContract &contract) {
  appendCanonical(out, "CHTHOLLY-CFFI-TOOLCHAIN-V1");
  appendCanonical(out, std::to_string(static_cast<unsigned>(contract.family)));
  for (const auto &value : {contract.target, contract.compiler,
                            escapeCacheValue(contract.compiler_version),
                            contract.compiler_target_triple,
                            contract.canonical_target_triple,
                            contract.compiler_multiarch,
                            contract.compiler_multilibs, contract.sysroot_mode,
                            contract.sysroot, contract.resource_dir,
                            contract.sdk_name, contract.sdk_version,
                            contract.runtime_architecture,
                            contract.runtime_link_probe, contract.fingerprint,
                            contract.sdk_fingerprint})
    appendCanonical(out, value);
  appendCanonical(out, contract.target_match ? "1" : "0");
  appendList(out, "system-includes", contract.system_include_paths);
  appendList(out, "system-libraries", contract.system_library_paths);
  appendList(out, "runtime-includes", contract.runtime_include_paths);
  appendList(out, "runtime-libraries", contract.runtime_library_paths);
  appendList(out, "runtime-files", contract.runtime_file_probes);
  appendList(out, "header-probes", contract.standard_header_probes);
  appendList(out, "missing", contract.missing_components);
  appendList(out, "validated", contract.validated_components);
  appendCanonical(out, "environment-overrides");
  appendCanonical(out, std::to_string(contract.environment_overrides.size()));
  for (const auto &[name, value] : contract.environment_overrides) {
    appendCanonical(out, name);
    appendCanonical(out, value);
  }
}

bool writeDiskContract(const std::filesystem::path &path,
                       std::string_view key,
                       const CFFIToolchainContract &contract) {
  std::ostringstream content;
  appendCanonical(content, key);
  appendContract(content, contract);
  const auto temporary = path.string() + ".tmp";
  std::string error;
  if (!writeTextFile(temporary, content.str(), error))
    return false;
  std::error_code ec;
  std::filesystem::rename(std::filesystem::path(temporary), path, ec);
  if (ec) {
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(std::filesystem::path(temporary), path, ec);
  }
  if (ec)
    std::filesystem::remove(std::filesystem::path(temporary), ec);
  return !ec;
}

std::optional<CFFIToolchainContract>
readDiskContract(const std::filesystem::path &path, std::string_view key) {
  std::string error;
  const auto text = readTextFile(path.string(), error);
  if (!text || text->size() > 1024U * 1024U)
    return std::nullopt;
  // The disk record is intentionally strict and length-prefixed. Reuse the
  // canonical payload as the integrity check; malformed records are misses.
  std::vector<std::string> tokens;
  std::size_t offset = 0;
  while (offset < text->size()) {
    const auto end = text->find('\n', offset);
    const auto line = text->substr(offset, end == std::string::npos
                                             ? text->size() - offset
                                             : end - offset);
    const auto separator = line.find(':');
    if (separator == std::string_view::npos)
      return std::nullopt;
    std::size_t length = 0;
    const auto parsed = std::from_chars(line.data(), line.data() + separator,
                                        length);
    if (parsed.ec != std::errc{} || parsed.ptr != line.data() + separator ||
        line.size() - separator - 1 != length)
      return std::nullopt;
    tokens.emplace_back(line.substr(separator + 1));
    if (end == std::string::npos)
      break;
    offset = end + 1;
  }
  std::size_t cursor = 0;
  const auto next = [&]() -> std::optional<std::string_view> {
    return cursor < tokens.size() ? std::optional<std::string_view>(tokens[cursor++])
                                  : std::nullopt;
  };
  const auto first = next();
  if (!first || *first != key)
    return std::nullopt;
  const auto magic = next();
  if (!magic || *magic != "CHTHOLLY-CFFI-TOOLCHAIN-V1")
    return std::nullopt;
  CFFIToolchainContract contract;
  const auto family = next();
  unsigned family_value = 0;
  if (!family || std::from_chars(family->data(), family->data() + family->size(),
                                 family_value).ec != std::errc{} ||
      family_value >= static_cast<unsigned>(CFFICompilerFamily::Count))
    return std::nullopt;
  contract.family = static_cast<CFFICompilerFamily>(family_value);
  auto scalar = [&](std::string &value) {
    const auto item = next();
    if (!item) return false;
    value.assign(*item);
    return true;
  };
  if (!scalar(contract.target) || !scalar(contract.compiler) ||
      !scalar(contract.compiler_version) ||
      !scalar(contract.compiler_target_triple) ||
      !scalar(contract.canonical_target_triple) ||
      !scalar(contract.compiler_multiarch) ||
      !scalar(contract.compiler_multilibs) || !scalar(contract.sysroot_mode) ||
      !scalar(contract.sysroot) || !scalar(contract.resource_dir) ||
      !scalar(contract.sdk_name) || !scalar(contract.sdk_version) ||
      !scalar(contract.runtime_architecture) ||
      !scalar(contract.runtime_link_probe) || !scalar(contract.fingerprint) ||
      !scalar(contract.sdk_fingerprint))
    return std::nullopt;
  if (auto decoded = unescapeCacheValue(contract.compiler_version))
    contract.compiler_version = std::move(*decoded);
  else
    return std::nullopt;
  const auto target_match = next();
  if (!target_match || (*target_match != "0" && *target_match != "1"))
    return std::nullopt;
  contract.target_match = *target_match == "1";
  const auto read_list = [&](std::string_view expected,
                             std::vector<std::string> &values) {
    const auto name = next();
    const auto count_text = next();
    if (!name || !count_text || *name != expected)
      return false;
    std::size_t count = 0;
    const auto parsed = std::from_chars(count_text->data(),
                                        count_text->data() + count_text->size(),
                                        count);
    if (parsed.ec != std::errc{} || parsed.ptr != count_text->data() + count_text->size() ||
        count > 4096)
      return false;
    values.clear();
    values.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      const auto item = next();
      if (!item) return false;
      values.emplace_back(*item);
    }
    return true;
  };
  if (!read_list("system-includes", contract.system_include_paths) ||
      !read_list("system-libraries", contract.system_library_paths) ||
      !read_list("runtime-includes", contract.runtime_include_paths) ||
      !read_list("runtime-libraries", contract.runtime_library_paths) ||
      !read_list("runtime-files", contract.runtime_file_probes) ||
      !read_list("header-probes", contract.standard_header_probes) ||
      !read_list("missing", contract.missing_components) ||
      !read_list("validated", contract.validated_components))
    return std::nullopt;
  const auto env_name = next();
  const auto env_count = next();
  if (!env_name || !env_count || *env_name != "environment-overrides")
    return std::nullopt;
  std::size_t count = 0;
  const auto parsed = std::from_chars(env_count->data(),
                                      env_count->data() + env_count->size(),
                                      count);
  if (parsed.ec != std::errc{} || parsed.ptr != env_count->data() + env_count->size() ||
      count > 4096)
    return std::nullopt;
  for (std::size_t index = 0; index < count; ++index) {
    const auto name = next();
    const auto value = next();
    if (!name || !value) return std::nullopt;
    contract.environment_overrides.emplace_back(*name, *value);
  }
  if (cursor != tokens.size())
    return std::nullopt;
  const auto stored_fingerprint = contract.fingerprint;
  const auto stored_sdk_fingerprint = contract.sdk_fingerprint;
  finishIdentity(contract);
  if (contract.fingerprint != stored_fingerprint ||
      contract.sdk_fingerprint != stored_sdk_fingerprint)
    return std::nullopt;
  return contract;
}

ProcessRunOptions processOptions(const CFFIToolchainContract &contract) {
  ProcessRunOptions options;
  options.timeout_milliseconds = 30000;
  options.max_stdout_bytes = options.max_stderr_bytes = 1024U * 1024U;
  options.environment_overrides = contract.environment_overrides;
  return options;
}

#ifdef _WIN32

std::map<std::string, std::string, std::less<>>
parseEnvironment(std::string_view text) {
  std::map<std::string, std::string, std::less<>> result;
  std::istringstream input{std::string(text)};
  for (std::string line; std::getline(input, line);) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const auto split = line.find('=');
    if (split == std::string::npos || split == 0)
      continue;
    auto name = line.substr(0, split);
    std::ranges::transform(name, name.begin(), [](unsigned char ch) {
      return static_cast<char>(std::toupper(ch));
    });
    result[name] = line.substr(split + 1);
  }
  return result;
}

std::optional<std::filesystem::path>
vcvarsPath(const CFFIToolchainRequest &request, std::vector<std::string> &trace,
           std::string &error) {
  if (!request.msvc_install.empty()) {
    auto configured = pathForFileSystem(request.msvc_install);
    auto extension = configured.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    if (extension != ".bat")
      configured /= "VC/Auxiliary/Build/vcvars64.bat";
    trace.push_back("configured-msvc-install=" + configured.string());
    return configured;
  }
  if (request.compiler != "auto") {
    auto compiler = pathForFileSystem(request.compiler);
    if (compiler.is_absolute())
      for (auto parent = compiler.parent_path(); !parent.empty();
           parent = parent.parent_path()) {
        if (parent.filename() == "VC") {
          const auto candidate = parent / "Auxiliary/Build/vcvars64.bat";
          trace.push_back("compiler-relative-vcvars=" + candidate.string());
          return candidate;
        }
        if (parent == parent.root_path())
          break;
      }
  }
  if (const auto configured = environmentValue("ChthollyMSVCPath")) {
    trace.push_back("environment-ChthollyMSVCPath=" + *configured);
    return pathForFileSystem(*configured);
  }
  if (const auto install = environmentValue("VSINSTALLDIR")) {
    const auto candidate =
        pathForFileSystem(*install) / "VC/Auxiliary/Build/vcvars64.bat";
    trace.push_back("environment-VSINSTALLDIR=" + candidate.string());
    return candidate;
  }
  std::vector<std::filesystem::path> vswhere_candidates;
  if (const auto value = environmentValue("ProgramFiles(x86)"))
    vswhere_candidates.push_back(
        pathForFileSystem(*value) /
        "Microsoft Visual Studio/Installer/vswhere.exe");
  if (const auto value = environmentValue("ProgramFiles"))
    vswhere_candidates.push_back(
        pathForFileSystem(*value) /
        "Microsoft Visual Studio/Installer/vswhere.exe");
  for (const auto &vswhere : vswhere_candidates) {
    std::error_code file_error;
    if (!std::filesystem::is_regular_file(vswhere, file_error) || file_error)
      continue;
    trace.push_back("vswhere=" + vswhere.string());
    ProcessRunOptions options;
    options.timeout_milliseconds = 10000;
    options.max_stdout_bytes = options.max_stderr_bytes = 64U * 1024U;
    auto result =
        runProcess(vswhere.string(),
                   {"-latest", "-products", "*", "-requires",
                    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                    "-property", "installationPath"},
                   options, error);
    if (!result || result->exit_code != 0)
      continue;
    const auto install = trim(result->stdout_text);
    if (!install.empty())
      return pathForFileSystem(install) / "VC/Auxiliary/Build/vcvars64.bat";
  }
  error = "MSVC environment is incomplete and no vcvars64.bat was discovered";
  return std::nullopt;
}

bool resolveMSVC(const CFFIToolchainRequest &request,
                 CFFIToolchainContract &contract, std::string &error) {
  std::map<std::string, std::string, std::less<>> environment;
  constexpr const char *EnvironmentNames[] = {"PATH",
                                              "INCLUDE",
                                              "LIB",
                                              "LIBPATH",
                                              "WINDOWSSDKDIR",
                                              "WINDOWSSDKVERSION",
                                              "VCTOOLSINSTALLDIR",
                                              "UNIVERSALCRTSDKDIR",
                                              "UCRTVERSION",
                                              "VCINSTALLDIR",
                                              "VSINSTALLDIR",
                                              "VISUALSTUDIOVERSION"};
  for (const auto *name : EnvironmentNames)
    if (const auto value = environmentValue(name))
      environment[name] = *value;
  bool inherited = environment.contains("INCLUDE") &&
                   environment.contains("LIB") &&
                   environment.contains("PATH") && request.msvc_install.empty();
  if (inherited && request.compiler != "auto" &&
      pathForFileSystem(request.compiler).is_absolute()) {
    auto compiler = canonicalPath(pathForFileSystem(request.compiler));
    auto tools = environment.contains("VCTOOLSINSTALLDIR")
                     ? canonicalPath(pathForFileSystem(
                           environment.at("VCTOOLSINSTALLDIR")))
                     : std::string{};
    std::ranges::transform(compiler, compiler.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    std::ranges::transform(tools, tools.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    inherited = !tools.empty() && compiler.starts_with(tools);
  }
  if (inherited) {
    contract.discovery_trace.push_back("inherited-msvc-environment");
  } else {
    auto script = vcvarsPath(request, contract.discovery_trace, error);
    if (!script)
      return false;
    std::error_code file_error;
    if (!std::filesystem::is_regular_file(*script, file_error) || file_error) {
      error = "MSVC environment script is missing: '" + script->string() + "'";
      return false;
    }
    ProcessRunOptions options;
    options.timeout_milliseconds = 30000;
    options.max_stdout_bytes = 4U * 1024U * 1024U;
    options.max_stderr_bytes = 1024U * 1024U;
    static std::atomic<std::uint64_t> sequence = 0;
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto wrapper =
        std::filesystem::temp_directory_path() /
        ("chtholly-cffi-vcvars-" + std::to_string(stamp) + "-" +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) +
         ".cmd");
    if (!writeTextFile(wrapper.string(),
                       "@call \"" + script->string() +
                           "\" >nul\r\n@if errorlevel 1 exit /b "
                           "%errorlevel%\r\n@set\r\n",
                       error))
      return false;
    auto result =
        runProcess("cmd.exe", {"/d", "/c", wrapper.string()}, options, error);
    std::error_code cleanup_error;
    removeFile(wrapper.string(), cleanup_error);
    if (!result || result->exit_code != 0) {
      if (result)
        error = summarizeCommandFailure("cmd.exe", *result);
      return false;
    }
    environment = parseEnvironment(result->stdout_text);
    contract.discovery_trace.push_back("imported-vcvars64");
    for (const auto *name : EnvironmentNames)
      if (environment.contains(name))
        contract.environment_overrides.emplace_back(name, environment.at(name));
  }
  const auto path_environment =
      environment.contains("PATH")
          ? std::optional<std::string>(environment.at("PATH"))
          : std::nullopt;
  auto compiler = request.compiler == "auto"
                      ? findExecutable("cl.exe", path_environment)
                      : findExecutable(request.compiler, path_environment);
  if (!compiler) {
    contract.discovery_trace.push_back("rejected-compiler=cl.exe:not-found");
    error = "MSVC compiler was not found in the resolved environment";
    return false;
  }
  contract.discovery_trace.push_back("selected-compiler=" + *compiler);
  contract.family = CFFICompilerFamily::MSVC;
  contract.compiler = *compiler;
  contract.system_include_paths = splitPathList(environment["INCLUDE"], ';');
  contract.system_library_paths = splitPathList(environment["LIB"], ';');
  std::set<std::string, std::less<>> sdk_candidates;
  const auto add_sdk_candidate = [&](std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    const auto marker = value.find("windows kits\\10");
    if (marker != std::string::npos)
      sdk_candidates.insert(
          value.substr(0, marker + std::string("windows kits\\10").size()));
  };
  if (environment.contains("WINDOWSSDKDIR"))
    add_sdk_candidate(canonicalPath(
        pathForFileSystem(environment.at("WINDOWSSDKDIR"))));
  for (const auto &include_path : contract.system_include_paths)
    add_sdk_candidate(canonicalPath(pathForFileSystem(include_path)));
  for (const auto &library_path : contract.system_library_paths)
    add_sdk_candidate(canonicalPath(pathForFileSystem(library_path)));
  if (sdk_candidates.size() > 1) {
    std::string detail;
    for (const auto &candidate : sdk_candidates) {
      if (!detail.empty())
        detail += ',';
      detail += candidate;
    }
    contract.discovery_trace.push_back("sdk-candidate-conflict=" + detail);
    error = "MSVC Windows SDK candidates conflict: " + detail;
    return false;
  }
  for (const auto &candidate : sdk_candidates)
    contract.discovery_trace.push_back("sdk-candidate=" + candidate);
  for (const auto &include_path : contract.system_include_paths)
    validateComponentPath(contract, "msvc.include",
                          pathForFileSystem(include_path));
  for (const auto &library_path : contract.system_library_paths)
    validateComponentPath(contract, "msvc.lib",
                          pathForFileSystem(library_path));
  if (environment.contains("VCTOOLSINSTALLDIR"))
    validateComponentPath(
        contract, "vc.tools",
        pathForFileSystem(environment.at("VCTOOLSINSTALLDIR")));
  if (environment.contains("WINDOWSSDKDIR"))
    validateComponentPath(contract, "windows.sdk",
                          pathForFileSystem(environment.at("WINDOWSSDKDIR")));
  if (environment.contains("UNIVERSALCRTSDKDIR"))
    validateComponentPath(
        contract, "ucrt.sdk",
        pathForFileSystem(environment.at("UNIVERSALCRTSDKDIR")));
  contract.sdk_name = "msvc-windows-sdk";
  contract.sdk_version = environment["VCTOOLSINSTALLDIR"] + "|" +
                         environment["WINDOWSSDKVERSION"] + "|" +
                         environment["UCRTVERSION"];
  auto version =
      runProcess(contract.compiler, {"/Bv"}, processOptions(contract), error);
  if (!version ||
      (version->stdout_text.empty() && version->stderr_text.empty())) {
    if (version)
      error = "MSVC version query produced no output";
    return false;
  }
  contract.compiler_version = version->stdout_text + version->stderr_text;
  return true;
}

#else

bool resolveUnix(const CFFIToolchainRequest &request,
                 CFFIToolchainContract &contract, std::string &error) {
  std::vector<std::string> candidates;
  if (request.compiler != "auto")
    candidates.push_back(request.compiler);
  else {
    if (const auto cc = environmentValue("CC"))
      candidates.push_back(*cc);
    candidates.insert(candidates.end(), {"clang", "gcc", "cc"});
  }
  for (const auto &candidate : candidates) {
    contract.discovery_trace.push_back("compiler-candidate=" + candidate);
    if (auto found = findExecutable(candidate)) {
      contract.compiler = *found;
      contract.discovery_trace.push_back("selected-compiler=" + *found);
      break;
    }
    contract.discovery_trace.push_back("rejected-compiler=" + candidate +
                                       ":not-found");
  }
  if (contract.compiler.empty()) {
    error = "no Tier-1 C compiler was found through config, CC, or PATH";
    return false;
  }
  ProcessRunOptions options;
  options.timeout_milliseconds = 30000;
  options.max_stdout_bytes = options.max_stderr_bytes = 1024U * 1024U;
  auto version = runProcess(contract.compiler, {"--version"}, options, error);
  if (!version || version->exit_code != 0) {
    if (version)
      error = summarizeCommandFailure(contract.compiler, *version);
    return false;
  }
  contract.compiler_version = version->stdout_text + version->stderr_text;
  auto machine =
      runProcess(contract.compiler, {"-dumpmachine"}, options, error);
  if (machine && machine->exit_code == 0)
    contract.compiler_target_triple = trim(machine->stdout_text);
  if (contract.compiler_target_triple.empty())
    contract.compiler_target_triple = firstLine(contract.compiler_version);
  contract.discovery_trace.push_back("compiler-target=" +
                                     contract.compiler_target_triple);
  contract.canonical_target_triple =
      canonicalLinuxTriple(contract.compiler_target_triple);
  contract.target_match = contract.canonical_target_triple == contract.target;
  auto multiarch =
      runProcess(contract.compiler, {"-print-multiarch"}, options, error);
  if (multiarch && multiarch->exit_code == 0)
    contract.compiler_multiarch = trim(multiarch->stdout_text);
  auto multilib =
      runProcess(contract.compiler, {"-print-multi-lib"}, options, error);
  if (multilib && multilib->exit_code == 0)
    contract.compiler_multilibs = trim(multilib->stdout_text);
  const auto first = firstLine(contract.compiler_version);
  contract.family = first.find("clang") != std::string::npos
                        ? CFFICompilerFamily::Clang
                        : CFFICompilerFamily::GCC;
  contract.sysroot = request.sysroot;
  contract.sysroot_mode = contract.sysroot.empty() ? "host-root" : "explicit";
  if (!contract.sysroot.empty())
    validateComponentPath(contract, "linux.sysroot",
                          pathForFileSystem(contract.sysroot));
  if (contract.sysroot.empty()) {
    auto sysroot =
        runProcess(contract.compiler, {"--print-sysroot"}, options, error);
    if (sysroot && sysroot->exit_code == 0)
      contract.sysroot = trim(sysroot->stdout_text);
    if (contract.sysroot.empty() || contract.sysroot == "/")
      contract.sysroot_mode = "host-root";
    error.clear();
  }
  if (contract.family == CFFICompilerFamily::Clang) {
    auto resource =
        runProcess(contract.compiler, {"-print-resource-dir"}, options, error);
    if (resource && resource->exit_code == 0)
      contract.resource_dir = trim(resource->stdout_text);
    if (!contract.resource_dir.empty())
      validateComponentPath(contract, "clang.resource-dir",
                            pathForFileSystem(contract.resource_dir));
    error.clear();
  }
  const auto compilerArguments = [&](std::vector<std::string> arguments) {
    if (!contract.sysroot.empty() && contract.sysroot != "/")
      arguments.insert(arguments.begin(), "--sysroot=" + contract.sysroot);
    return arguments;
  };
  options.stdin_text = "";
  auto includes = runProcess(
      contract.compiler, compilerArguments({"-E", "-x", "c", "-v", "-"}),
      options, error);
  if (!includes || includes->exit_code != 0) {
    if (includes)
      error = summarizeCommandFailure(contract.compiler, *includes);
    return false;
  }
  bool in_search = false;
  std::istringstream include_output(includes->stderr_text);
  for (std::string line; std::getline(include_output, line);) {
    const auto value = trim(line);
    if (value.find("search starts here:") != std::string::npos) {
      in_search = true;
      continue;
    }
    if (value == "End of search list.") {
      in_search = false;
      break;
    }
    if (in_search && !value.empty() && !value.starts_with('('))
      contract.system_include_paths.push_back(value);
  }
  auto searches = runProcess(contract.compiler,
                             compilerArguments({"-print-search-dirs"}),
                             options, error);
  if (searches && searches->exit_code == 0) {
    std::istringstream lines(searches->stdout_text);
    for (std::string line; std::getline(lines, line);)
      if (line.starts_with("libraries: ="))
        contract.system_library_paths =
            splitPathList(line.substr(std::string("libraries: =").size()), ':');
  }
  error.clear();
  contract.runtime_include_paths = contract.system_include_paths;
  contract.runtime_library_paths = contract.system_library_paths;
  const auto requested = contract.target;
  if (!contract.target_match) {
    contract.missing_components.push_back("linux.target-triple=" +
                                          contract.compiler_target_triple);
    contract.discovery_trace.push_back(
        "rejected-component=linux.target-triple:mismatch:" + requested + ":" +
        contract.compiler_target_triple);
    error =
        "compiler target triple does not match requested native Linux target";
    return false;
  }
  if (!contract.sysroot.empty() && contract.sysroot != "/") {
    const auto root = pathForFileSystem(contract.sysroot);
    for (const auto &required : {"usr/include", "usr/lib"})
      validateComponentPath(contract, std::string("linux.sysroot.") + required,
                            root / required);
  }
  for (const auto &path : contract.system_include_paths)
    validateComponentPath(contract, "linux.include", pathForFileSystem(path));
  for (const auto &path : contract.system_library_paths)
    validateComponentPath(contract, "linux.library", pathForFileSystem(path));
  for (const auto header :
       {"stddef.h", "stdint.h", "stdio.h", "stdlib.h", "errno.h"}) {
    ProcessRunOptions probe_options = options;
    probe_options.stdin_text = "#include <" + std::string(header) + ">\n";
    auto probe = runProcess(
        contract.compiler, compilerArguments({"-E", "-x", "c", "-"}),
        probe_options, error);
    const bool ok = probe && probe->exit_code == 0;
    contract.standard_header_probes.push_back(std::string(header) +
                                              (ok ? "=ok" : "=failed"));
    if (!ok) {
      contract.missing_components.push_back("linux.header=" +
                                            std::string(header));
      contract.discovery_trace.push_back("rejected-component=linux.header:" +
                                         std::string(header) + ":probe-failed");
    }
    error.clear();
  }
  for (const auto file :
       {"crt1.o", "crti.o", "crtn.o", "libc.so", "libgcc_s.so.1"}) {
    auto located = runProcess(
        contract.compiler,
        compilerArguments({"-print-file-name=" + std::string(file)}), options,
        error);
    const auto path = located && located->exit_code == 0
                          ? trim(located->stdout_text)
                          : std::string{};
    auto resolved_path = pathForFileSystem(path);
    bool under_sysroot = true;
    if (contract.sysroot_mode == "explicit") {
      const auto root = canonicalPath(pathForFileSystem(contract.sysroot));
      const auto candidate = canonicalPath(resolved_path);
      under_sysroot = candidate == root ||
                      candidate.starts_with(root +
                                            std::filesystem::path::preferred_separator);
      if (!under_sysroot && resolved_path.is_absolute()) {
        const auto remapped = pathForFileSystem(contract.sysroot) /
                              resolved_path.relative_path();
        if (std::filesystem::is_regular_file(remapped)) {
          resolved_path = remapped;
          under_sysroot = true;
        }
      }
    }
    const auto reported_path = resolved_path.string();
    const bool ok = !path.empty() && path != file && under_sysroot &&
                    std::filesystem::is_regular_file(resolved_path);
    std::string digest;
    std::string architecture;
    bool architecture_ok = true;
    if (ok) {
      if (const auto value = sha256File(resolved_path.string()))
        digest = *value;
      if (std::string(file).starts_with("crt")) {
        const auto machine = elfMachine(resolved_path);
        architecture = machine.value_or("invalid");
        architecture_ok = machine && *machine == "x86_64";
      }
    }
    const bool verified = ok && architecture_ok;
    contract.runtime_file_probes.push_back(
        std::string(file) + (verified ? "=ok:" : "=missing:") +
        reported_path +
        (digest.empty() ? "" : ":sha256=" + digest) +
        (architecture.empty() ? "" : ":elf=" + architecture));
    if (verified)
      contract.validated_components.push_back("linux.runtime=" +
                                              reported_path);
    else {
      contract.missing_components.push_back(
          "linux.runtime=" + std::string(file) + ":" + reported_path);
      if (ok && !architecture_ok)
        contract.discovery_trace.push_back(
            "rejected-component=linux.runtime:" + std::string(file) +
            ":architecture-mismatch:expected=x86_64:actual=" + architecture);
    }
    error.clear();
  }
  contract.runtime_architecture = contract.compiler_target_triple;
  std::ostringstream runtime_identity;
  runtime_identity << contract.canonical_target_triple << '\n'
                   << contract.sysroot_mode << '\n';
  for (const auto &probe : contract.runtime_file_probes)
    runtime_identity << probe << '\n';
  contract.runtime_link_probe =
      "files-verified:" + sha256Hex(runtime_identity.str());
  contract.sdk_name = "linux-system-c";
  contract.sdk_version = contract.sysroot.empty() ? "/" : contract.sysroot;
  if (!contract.sysroot.empty()) {
    validateComponentPath(contract, "linux.sysroot.include",
                          pathForFileSystem(contract.sysroot) / "usr/include");
    validateComponentPath(contract, "linux.sysroot.lib",
                          pathForFileSystem(contract.sysroot) / "usr/lib");
  }
  return true;
}

#endif

} // namespace

std::string_view cffiCompilerFamilyName(CFFICompilerFamily family) {
  switch (family) {
  case CFFICompilerFamily::MSVC:
    return "msvc";
  case CFFICompilerFamily::Clang:
    return "clang";
  case CFFICompilerFamily::GCC:
    return "gcc";
  case CFFICompilerFamily::Count:
    return "invalid";
  }
  return "invalid";
}

std::string nativeTier1CFFITarget() {
#if defined(_WIN32) && defined(_M_X64)
  return "x86_64-pc-windows-msvc";
#elif defined(__linux__) && defined(__x86_64__)
  return "x86_64-unknown-linux-gnu";
#else
  return "unsupported-host";
#endif
}

bool loadCFFIToolchainRequest(const std::string &config_path,
                              CFFIToolchainRequest &request,
                              std::string &error) {
  request = {};
  auto text = readTextFile(config_path, error);
  if (!text)
    return false;
  auto assignments = manifest_toml::parseAssignments(
      *text, {"toolchain", "clang", "probe", "[roots]", "[type_mappings]"},
      "CFFI config", error);
  if (!assignments)
    return false;
  std::uint64_t version = 0;
  std::set<std::string> seen;
  for (const auto &assignment : *assignments) {
    const auto key = assignment.fullKey();
    if ((key == "version" || key == "target" ||
         key.starts_with("toolchain.")) &&
        !seen.insert(key).second) {
      error = "duplicate CFFI toolchain config key: " + key;
      return false;
    }
    if (key == "version") {
      if (!manifest_toml::parseUnsigned(assignment.value, version) ||
          version != 3) {
        error = "CFFI config requires version = 3";
        return false;
      }
    } else if (key == "target") {
      if (!manifest_toml::parseString(assignment.value, request.target))
        return false;
    } else if (key == "toolchain.compiler") {
      if (!manifest_toml::parseString(assignment.value, request.compiler))
        return false;
    } else if (key == "toolchain.msvc_install") {
      if (!manifest_toml::parseString(assignment.value, request.msvc_install))
        return false;
    } else if (key == "toolchain.sysroot") {
      if (!manifest_toml::parseString(assignment.value, request.sysroot))
        return false;
    }
  }
  if (version != 3 || request.target.empty()) {
    error = "CFFI config v3 has no target toolchain contract";
    return false;
  }
  const auto root = pathForFileSystem(config_path).parent_path();
  const auto resolve_relative = [&](std::string &value) {
    if (value.empty() || value == "auto")
      return;
    const auto path = pathForFileSystem(value);
    if (path.has_parent_path() && !path.is_absolute())
      value = canonicalPath(root / path);
  };
  resolve_relative(request.compiler);
  resolve_relative(request.msvc_install);
  resolve_relative(request.sysroot);
  return true;
}

bool resolveCFFIToolchainUncached(const CFFIToolchainRequest &request,
                                  CFFIToolchainContract &contract,
                                  std::string &error) {
  contract = {};
  error.clear();
  contract.target =
      request.target.empty() ? nativeTier1CFFITarget() : request.target;
  if (contract.target != nativeTier1CFFITarget()) {
    error = "CFFI toolchain discovery requires the native Tier-1 target";
    return false;
  }
#ifdef _WIN32
  if (!request.sysroot.empty()) {
    error = "toolchain.sysroot is unavailable for the MSVC Tier-1 target";
    return false;
  }
  const bool resolved = resolveMSVC(request, contract, error);
#else
  if (!request.msvc_install.empty()) {
    error = "toolchain.msvc_install is unavailable on Linux";
    return false;
  }
  const bool resolved = resolveUnix(request, contract, error);
#endif
  if (!resolved) {
    if (!contract.discovery_trace.empty()) {
      error += "; discovery:";
      for (const auto &entry : contract.discovery_trace)
        error += " [" + entry + "]";
    }
    return false;
  }
  std::string critical_missing;
  for (const auto &missing : contract.missing_components) {
    if (missing.starts_with("linux.runtime=") ||
        missing.starts_with("linux.header=") ||
        missing.starts_with("linux.sysroot") ||
        missing.starts_with("linux.target-triple=")) {
      if (!critical_missing.empty())
        critical_missing += ", ";
      critical_missing += missing;
    }
  }
  if (!critical_missing.empty()) {
    error = "resolved CFFI toolchain is incomplete: " + critical_missing;
    if (!contract.discovery_trace.empty()) {
      error += "; discovery:";
      for (const auto &entry : contract.discovery_trace)
        error += " [" + entry + "]";
    }
    return false;
  }
  if (contract.system_include_paths.empty()) {
    error = "resolved CFFI toolchain has no system include paths";
    return false;
  }
  finishIdentity(contract);
  return true;
}

std::filesystem::path cachePathFor(std::string_view directory,
                                   std::string_view key) {
  return pathForFileSystem(std::string(directory) + "/" + std::string(key) +
                           ".toolchain");
}

std::filesystem::path cacheMetadataPath(const std::filesystem::path &root,
                                        std::string_view key) {
  return root / (std::string(key) + ".meta");
}

bool writeCacheMetadata(const std::filesystem::path &root, std::string_view key,
                        std::uint64_t access, std::uint64_t bytes) {
  std::ostringstream out;
  out << "version=1\naccess=" << access << "\nbytes=" << bytes << "\n";
  std::string error;
  const auto temporary = cacheMetadataPath(root, key).string() + ".tmp";
  if (!writeTextFile(temporary, out.str(), error))
    return false;
  std::error_code ec;
  std::filesystem::rename(temporary, cacheMetadataPath(root, key), ec);
  if (ec) {
    std::filesystem::remove(cacheMetadataPath(root, key), ec);
    ec.clear();
    std::filesystem::rename(temporary, cacheMetadataPath(root, key), ec);
  }
  if (ec)
    std::filesystem::remove(temporary, ec);
  return !ec;
}

std::optional<std::pair<std::uint64_t, std::uint64_t>>
readCacheMetadata(const std::filesystem::path &root, std::string_view key) {
  std::string error;
  const auto text = readTextFile(cacheMetadataPath(root, key).string(), error);
  if (!text)
    return std::nullopt;
  std::uint64_t access = 0, bytes = 0;
  std::istringstream input(*text);
  std::string line;
  while (std::getline(input, line)) {
    if (line.starts_with("access="))
      std::from_chars(line.data() + 7, line.data() + line.size(), access);
    else if (line.starts_with("bytes="))
      std::from_chars(line.data() + 6, line.data() + line.size(), bytes);
  }
  if (!access)
    return std::nullopt;
  return std::pair{access, bytes};
}

std::uint64_t fileAgeSeconds(const std::filesystem::path &path) {
  std::error_code ec;
  const auto stamp = std::filesystem::last_write_time(path, ec);
  if (ec)
    return 0;
  const auto now = std::chrono::file_clock::now();
  const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - stamp);
  return age.count() > 0 ? static_cast<std::uint64_t>(age.count()) : 0;
}

bool pruneDiskCache(const CFFIToolchainCacheOptions &options,
                    const std::filesystem::path &root,
                    CFFIToolchainCacheMetrics *metrics, std::string &error) {
  error.clear();
  std::error_code ec;
  if (!std::filesystem::exists(root, ec))
    return true;
  struct Entry { std::filesystem::path path; std::string key; std::uint64_t access; };
  std::vector<Entry> entries;
  for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; ++it) {
    if (!it->is_regular_file(ec) || ec || it->path().extension() != ".toolchain")
      continue;
    const auto key = it->path().stem().string();
    auto metadata = readCacheMetadata(root, key);
    const auto access = metadata ? metadata->first :
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()) - fileAgeSeconds(it->path());
    const auto age = metadata ?
      (static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()) > metadata->first
         ? static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch()).count()) - metadata->first : 0)
      : fileAgeSeconds(it->path());
    if (options.max_age.count() >= 0 && age > static_cast<std::uint64_t>(options.max_age.count())) {
      std::filesystem::remove(it->path(), ec);
      std::filesystem::remove(cacheMetadataPath(root, key), ec);
      metric(metrics, &CFFIToolchainCacheMetrics::expired_entries);
      continue;
    }
    entries.push_back({it->path(), key, access});
  }
  std::ranges::sort(entries, [](const Entry &a, const Entry &b) { return a.access < b.access; });
  while (entries.size() > std::max<std::size_t>(1, options.max_entries)) {
    const auto entry = entries.front();
    entries.erase(entries.begin());
    std::filesystem::remove(entry.path, ec);
    std::filesystem::remove(cacheMetadataPath(root, entry.key), ec);
    metric(metrics, &CFFIToolchainCacheMetrics::evictions);
  }
  return true;
}

bool writeCachedContract(const std::filesystem::path &root,
                         std::string_view key,
                         const CFFIToolchainContract &contract,
                         CFFIToolchainCacheMetrics *metrics) {
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  if (ec)
    return false;
  const auto lock = root / (std::string(key) + ".lock");
  if (!std::filesystem::create_directory(lock, ec))
    return false;
  const auto path = root / (std::string(key) + ".toolchain");
  const bool result = writeDiskContract(path, key, contract);
  if (result) {
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    (void)writeCacheMetadata(root, key, now, size_error ? 0 : size);
    metric(metrics, &CFFIToolchainCacheMetrics::bytes_written,
           size_error ? 0 : size);
  }
  std::filesystem::remove_all(lock, ec);
  return result;
}

bool resolveCFFIToolchain(const CFFIToolchainRequest &request,
                          CFFIToolchainContract &contract, std::string &error,
                          const CFFIToolchainCacheOptions &cache_options) {
  const auto key = requestFingerprint(request);
  if (cache_options.enable_memory) {
    std::lock_guard lock(toolchainCacheMutex());
    if (const auto found = toolchainCache().find(key);
        found != toolchainCache().end()) {
      contract = found->second.contract;
      found->second.last_access = ++cacheAccessClock();
      metric(cache_options.metrics, &CFFIToolchainCacheMetrics::memory_hits);
      metric(cache_options.metrics, &CFFIToolchainCacheMetrics::discovery_avoided);
      contract.discovery_trace.push_back("cache-hit-memory");
      return true;
    }
  }
  bool invalid_disk_cache = false;
  if (cache_options.enable_disk && !cache_options.directory.empty()) {
    const auto root = pathForFileSystem(cache_options.directory);
    std::string prune_error;
    (void)pruneDiskCache(cache_options, root, cache_options.metrics, prune_error);
    const auto path = cachePathFor(cache_options.directory, key);
    if (auto cached = readDiskContract(path, key)) {
      contract = std::move(*cached);
      std::error_code size_error;
      const auto size = std::filesystem::file_size(path, size_error);
      metric(cache_options.metrics, &CFFIToolchainCacheMetrics::disk_hits);
      metric(cache_options.metrics, &CFFIToolchainCacheMetrics::discovery_avoided);
      metric(cache_options.metrics, &CFFIToolchainCacheMetrics::bytes_read,
             size_error ? 0 : size);
      const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
      (void)writeCacheMetadata(path.parent_path(), key, now,
                               size_error ? 0 : size);
      contract.discovery_trace.push_back("cache-hit-disk");
      if (cache_options.enable_memory) {
        std::lock_guard lock(toolchainCacheMutex());
        toolchainCache()[key] = MemoryCacheEntry{contract, ++cacheAccessClock()};
      }
      return true;
    }
    if (std::filesystem::exists(path)) {
      metric(cache_options.metrics, &CFFIToolchainCacheMetrics::invalid_entries);
      invalid_disk_cache = true;
    }
  }
  metric(cache_options.metrics, &CFFIToolchainCacheMetrics::misses);
  if (!resolveCFFIToolchainUncached(request, contract, error))
    return false;
  if (invalid_disk_cache)
    contract.discovery_trace.push_back("inherited-msvc-cache-invalid-fallback");
  if (cache_options.enable_memory) {
    std::lock_guard lock(toolchainCacheMutex());
    toolchainCache()[key] = MemoryCacheEntry{contract, ++cacheAccessClock()};
    while (toolchainCache().size() > std::max<std::size_t>(1, cache_options.max_entries)) {
      auto victim = std::ranges::min_element(toolchainCache(),
        [](const auto &a, const auto &b) { return a.second.last_access < b.second.last_access; });
      if (victim == toolchainCache().end()) break;
      toolchainCache().erase(victim);
      metric(cache_options.metrics, &CFFIToolchainCacheMetrics::evictions);
    }
  }
  if (cache_options.enable_disk && !cache_options.directory.empty()) {
    const auto root = pathForFileSystem(cache_options.directory);
    (void)writeCachedContract(root, key, contract, cache_options.metrics);
    std::string prune_error;
    (void)pruneDiskCache(cache_options, root, cache_options.metrics, prune_error);
  }
  return true;
}

bool pruneCFFIToolchainCache(const CFFIToolchainCacheOptions &options,
                             std::string &error) {
  error.clear();
  if (!options.enable_disk || options.directory.empty())
    return true;
  return pruneDiskCache(options, pathForFileSystem(options.directory),
                        options.metrics, error);
}

bool resolveCFFIToolchain(const CFFIToolchainRequest &request,
                          CFFIToolchainContract &contract, std::string &error) {
  return resolveCFFIToolchain(request, contract, error, {});
}

std::string cffiToolchainRequestFingerprint(
    const CFFIToolchainRequest &request) {
  return requestFingerprint(request);
}

bool probeCFFIToolchain(const CFFIToolchainContract &contract,
                        std::string &description, std::string &error) {
  static std::atomic<std::uint64_t> sequence = 0;
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = std::filesystem::temp_directory_path() /
                    ("chtholly-cffi-toolchain-" + std::to_string(stamp) + "-" +
                     std::to_string(sequence.fetch_add(1)));
  std::error_code file_error;
  std::filesystem::create_directories(root, file_error);
  if (file_error) {
    error = "failed to create CFFI toolchain probe directory";
    return false;
  }
  const auto source = (root / "probe.c").string();
#ifdef _WIN32
  const auto executable = (root / "probe.exe").string();
#else
  const auto executable = (root / "probe").string();
#endif
  if (!writeTextFile(
          source,
          "#include <stddef.h>\n#include <stdint.h>\n"
          "int main(void) { return sizeof(int32_t) == 4 ? 0 : 1; }\n",
          error))
    return false;
  std::vector<std::string> arguments;
  if (contract.family == CFFICompilerFamily::MSVC)
    arguments = {"/nologo", source, "/Fe:" + executable};
  else {
    arguments = {source, "-o", executable};
    if (!contract.sysroot.empty())
      arguments.insert(arguments.begin(), "--sysroot=" + contract.sysroot);
  }
  auto options = processOptions(contract);
  auto compiled = runProcess(contract.compiler, arguments, options, error);
  if (!compiled || compiled->exit_code != 0) {
    if (compiled)
      error = summarizeCommandFailure(contract.compiler, *compiled);
    std::filesystem::remove_all(root, file_error);
    return false;
  }
  auto executed = runProcess(executable, {}, options, error);
  if (!executed || executed->exit_code != 0) {
    if (executed)
      error = summarizeCommandFailure(executable, *executed);
    std::filesystem::remove_all(root, file_error);
    return false;
  }
  description = std::string(cffiCompilerFamilyName(contract.family)) +
                " ready (" + contract.compiler + ")";
  std::filesystem::remove_all(root, file_error);
  return true;
}

} // namespace chtholly
