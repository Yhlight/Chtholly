#include "chtholly/Driver/CompilerInvocation.h"

#include "chtholly/Config/Version.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <limits>
#include <sstream>

namespace chtholly {
namespace {

bool isOption(std::string_view value) {
  return value.starts_with('-');
}

bool parsePositiveSize(std::string_view text, std::size_t &value) {
  if (text.empty())
    return false;
  std::size_t parsed = 0;
  for (const char ch : text) {
    if (!std::isdigit(static_cast<unsigned char>(ch)))
      return false;
    const auto digit = static_cast<std::size_t>(ch - '0');
    if (parsed > (std::numeric_limits<std::size_t>::max() - digit) / 10)
      return false;
    parsed = parsed * 10 + digit;
  }
  if (parsed == 0)
    return false;
  value = parsed;
  return true;
}

bool validFeaturePart(std::string_view text) {
  if (text.empty())
    return false;
  for (const char ch : text) {
    if (ch == '/' || ch == '\t' || ch == '\n' || ch == '\r')
      return false;
  }
  return true;
}

bool parseFeature(std::string_view text, CompilerInvocation &invocation,
                  std::string &error) {
  const auto slash = text.find('/');
  if (slash == std::string_view::npos) {
    if (!validFeaturePart(text)) {
      error = "invalid feature name for --feature";
      return false;
    }
    invocation.feature_selections.emplace_back(text);
    return true;
  }
  if (slash == 0 || slash + 1 == text.size() ||
      text.find('/', slash + 1) != std::string_view::npos ||
      !validFeaturePart(text.substr(0, slash)) ||
      !validFeaturePart(text.substr(slash + 1))) {
    error = "invalid package-qualified feature for --feature";
    return false;
  }
  invocation.package_feature_selections.push_back(
      {std::string(text.substr(0, slash)),
       std::string(text.substr(slash + 1))});
  return true;
}

bool parseAbi(std::string_view text, AbiVersion &version) {
  if (text == "v0")
    version = AbiVersion::V0;
  else if (text == "v1")
    version = AbiVersion::V1;
  else if (text == "v2")
    version = AbiVersion::V2;
  else
    return false;
  return true;
}

std::size_t editDistance(std::string_view left, std::string_view right) {
  std::vector<std::size_t> previous(right.size() + 1);
  std::vector<std::size_t> current(right.size() + 1);
  for (std::size_t index = 0; index <= right.size(); ++index)
    previous[index] = index;
  for (std::size_t left_index = 0; left_index < left.size(); ++left_index) {
    current[0] = left_index + 1;
    for (std::size_t right_index = 0; right_index < right.size();
         ++right_index) {
      current[right_index + 1] =
          std::min({current[right_index] + 1, previous[right_index + 1] + 1,
                    previous[right_index] +
                        (left[left_index] == right[right_index] ? 0U : 1U)});
    }
    previous.swap(current);
  }
  return previous.back();
}

std::string commandSuggestion(std::string_view input) {
  static constexpr std::array Commands = {"new",   "init", "check",
                                          "build", "run",  "doctor"};
  const auto best = std::ranges::min_element(
      Commands, [&](std::string_view left, std::string_view right) {
        return editDistance(input, left) < editDistance(input, right);
      });
  return best != Commands.end() && editDistance(input, *best) <= 3
             ? std::string(*best)
             : std::string{};
}

} // namespace

std::string_view driverActionName(DriverAction action) {
  switch (action) {
  case DriverAction::EmitLLVM:
    return "emit-llvm";
  case DriverAction::EmitObject:
    return "emit-object";
  case DriverAction::EmitExecutable:
    return "emit-exe";
  case DriverAction::Check:
    return "check";
  case DriverAction::Scaffold:
    return "scaffold";
  case DriverAction::Doctor:
    return "doctor";
  case DriverAction::Help:
    return "help";
  case DriverAction::Version:
    return "version";
  }
  return "unknown";
}

bool validateCompilerInvocation(const CompilerInvocation &invocation,
                                    std::string &error) {
  if (invocation.action == DriverAction::Help ||
      invocation.action == DriverAction::Version)
    return true;
  if (invocation.action == DriverAction::Doctor) {
    if (!invocation.project_path.empty() || !invocation.manifest_path.empty() ||
        !invocation.workspace_path.empty() || !invocation.input_path.empty()) {
      error = "doctor does not accept a source, project, or workspace";
      return false;
    }
    return true;
  }
  if (invocation.action == DriverAction::Scaffold) {
    if (invocation.scaffold_path.empty() || invocation.scaffold_name.empty()) {
      error = "project scaffolding requires a target and package name";
      return false;
    }
    return true;
  }

  const auto graph_count = (invocation.project_path.empty() ? 0 : 1) +
                           (invocation.manifest_path.empty() ? 0 : 1) +
                           (invocation.workspace_path.empty() ? 0 : 1);
  if (graph_count > 1) {
    error = "pass only one of --project, --manifest, or --workspace";
    return false;
  }
  if ((!invocation.input_path.empty() && graph_count != 0) ||
      (invocation.workflow == DriverWorkflow::Compile &&
       invocation.input_path.empty() && graph_count == 0)) {
    error = "the compiler compiler requires one direct input or one "
            "project/workspace root";
    return false;
  }
  if (invocation.workflow != DriverWorkflow::Compile &&
      (!invocation.input_path.empty() || !invocation.manifest_path.empty())) {
    error = "check, build, and run accept --project or --workspace";
    return false;
  }
  if (!invocation.output_path.empty() && !invocation.out_dir.empty()) {
    error = "pass either -o or --out-dir, not both";
    return false;
  }
  if (invocation.workflow == DriverWorkflow::Compile &&
      invocation.output_path.empty() && invocation.out_dir.empty()) {
    error = "-" + std::string(driverActionName(invocation.action)) +
            " requires -o <output> or --out-dir <dir>";
    return false;
  }
  if (invocation.locked && invocation.disable_lockfile) {
    error = "pass either --locked or --no-lockfile, not both";
    return false;
  }
  if (invocation.abi_version_specified) {
    error = "--abi-version is not supported by this compiler";
    return false;
  }
  if (invocation.action != DriverAction::EmitExecutable &&
      (!invocation.sysroot_path.empty() || !invocation.linker_path.empty() ||
       !invocation.library_search_paths.empty() ||
       !invocation.link_libraries.empty())) {
    error = "native linker options require -emit-exe";
    return false;
  }
  const std::string *outputs[] = {
      &invocation.output_path,
      &invocation.compiler_tokens_output_path,
      &invocation.compiler_parse_tree_output_path,
      &invocation.compiler_sem_ir_output_path,
      &invocation.compiler_low_ir_output_path,
      &invocation.compiler_foreign_protocols_output_path,
      &invocation.compiler_metrics_output_path,
      &invocation.compiler_analysis_metrics_output_path,
      &invocation.compiler_artifact_load_metrics_output_path,
  };
  for (std::size_t left = 0; left < std::size(outputs); ++left) {
    if (outputs[left]->empty())
      continue;
    for (std::size_t right = left + 1; right < std::size(outputs); ++right) {
      if (*outputs[left] == *outputs[right]) {
        error = "compiler compiler outputs must use distinct paths";
        return false;
      }
    }
  }
  return true;
}

std::string compilerVersion() {
#if defined(_MSC_VER)
  constexpr std::string_view compiler = "msvc";
#elif defined(__clang__)
  constexpr std::string_view compiler = "clang";
#elif defined(__GNUC__)
  constexpr std::string_view compiler = "gcc";
#else
  constexpr std::string_view compiler = "unknown-compiler";
#endif

#if defined(_WIN64)
  constexpr std::string_view platform = "windows-x64";
#elif defined(_WIN32)
  constexpr std::string_view platform = "windows-x86";
#elif defined(__linux__)
  constexpr std::string_view platform = "linux";
#elif defined(__APPLE__)
  constexpr std::string_view platform = "darwin";
#else
  constexpr std::string_view platform = "unknown-platform";
#endif

  std::ostringstream out;
  out << "chthollyc " << CHTHOLLY_VERSION_FULL << " (" << platform << ", "
      << compiler << ", release " << CHTHOLLY_RELEASE_ID << ")\n";
  return out.str();
}

std::string compilerUsage(std::string_view program) {
  std::ostringstream out;
  out << "usage:\n"
      << "  " << program << " new <path> [--name <name>] [--lib]\n"
      << "  " << program << " init [--name <name>] [--lib]\n"
      << "  " << program
      << " doctor [--resource-dir <dir>] [--target <triple>] "
         "[--linker <path>] [--cffi-config <file>]\n"
      << "  " << program
      << " check [--project <dir> | --workspace <dir>] [--package <name>] "
         "[--jobs <n>]\n"
      << "  " << program
      << " build [--project <dir> | --workspace <dir>] [--package <name>] "
         "[--out-dir <dir>] [--jobs <n>]\n"
      << "  " << program
      << " run [--project <dir> | --workspace <dir>] [--package <name>] "
         "[-- <program-args>...]\n"
      << "  " << program
      << " <input.cns> -emit-llvm -o <output.ll> [--cache-dir <dir>]\n"
      << "  " << program
      << " <input.cns> -emit-object [--target <triple>] -o <output.obj> "
         "[--cache-dir <dir>]\n"
      << "  " << program
      << " <input.cns> -emit-exe [--target <triple>] [--sysroot <dir>] "
         "[--linker <path>] [-L <dir>] [-l <name>] -o <output.exe> "
         "[--cache-dir <dir>]\n"
      << "  " << program
      << " (--project <dir> | --workspace <dir>) [--package <name>] "
         "[--feature <name>|<package>/<name>] [--disable-default-features] "
         "[--no-default-feature <package>] [--locked|--no-lockfile]\n"
      << "  --dump-{tokens,parse-tree,semir,lowir,foreign-protocols,metrics,"
         "analysis-metrics,artifact-load-metrics} <path>\n"
      << "  --resource-dir <dir> overrides standard-library and hosted-runtime "
         "discovery\n"
      << "  --explain-invalidation reports incremental cache hits and rebuild "
         "reasons for check and build\n"
      << "  --output-format human|jsonl|jsonl-v1 selects the versioned CLI "
         "output contract; jsonl is unavailable for run\n"
      << "  " << program << " --help\n"
      << "  " << program << " --version\n";
  return out.str();
}

bool parseCompilerInvocation(int argc, char **argv,
                             CompilerInvocation &invocation,
                             std::string &error) {
  invocation = CompilerInvocation{};
  invocation.executable_path = argc > 0 ? argv[0] : "chthollyc";
  const std::string_view program = invocation.executable_path;
  if (argc <= 1) {
    error = compilerUsage(program);
    return false;
  }
  if (std::string_view(argv[1]) == "artifact") {
    error = "artifact commands have been retired from chthollyc; use the "
            "dedicated package tooling";
    return false;
  }
  if (std::string_view(argv[1]) == "registry") {
    error = "registry commands have been retired from chthollyc; use the "
            "dedicated registry tooling";
    return false;
  }
  const std::string_view first_argument(argv[1]);
  if (!isOption(first_argument) &&
      first_argument.find_first_of("/\\") == std::string_view::npos &&
      std::filesystem::path(first_argument).extension() != ".cns" &&
      first_argument != "new" && first_argument != "init" &&
      first_argument != "check" && first_argument != "build" &&
      first_argument != "run" && first_argument != "doctor") {
    const auto suggestion = commandSuggestion(first_argument);
    error = "unknown command '" + std::string(first_argument) + "'";
    if (!suggestion.empty())
      error += "; did you mean '" + suggestion + "'?";
    return false;
  }

  int first = 1;
  if (std::string_view(argv[1]) == "new") {
    invocation.workflow = DriverWorkflow::New;
    invocation.action = DriverAction::Scaffold;
    first = 2;
  } else if (std::string_view(argv[1]) == "init") {
    invocation.workflow = DriverWorkflow::Init;
    invocation.action = DriverAction::Scaffold;
    invocation.scaffold_path = ".";
    first = 2;
  } else if (std::string_view(argv[1]) == "check") {
    invocation.workflow = DriverWorkflow::Check;
    invocation.action = DriverAction::Check;
    first = 2;
  } else if (std::string_view(argv[1]) == "build") {
    invocation.workflow = DriverWorkflow::Build;
    invocation.action = DriverAction::EmitExecutable;
    first = 2;
  } else if (std::string_view(argv[1]) == "run") {
    invocation.workflow = DriverWorkflow::Run;
    invocation.action = DriverAction::EmitExecutable;
    first = 2;
  } else if (std::string_view(argv[1]) == "doctor") {
    invocation.workflow = DriverWorkflow::Doctor;
    invocation.action = DriverAction::Doctor;
    first = 2;
  }

  const auto takeValue = [&](int &index, std::string &destination,
                             std::string_view option) {
    if (index + 1 >= argc) {
      error = std::string(option) + " requires a value";
      return false;
    }
    destination = argv[++index];
    return true;
  };

  if (invocation.action == DriverAction::Scaffold) {
    for (int index = first; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--help" || argument == "-h") {
        invocation.action = DriverAction::Help;
        return true;
      }
      if (argument == "--lib") {
        invocation.scaffold_library = true;
      } else if (argument == "--name") {
        if (!takeValue(index, invocation.scaffold_name, argument))
          return false;
      } else if (isOption(argument)) {
        error = "unknown project scaffolding option: " + std::string(argument);
        return false;
      } else if (invocation.workflow == DriverWorkflow::Init) {
        error = "init does not accept a target path";
        return false;
      } else if (!invocation.scaffold_path.empty()) {
        error = "new accepts exactly one target path";
        return false;
      } else {
        invocation.scaffold_path = argument;
      }
    }
    if (invocation.scaffold_path.empty()) {
      error = "new requires a target path";
      return false;
    }
    if (invocation.scaffold_name.empty()) {
      auto default_path = invocation.scaffold_path;
      if (invocation.workflow == DriverWorkflow::Init) {
        std::error_code file_error;
        default_path =
            std::filesystem::current_path(file_error).filename().string();
        if (file_error) {
          error = "failed to determine current directory name: " +
                  file_error.message();
          return false;
        }
      }
      auto name = std::string_view(default_path);
      while (!name.empty() && (name.back() == '/' || name.back() == '\\'))
        name.remove_suffix(1);
      const auto separator = name.find_last_of("/\\");
      invocation.scaffold_name = std::string(separator == std::string_view::npos
                                                 ? name
                                                 : name.substr(separator + 1));
    }
    return validateCompilerInvocation(invocation, error);
  }

