#include "chtholly/Driver/CliOutput.h"
#include "chtholly/Driver/ToolchainManager.h"

#include <charconv>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string hostName() {
#if defined(_WIN64)
  return "x86_64-pc-windows-msvc";
#elif defined(_WIN32)
  return "i686-pc-windows-msvc";
#elif defined(__linux__) && defined(__aarch64__)
  return "aarch64-unknown-linux-gnu";
#elif defined(__linux__) && defined(__x86_64__)
  return "x86_64-unknown-linux-gnu";
#elif defined(__linux__) && defined(__i386__)
  return "i686-unknown-linux-gnu";
#elif defined(__APPLE__) && defined(__aarch64__)
  return "aarch64-apple-darwin";
#elif defined(__APPLE__)
  return "x86_64-apple-darwin";
#else
  return "unknown-host";
#endif
}

std::string usage(std::string_view program) {
  return "usage:\n  " + std::string(program) +
         " key generate --secret <secret-key> --public <public-key>\n  " +
         std::string(program) +
         " trust create -o <root-file> --version <n> --threshold <n> --key "
         "<public-key>... --secret-key <secret-key>... [--revoke "
         "<key-id>...]\n  " +
         std::string(program) +
         " trust init|update <root-file> --root <dir>\n  " +
         std::string(program) + " trust inspect --root <dir>\n  " +
         std::string(program) +
         " package <install-tree> -o <archive> --version <semver> "
         "--source-commit <40-hex> [--host <triple>] --secret-key <file>...\n "
         " " +
         std::string(program) +
         " verify|install|upgrade <archive> --root <dir> "
         "[--host <triple>]\n  " +
         std::string(program) +
         " activate|remove <release-id> --root <dir>\n  " +
         std::string(program) + " rollback|list --root <dir>\n  global: " +
         "--output-format human|jsonl|jsonl-v1\n";
}

bool takeValue(const std::vector<std::string> &args, std::size_t &index,
               std::string_view option, std::string &value,
               std::string &error) {
  if (index + 1 >= args.size()) {
    error = std::string(option) + " requires a value";
    return false;
  }
  value = args[++index];
  return true;
}

template <typename Integer>
bool unsignedValue(std::string_view text, Integer &value) {
  const auto [end, status] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return status == std::errc{} && end == text.data() + text.size() && value > 0;
}

void printRelease(const chtholly::ToolchainReleaseInfo &info) {
  std::cout << "release-id\t" << info.release_id << "\nversion\t"
            << info.version << "\nsource-commit\t" << info.source_commit
            << "\nhost\t" << info.host << "\narchive-sha256\t"
            << info.archive_sha256 << "\nfile-count\t" << info.file_count
            << '\n';
  if (info.space_sufficient) {
    std::cout << "space-payload-bytes\t" << info.space_payload_bytes << '\n'
              << "space-index-bytes\t" << info.space_index_bytes << '\n'
              << "space-required-bytes\t" << info.space_required_bytes << '\n'
              << "space-available-bytes\t" << info.space_available_bytes
              << '\n'
              << "space-path\t" << info.space_path << '\n'
              << "space-sufficient\ttrue\n";
  }
}

