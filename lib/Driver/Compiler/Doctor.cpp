#include "chtholly/Driver/Doctor.h"

#include "chtholly/Driver/CFFIToolchain.h"
#include "chtholly/Driver/CompilerInvocation.h"
#include "chtholly/Driver/NativeLinker.h"
#include "chtholly/Driver/CompilerInputFileSystem.h"
#include "chtholly/Driver/CompilerStandardLibrary.h"
#include "chtholly/Driver/ArtifactCompatibility.h"
#include "chtholly/Compiler/ComponentABI.h"
#include "chtholly/Driver/ProcessRunner.h"
#include "chtholly/Driver/ResourceLocator.h"
#include "chtholly/Driver/TargetConfig.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <filesystem>
#include <optional>
#include <ostream>
#include <string_view>
#include <vector>

namespace chtholly {
namespace {

std::string firstLine(std::string text) {
  const auto end = text.find_first_of("\r\n");
  if (end != std::string::npos)
    text.resize(end);
  return text;
}

bool probeLinker(const std::string &linker, std::string &description,
                 std::string &error) {
  ProcessRunOptions options;
  options.timeout_milliseconds = 5000;
  options.max_stdout_bytes = 64U * 1024U;
  options.max_stderr_bytes = 64U * 1024U;
  const std::vector<std::string> arguments =
      isMsvcStyleLinker(linker) ? std::vector<std::string>{"/?"}
                                : std::vector<std::string>{"--version"};
  auto result = runProcess(linker, arguments, options, error);
  if (!result)
    return false;
  description = firstLine(result->stdout_text.empty() ? result->stderr_text
                                                      : result->stdout_text);
  if (description.empty())
    description = "available";
  return true;
}

std::optional<std::filesystem::path>
locateCFFITool(const CompilerInvocation &invocation) {
#ifdef _WIN32
  constexpr std::string_view Name = "chtholly-cffi.exe";
#else
  constexpr std::string_view Name = "chtholly-cffi";
#endif
  const auto executable = pathForFileSystem(invocation.executable_path);
  const auto directory = executable.parent_path();
  const std::filesystem::path candidates[] = {
      directory / Name,
      directory.parent_path() / "chtholly-cffi" / Name,
      directory.parent_path().parent_path() / "bin" / Name,
  };
  for (const auto &candidate : candidates) {
    std::error_code file_error;
    if (std::filesystem::is_regular_file(candidate, file_error) && !file_error)
      return std::filesystem::weakly_canonical(candidate, file_error);
  }
  return std::nullopt;
}

std::optional<std::filesystem::path>
locateLibclangRuntime(const std::filesystem::path &tool) {
  std::error_code file_error;
  for (std::filesystem::directory_iterator
           iterator(tool.parent_path(), file_error),
       end;
       !file_error && iterator != end; ++iterator) {
    if (!iterator->is_regular_file(file_error) || file_error)
      continue;
    const auto name = iterator->path().filename().string();
#ifdef _WIN32
    if (name == "libclang.dll")
#else
    if (name.starts_with("libclang.so"))
#endif
      return iterator->path();
  }
  return std::nullopt;
}

} // namespace

int runCompilerDoctor(const CompilerInvocation &invocation,
                      std::ostream &output, std::string &error) {
  error.clear();
  output << "compiler\t" << firstLine(compilerVersion()) << '\n';

  auto resources = locateCompilerResources(invocation, error);
  if (!resources) {
    if (error.empty()) {
      error = "compiler resources were not found next to the executable; "
              "reinstall Chtholly or pass --resource-dir <share/chtholly>";
    }
    return 1;
  }
  output << "resources\t" << resources->resource_dir << '\n';
  output << "runtime\t" << resources->runtime_library_path << '\n';

  auto file_system = makeCompilerRealInputFileSystem();
  auto standard_library = CompilerStandardLibraryManifest::load(
      resources->resource_dir, *file_system, error);
  if (!standard_library) {
    error = "standard library validation failed: " + error;
    return 1;
  }
  output << "stdlib\tformat=" << standard_library->formatVersion()
         << " contract=" << standard_library->compilerContractEpoch()
         << " api=" << standard_library->libraryApiEpoch()
         << " modules=" << standard_library->modules().size() << '\n';

  auto target = resolveTargetConfig(invocation, {}, error);
  if (!target) {
    error = "target configuration validation failed: " + error;
    return 1;
  }
  output << "target\t" << target->info.triple << '\n';
  // These facts are derived from the resolved target and shared compatibility
  // constants. They make platform ABI evidence inspectable without creating a
  // second ABI contract.
  output << "pointer-width\t" << target->info.pointer_width_bits << '\n';
  output << "endianness\tlittle\n";
  output << "component-abi\t" << compiler::ComponentAbiEpoch << '\n';
  output << "runtime-abi\t" << HostedRuntimeAbiVersion << '\n';

  const auto cffi_tool = locateCFFITool(invocation);
  if (!cffi_tool && invocation.cffi_config_path.empty()) {
    // The minimal runtime/compiler profile deliberately omits the optional
    // CFFI developer tool. Keep doctor useful for that profile while still
    // requiring a complete CFFI check whenever a tool or explicit config is
    // requested.
    output << "cffi-tool\tunavailable (full install required)\n"
           << "cffi-doctor\tskipped (full install required)\n"
           << "cffi-probe\tskipped (full install required)\n";
  } else {
    CFFIToolchainRequest cffi_request;
    cffi_request.target = target->info.triple;
    if (!invocation.cffi_config_path.empty() &&
        !loadCFFIToolchainRequest(invocation.cffi_config_path, cffi_request,
                                  error)) {
      error = "CFFI config validation failed: " + error;
      return 1;
    }
    CFFIToolchainContract cffi_toolchain;
    if (!resolveCFFIToolchain(cffi_request, cffi_toolchain, error)) {
      error = "CFFI toolchain discovery failed: " + error;
      return 1;
    }
    output << "c-compiler\t" << cffi_toolchain.compiler << " ("
           << cffiCompilerFamilyName(cffi_toolchain.family) << ")\n";
    output << "c-sdk\t" << cffi_toolchain.sdk_name << ' '
           << cffi_toolchain.sdk_version
           << " identity=" << cffi_toolchain.sdk_fingerprint << '\n';
    output << "c-includes\tcount=" << cffi_toolchain.system_include_paths.size()
           << '\n';
    output << "c-libraries\tcount=" << cffi_toolchain.system_library_paths.size()
           << '\n';
    output << "c-discovery\tsteps=" << cffi_toolchain.discovery_trace.size()
           << '\n';
    output << "c-target\trequested=" << cffi_toolchain.target
           << " compiler=" << cffi_toolchain.compiler_target_triple << '\n';
    output << "c-sysroot\tmode=" << cffi_toolchain.sysroot_mode << " path="
           << (cffi_toolchain.sysroot.empty() ? "/" : cffi_toolchain.sysroot)
           << '\n';
    output << "c-multiarch\t" << cffi_toolchain.compiler_multiarch << '\n';
    output << "c-header-probe\tcount="
           << cffi_toolchain.standard_header_probes.size() << '\n';
    output << "c-runtime-files\tcount="
           << cffi_toolchain.runtime_file_probes.size() << '\n';
    output << "c-runtime-arch\t" << cffi_toolchain.runtime_architecture << '\n';
    output << "c-runtime-link-probe\t" << cffi_toolchain.runtime_link_probe
           << '\n';
    if (!cffi_tool) {
      error = "chtholly-cffi was not found in the toolchain installation";
      return 1;
    }
    output << "cffi-tool\t" << cffi_tool->string() << '\n';
    const auto libclang = locateLibclangRuntime(*cffi_tool);
    if (libclang) {
      const auto libclang_digest = sha256File(libclang->string());
      if (!libclang_digest) {
        error = "the chtholly-cffi libclang runtime is unreadable";
        return 1;
      }
      output << "libclang\t" << libclang->string()
             << " sha256=" << *libclang_digest << '\n';
    }
    std::vector<std::string> cffi_doctor_arguments = {"doctor"};
    if (!invocation.cffi_config_path.empty())
      cffi_doctor_arguments.insert(cffi_doctor_arguments.end(),
                                   {"--config", invocation.cffi_config_path});
    else
      cffi_doctor_arguments.insert(cffi_doctor_arguments.end(),
                                   {"--target", target->info.triple});
    ProcessRunOptions cffi_doctor_options;
    cffi_doctor_options.timeout_milliseconds = 60000;
    cffi_doctor_options.max_stdout_bytes = cffi_doctor_options.max_stderr_bytes =
        1024U * 1024U;
    cffi_doctor_options.environment_overrides =
        cffi_toolchain.environment_overrides;
    auto cffi_doctor = runProcess(cffi_tool->string(), cffi_doctor_arguments,
                                  cffi_doctor_options, error);
    if (!cffi_doctor || cffi_doctor->exit_code != 0) {
      if (cffi_doctor)
        error = summarizeCommandFailure(cffi_tool->string(), *cffi_doctor);
      error = "independent CFFI doctor failed: " + error;
      return 1;
    }
    if (!libclang) {
      const auto marker = cffi_doctor->stdout_text.find("libclang\t");
      const auto end = marker == std::string::npos
                           ? std::string::npos
                           : cffi_doctor->stdout_text.find('\n', marker);
      if (marker == std::string::npos) {
        error = "independent CFFI doctor did not report its loaded libclang";
        return 1;
      }
      output << cffi_doctor->stdout_text.substr(marker, end - marker) << '\n';
    }
    output << "cffi-doctor\tready\n";
    output << "cffi-probe\tvalidated by independent doctor\n";
  }

  const auto linker = defaultNativeLinkerForTarget(*target);
  if (linker.empty()) {
    error = "no native linker is configured for target '" +
            target->info.triple + "'; pass --linker <path>";
    return 1;
  }
  std::string linker_description;
  if (!probeLinker(linker, linker_description, error)) {
    error = "native linker '" + linker + "' could not be started: " + error +
            "; install a host linker or pass --linker <path>";
    return 1;
  }
  output << "linker\t" << linker << " (" << linker_description << ")\n";
  output << "doctor\tready\n";
  return 0;
}

} // namespace chtholly