  for (int index = first; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--" && invocation.workflow == DriverWorkflow::Run) {
      for (++index; index < argc; ++index)
        invocation.program_arguments.emplace_back(argv[index]);
      break;
    }
    if (argument == "--help" || argument == "-h") {
      invocation.action = DriverAction::Help;
      return true;
    }
    if (argument == "--version") {
      invocation.action = DriverAction::Version;
      return true;
    }
    const auto dump = [&](std::string &path) {
      return takeValue(index, path, argument);
    };
    if (argument == "--dump-tokens" || argument == "--next-dump-tokens") {
      if (!dump(invocation.compiler_tokens_output_path))
        return false;
      continue;
    }
    if (argument == "--dump-parse-tree" ||
        argument == "--next-dump-parse-tree") {
      if (!dump(invocation.compiler_parse_tree_output_path))
        return false;
      continue;
    }
    if (argument == "--dump-semir" || argument == "--next-dump-semir") {
      if (!dump(invocation.compiler_sem_ir_output_path))
        return false;
      continue;
    }
    if (argument == "--dump-lowir" || argument == "--next-dump-lowir") {
      if (!dump(invocation.compiler_low_ir_output_path))
        return false;
      continue;
    }
    if (argument == "--dump-foreign-protocols" ||
        argument == "--next-dump-foreign-protocols") {
      if (!dump(invocation.compiler_foreign_protocols_output_path))
        return false;
      continue;
    }
    if (argument == "--dump-metrics" || argument == "--next-dump-metrics") {
      if (!dump(invocation.compiler_metrics_output_path))
        return false;
      continue;
    }
    if (argument == "--dump-analysis-metrics" ||
        argument == "--next-dump-analysis-metrics") {
      if (!dump(invocation.compiler_analysis_metrics_output_path))
        return false;
      continue;
    }
    if (argument == "--dump-artifact-load-metrics" ||
        argument == "--next-dump-artifact-load-metrics") {
      if (!dump(invocation.compiler_artifact_load_metrics_output_path))
        return false;
      continue;
    }