int run(const std::vector<std::string> &args, std::string &action,
        std::string &error) {
  if (args.size() < 2 || args[1] == "--help" || args[1] == "-h") {
    action = "help";
    std::cout << usage(args.empty() ? "chtholly-toolchain" : args[0]);
    return args.size() < 2 ? 2 : 0;
  }
  action = args[1];
  std::string root, host = hostName(), output, version, commit;
  std::string secret_output, public_output;
  std::uint64_t root_version = 0;
  std::uint32_t threshold = 0;
  bool root_set = false, host_set = false, output_set = false;
  bool version_set = false, threshold_set = false, commit_set = false;
  std::vector<std::string> keys, secrets, revoked, positional;
  for (std::size_t index = 2; index < args.size(); ++index) {
    const auto &arg = args[index];
    if (arg == "--root") {
      root_set = true;
      if (!takeValue(args, index, arg, root, error))
        return 2;
    } else if (arg == "--host") {
      host_set = true;
      if (!takeValue(args, index, arg, host, error))
        return 2;
    } else if (arg == "-o" || arg == "--output") {
      output_set = true;
      if (!takeValue(args, index, arg, output, error))
        return 2;
    } else if (arg == "--version") {
      version_set = true;
      std::string value;
      if (!takeValue(args, index, arg, value, error))
        return 2;
      if (action == "trust") {
        if (!unsignedValue(value, root_version)) {
          error = "--version requires a positive integer for trust roots";
          return 2;
        }
      } else
        version = std::move(value);
    } else if (arg == "--threshold") {
      threshold_set = true;
      std::string value;
      if (!takeValue(args, index, arg, value, error) ||
          !unsignedValue(value, threshold)) {
        error = "--threshold requires a positive integer";
        return 2;
      }
    } else if (arg == "--source-commit") {
      commit_set = true;
      if (!takeValue(args, index, arg, commit, error))
        return 2;
    } else if (arg == "--key") {
      std::string value;
      if (!takeValue(args, index, arg, value, error))
        return 2;
      keys.push_back(std::move(value));
    } else if (arg == "--secret-key") {
      std::string value;
      if (!takeValue(args, index, arg, value, error))
        return 2;
      secrets.push_back(std::move(value));
    } else if (arg == "--secret") {
      if (!takeValue(args, index, arg, secret_output, error))
        return 2;
    } else if (arg == "--public") {
      if (!takeValue(args, index, arg, public_output, error))
        return 2;
    } else if (arg == "--revoke") {
      std::string value;
      if (!takeValue(args, index, arg, value, error))
        return 2;
      revoked.push_back(std::move(value));
    } else if (!arg.empty() && arg[0] == '-') {
      error = "unknown option: " + arg;
      return 2;
    } else
      positional.push_back(arg);
  }

  if (action == "key") {
    action = "key-generate";
    const bool unrelated =
        root_set || host_set || output_set || version_set || threshold_set ||
        commit_set || !keys.empty() || !secrets.empty() || !revoked.empty();
    if (positional.size() != 1 || positional.front() != "generate" ||
        secret_output.empty() || public_output.empty() || unrelated) {
      error = "key generate requires --secret and --public";
      return 2;
    }
    if (!chtholly::generateToolchainSigningKeyFiles(
            secret_output, public_output, error))
      return 1;
    std::cout << "secret-key\t" << secret_output << "\npublic-key\t"
              << public_output << '\n';
    return 0;
  }
  if (!secret_output.empty() || !public_output.empty()) {
    error = action + " does not accept --secret or --public";
    return 2;
  }

  if (action == "trust") {
    if (positional.empty()) {
      error = "trust requires create, init, update, or inspect";
      return 2;
    }
    const auto operation = positional.front();
    action = "trust-" + operation;
    if (operation == "create") {
      if (positional.size() != 1 || root_set || host_set || commit_set) {
        error = "trust create received options or arguments for another "
                "operation";
        return 2;
      }
      chtholly::ToolchainTrustRootRequest request{
          output, root_version, threshold, keys, revoked, secrets};
      if (output.empty()) {
        error = "trust create requires -o";
        return 2;
      }
      if (!chtholly::createToolchainTrustRoot(request, error))
        return 1;
      std::cout << "trust-root\t" << output << "\nroot-version\t"
                << root_version << '\n';
      return 0;
    }
    const bool has_create_options =
        output_set || version_set || threshold_set || commit_set || host_set ||
        !keys.empty() || !secrets.empty() || !revoked.empty();
    if (root.empty()) {
      error = "trust operation requires --root";
      return 2;
    }
    if (operation == "inspect" && positional.size() == 1 &&
        !has_create_options) {
      auto result = chtholly::inspectToolchainTrustRoot(root, error);
      if (!result)
        return 1;
      std::cout << *result;
      return 0;
    }
    if ((operation == "init" || operation == "update") &&
        positional.size() == 2 && !has_create_options) {
      if (!chtholly::installToolchainTrustRoot(root, positional[1],
                                               operation == "init", error))
        return 1;
      std::cout << "trust-root\t" << root << "\noperation\t" << operation
                << '\n';
      return 0;
    }
    error = "invalid trust operation arguments";
    return 2;
  }

  if (action == "package") {
    if (positional.size() != 1 || output.empty() || root_set || threshold_set ||
        !keys.empty() || !revoked.empty()) {
      error = "package requires an install tree and -o";
      return 2;
    }
    chtholly::ToolchainPackageRequest request{positional[0], output, version,
                                              commit,        host,   secrets};
    auto info = chtholly::packageToolchainRelease(request, error);
    if (!info)
      return 1;
    printRelease(*info);
    return 0;
  }
  if (action == "verify" || action == "install" || action == "upgrade") {
    if (positional.size() != 1 || root.empty() || output_set || version_set ||
        threshold_set || commit_set || !keys.empty() || !secrets.empty() ||
        !revoked.empty()) {
      error = action + " requires an archive and --root";
      return 2;
    }
    auto info =
        action == "verify"
            ? chtholly::verifyToolchainRelease(positional[0], root, host, error)
            : chtholly::installToolchainRelease(positional[0], root, host,
                                                action == "upgrade", error);
    if (!info)
      return 1;
    printRelease(*info);
    return 0;
  }
  if (root.empty()) {
    error = action + " requires --root";
    return 2;
  }
  if (host_set || output_set || version_set || threshold_set || commit_set ||
      !keys.empty() || !secrets.empty() || !revoked.empty()) {
    error = action + " received options for another command";
    return 2;
  }
  if (action == "activate" && positional.size() == 1) {
    if (!chtholly::activateToolchainRelease(root, positional[0], error))
      return 1;
    std::cout << "active\t" << positional[0] << '\n';
    return 0;
  }
  if (action == "rollback" && positional.empty()) {
    auto active = chtholly::rollbackToolchainRelease(root, error);
    if (!active)
      return 1;
    std::cout << "active\t" << *active << '\n';
    return 0;
  }
  if (action == "list" && positional.empty()) {
    auto releases = chtholly::listToolchainReleases(root, error);
    if (!releases)
      return 1;
    for (const auto &release : *releases)
      std::cout << release << '\n';
    return 0;
  }
  if (action == "remove" && positional.size() == 1) {
    if (!chtholly::removeToolchainRelease(root, positional[0], error))
      return 1;
    std::cout << "removed\t" << positional[0] << '\n';
    return 0;
  }
  error = "unknown command or invalid command arguments";
  return 2;
}

} // namespace

int main(int argc, char **argv) {
  chtholly::CliOutputFormat format;
  std::vector<std::string> args;
  std::string error;
  if (!chtholly::filterCliOutputArguments(argc, argv, args, format, error)) {
    chtholly::CliOutputSink output(format, std::cout, std::cerr);
    output.diagnostic("chtholly.toolchain.invalid-arguments", error);
    output.result("parse", 2);
    return 2;
  }
  chtholly::CliOutputSink output(format, std::cout, std::cerr);
  std::ostringstream captured;
  auto *original = std::cout.rdbuf(captured.rdbuf());
  std::string action;
  const int exit_code = run(args, action, error);
  std::cout.rdbuf(original);
  output.output(captured.str());
  if (exit_code != 0 && !error.empty()) {
    const bool insufficient_space = error.starts_with("insufficient-space:");
    output.diagnostic(
        exit_code == 2
            ? "chtholly.toolchain.invalid-arguments"
            : (insufficient_space ? "chtholly.toolchain.insufficient-space"
                                   : "chtholly.toolchain.operation-failed"),
        error);
  }
  output.result(action.empty() ? "parse" : action, exit_code);
  return exit_code;
}