    if (argument == "-emit-llvm" || argument == "-emit-object" ||
        argument == "-emit-exe") {
      if (invocation.workflow != DriverWorkflow::Compile) {
        error = "check, build, and run do not accept -emit-* actions";
        return false;
      }
      invocation.action = argument == "-emit-llvm" ? DriverAction::EmitLLVM
                          : argument == "-emit-object"
                              ? DriverAction::EmitObject
                              : DriverAction::EmitExecutable;
      continue;
    }
    if (invocation.workflow == DriverWorkflow::Doctor &&
        argument != "--resource-dir" && argument != "--target" &&
        argument != "--linker" && argument != "--cffi-config") {
      error = "unknown doctor option: " + std::string(argument);
      return false;
    }
    if (argument == "--all") {
      error = "the compiler compiler supports exactly one selected package";
      return false;
    }

    if (argument == "-O0")
      invocation.optimization = OptimizationLevel::O0;
    else if (argument == "-O1")
      invocation.optimization = OptimizationLevel::O1;
    else if (argument == "-O2")
      invocation.optimization = OptimizationLevel::O2;
    else if (argument == "-O3")
      invocation.optimization = OptimizationLevel::O3;
    else if (argument == "-Os")
      invocation.optimization = OptimizationLevel::Os;
    else if (argument == "-Oz")
      invocation.optimization = OptimizationLevel::Oz;
    else if (argument == "-g0")
      invocation.debug_info = DebugInfoKind::None;
    else if (argument == "-gline-tables-only")
      invocation.debug_info = DebugInfoKind::LineTablesOnly;
    else if (argument == "-g")
      invocation.debug_info = DebugInfoKind::Full;
    else if (argument == "--explain-invalidation")
      invocation.explain_invalidation = true;
    else if (argument == "--resource-dir") {
      if (!takeValue(index, invocation.resource_dir, argument))
        return false;
    } else if (argument == "--cffi-config") {
      if (invocation.workflow != DriverWorkflow::Doctor ||
          !takeValue(index, invocation.cffi_config_path, argument))
        return false;
    } else if (argument == "-o") {
      if (invocation.workflow != DriverWorkflow::Compile) {
        error = "check, build, and run do not accept -o";
        return false;
      }
      if (!takeValue(index, invocation.output_path, argument))
        return false;
    } else if (argument == "--project") {
      if (!takeValue(index, invocation.project_path, argument))
        return false;
    } else if (argument == "--manifest") {
      if (!takeValue(index, invocation.manifest_path, argument))
        return false;
    } else if (argument == "--workspace") {
      if (!takeValue(index, invocation.workspace_path, argument))
        return false;
    } else if (argument == "--package" || argument == "-package") {
      if (!invocation.package_name.empty()) {
        error = "the compiler compiler supports exactly one selected package";
        return false;
      }
      if (!takeValue(index, invocation.package_name, argument))
        return false;
    } else if (argument == "--feature") {
      std::string feature;
      if (!takeValue(index, feature, argument) ||
          !parseFeature(feature, invocation, error))
        return false;
    } else if (argument == "--disable-default-features") {
      invocation.enable_default_features = false;
    } else if (argument == "--no-default-feature") {
      std::string package;
      if (!takeValue(index, package, argument))
        return false;
      if (!validFeaturePart(package)) {
        error = "invalid package name for --no-default-feature";
        return false;
      }
      invocation.default_feature_disabled_packages.push_back(
          std::move(package));
    } else if (argument == "--out-dir") {
      if (!takeValue(index, invocation.out_dir, argument))
        return false;
    } else if (argument == "--jobs") {
      std::string value;
      if (!takeValue(index, value, argument) ||
          !parsePositiveSize(value, invocation.jobs)) {
        error = "--jobs requires a positive integer";
        return false;
      }
      invocation.jobs_specified = true;
    } else if (argument == "--cache-dir") {
      if (!takeValue(index, invocation.cache_dir, argument))
        return false;
    } else if (argument == "--locked") {
      invocation.locked = true;
    } else if (argument == "--no-lockfile") {
      invocation.disable_lockfile = true;
    } else if (argument == "--target") {
      if (!takeValue(index, invocation.target_triple, argument))
        return false;
    } else if (argument == "--sysroot") {
      if (!takeValue(index, invocation.sysroot_path, argument))
        return false;
    } else if (argument == "--linker") {
      if (!takeValue(index, invocation.linker_path, argument))
        return false;
    } else if (argument == "-L") {
      std::string value;
      if (!takeValue(index, value, argument))
        return false;
      invocation.library_search_paths.push_back(std::move(value));
    } else if (argument == "-l") {
      std::string value;
      if (!takeValue(index, value, argument))
        return false;
      invocation.link_libraries.push_back(std::move(value));
    } else if (argument == "--abi-version") {
      std::string value;
      if (!takeValue(index, value, argument) ||
          !parseAbi(value, invocation.abi_version)) {
        error = "--abi-version requires v0, v1, or v2";
        return false;
      }
      invocation.abi_version_specified = true;
    } else if (isOption(argument)) {
      error = "unknown option: " + std::string(argument);
      return false;
    } else {
      if (!invocation.input_path.empty()) {
        error = "pass exactly one entry file; imports are discovered from the "
                "module graph";
        return false;
      }
      invocation.input_path = argument;
    }
  }

  return validateCompilerInvocation(invocation, error);
}

} // namespace chtholly
