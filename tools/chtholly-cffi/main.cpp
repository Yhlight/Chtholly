#include "chtholly/Driver/CFFITool.h"
#include "chtholly/Support/FileSystem.h"

#include <filesystem>
#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void usage(std::ostream &out) {
  out << "usage:\n"
         "  chtholly-cffi generate --config <file> -o <binding.cfdl> "
         "[--state <binding.cffi-state>] "
         "[--output-format human|jsonl-v1]\n"
         "  chtholly-cffi regenerate --config <file> <binding.cfdl> "
         "[--state <binding.cffi-state>] [--write] "
         "[--output-format human|jsonl-v1]\n"
         "  chtholly-cffi verify --config <file> <binding.cfdl> "
         "--receipt <file> [--keep-temp] "
         "[--output-format human|jsonl-v1]\n"
         "  chtholly-cffi doctor [--config <file>] [--target <triple>] "
         "[--cache-dir <dir>] [--cache-max-entries <n>] "
         "[--cache-max-age-seconds <n>] [--output-format human|jsonl-v1]\n"
         "  chtholly-cffi cache-gc --cache-dir <dir> [--cache-max-entries <n>] "
         "[--cache-max-age-seconds <n>] [--output-format human|jsonl-v1]\n";
}

std::string jsonEscape(std::string_view value) {
  std::string out;
  for (const char ch : value) {
    if (ch == '\\' || ch == '"')
      out.push_back('\\');
    if (ch == '\n')
      out += "\\n";
    else if (ch == '\r')
      out += "\\r";
    else
      out.push_back(ch);
  }
  return out;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2 || std::string_view(argv[1]) == "--help" ||
      std::string_view(argv[1]) == "-h") {
    usage(argc < 2 ? std::cerr : std::cout);
    return argc < 2 ? 2 : 0;
  }
  const std::string action = argv[1];
  std::string config_path, output_path, receipt_path, input_path, state_path;
  std::string target;
  std::string cache_dir;
  std::size_t cache_max_entries = 32;
  std::chrono::seconds cache_max_age = std::chrono::hours(24 * 30);
  std::string output_format = "human";
  bool keep_temp = false;
  bool write = false;
  for (int index = 2; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto take = [&](std::string &value) {
      if (++index >= argc)
        return false;
      value = argv[index];
      return !value.empty();
    };
    if (argument == "--config") {
      if (!take(config_path)) {
        std::cerr << "chtholly-cffi: --config requires a value\n";
        return 2;
      }
    } else if (argument == "-o" || argument == "--output") {
      if (!take(output_path)) {
        std::cerr << "chtholly-cffi: --output requires a value\n";
        return 2;
      }
    } else if (argument == "--receipt") {
      if (!take(receipt_path)) {
        std::cerr << "chtholly-cffi: --receipt requires a value\n";
        return 2;
      }
    } else if (argument == "--state") {
      if (!take(state_path)) {
        std::cerr << "chtholly-cffi: --state requires a value\n";
        return 2;
      }
    } else if (argument == "--target") {
      if (!take(target)) {
        std::cerr << "chtholly-cffi: --target requires a value\n";
        return 2;
      }
    } else if (argument == "--cache-dir") {
      if (!take(cache_dir)) {
        std::cerr << "chtholly-cffi: --cache-dir requires a value\n";
        return 2;
      }
    } else if (argument == "--cache-max-entries") {
      std::string value;
      if (!take(value)) { std::cerr << "chtholly-cffi: --cache-max-entries requires a value\n"; return 2; }
      try { cache_max_entries = std::stoull(value); } catch (...) { std::cerr << "chtholly-cffi: invalid --cache-max-entries\n"; return 2; }
    } else if (argument == "--cache-max-age-seconds") {
      std::string value;
      if (!take(value)) { std::cerr << "chtholly-cffi: --cache-max-age-seconds requires a value\n"; return 2; }
      try { cache_max_age = std::chrono::seconds(std::stoll(value)); } catch (...) { std::cerr << "chtholly-cffi: invalid --cache-max-age-seconds\n"; return 2; }
    } else if (argument == "--output-format") {
      if (!take(output_format) ||
          (output_format != "human" && output_format != "jsonl-v1")) {
        std::cerr << "chtholly-cffi: invalid --output-format\n";
        return 2;
      }
    } else if (argument == "--keep-temp") {
      keep_temp = true;
    } else if (argument == "--write") {
      write = true;
    } else if (!argument.empty() && argument.front() == '-') {
      std::cerr << "chtholly-cffi: unknown option: " << argument << '\n';
      return 2;
    } else if (input_path.empty()) {
      input_path = argument;
    } else {
      std::cerr << "chtholly-cffi: unexpected argument: " << argument << '\n';
      return 2;
    }
  }
  std::string error;
  bool usage_error = false;
  chtholly::CFFIConfig config;
  if (action == "cache-gc") {
    if (config_path.empty() && cache_dir.empty()) {
      error = "cache-gc requires --cache-dir";
      usage_error = true;
    } else {
      chtholly::CFFIToolchainCacheMetrics metrics;
      chtholly::CFFIToolchainCacheOptions options{.directory = cache_dir,
          .max_entries = cache_max_entries, .max_age = cache_max_age,
          .metrics = &metrics};
      if (chtholly::pruneCFFIToolchainCache(options, error)) {
        if (output_format == "human")
          std::cout << "cache-expired\t" << metrics.expired_entries << "\ncache-evicted\t" << metrics.evictions << '\n';
        else
          std::cout << "{\"event\":\"cache-metrics\",\"expired\":" << metrics.expired_entries << ",\"evictions\":" << metrics.evictions << "}\n";
        return 0;
      }
    }
  } else if (action == "doctor") {
    if (!input_path.empty() || !output_path.empty() || !receipt_path.empty() ||
        !state_path.empty() || keep_temp || write) {
      error = "doctor accepts only --config, --target, and --output-format";
      usage_error = true;
    } else if (!config_path.empty() &&
               !chtholly::loadCFFIConfig(config_path, config, error,
                                         cache_dir)) {
    } else {
      chtholly::CFFIDoctorReport report;
      chtholly::CFFIToolchainCacheMetrics cache_metrics;
      if (chtholly::doctorCFFI(
              config_path.empty() ? nullptr : &config, target, report, error,
              chtholly::CFFIToolchainCacheOptions{.directory = cache_dir,
                .max_entries = cache_max_entries, .max_age = cache_max_age,
                .metrics = &cache_metrics})) {
        const auto emit = [&](std::string_view name, const std::string &value) {
          if (output_format == "human")
            std::cout << name << '\t' << value << '\n';
          else
            std::cout << "{\"event\":\"doctor\",\"name\":\"" << jsonEscape(name)
                      << "\",\"value\":\"" << jsonEscape(value) << "\"}\n";
        };
        emit("c-compiler", report.toolchain.compiler);
        if (output_format == "jsonl-v1") {
          for (const auto &entry : report.toolchain.discovery_trace)
            std::cout
                << "{\"event\":\"discovery\",\"kind\":\"trace\",\"value\":\""
                << jsonEscape(entry) << "\"}\n";
          std::cout << "{\"event\":\"environment\",\"target\":\""
                    << jsonEscape(report.toolchain.target) << "\",\"family\":\""
                    << jsonEscape(std::string(chtholly::cffiCompilerFamilyName(
                           report.toolchain.family)))
                    << "\",\"compiler_version\":\""
                    << jsonEscape(report.toolchain.compiler_version)
                    << "\",\"fingerprint\":\""
                    << jsonEscape(report.toolchain.fingerprint) << "\"}\n";
          std::cout << "{\"event\":\"target\",\"requested\":\""
                    << jsonEscape(report.toolchain.target)
                    << "\",\"compiler\":\""
                    << jsonEscape(report.toolchain.compiler_target_triple)
                    << "\",\"multiarch\":\""
                    << jsonEscape(report.toolchain.compiler_multiarch)
                    << "\",\"sysroot_mode\":\""
                    << jsonEscape(report.toolchain.sysroot_mode) << "\"}\n";
          for (const auto &probe : report.toolchain.standard_header_probes)
            std::cout << "{\"event\":\"header-probe\",\"value\":\""
                      << jsonEscape(probe) << "\"}\n";
          for (const auto &probe : report.toolchain.runtime_file_probes)
            std::cout << "{\"event\":\"runtime-file\",\"value\":\""
                      << jsonEscape(probe) << "\"}\n";
          std::cout << "{\"event\":\"runtime-link-probe\",\"value\":\""
                    << jsonEscape(report.toolchain.runtime_link_probe)
                    << "\"}\n";
          for (const auto &component : report.toolchain.validated_components)
            std::cout
                << "{\"event\":\"component\",\"status\":\"ok\",\"value\":\""
                << jsonEscape(component) << "\"}\n";
          for (const auto &component : report.toolchain.missing_components)
            std::cout << "{\"event\":\"component\",\"status\":\"missing\","
                         "\"value\":\""
                      << jsonEscape(component) << "\"}\n";
          std::cout << "{\"event\":\"cache-metrics\",\"memory_hits\":" << cache_metrics.memory_hits
                    << ",\"disk_hits\":" << cache_metrics.disk_hits << ",\"misses\":" << cache_metrics.misses
                    << ",\"invalid\":" << cache_metrics.invalid_entries << ",\"expired\":" << cache_metrics.expired_entries
                    << ",\"evictions\":" << cache_metrics.evictions << ",\"bytes_read\":" << cache_metrics.bytes_read
                    << ",\"bytes_written\":" << cache_metrics.bytes_written << "}\n";
        }
        if (output_format == "human")
          std::cout << "cache\tmemory_hits=" << cache_metrics.memory_hits << " disk_hits=" << cache_metrics.disk_hits
                    << " misses=" << cache_metrics.misses << " expired=" << cache_metrics.expired_entries
                    << " evictions=" << cache_metrics.evictions << '\n';
        emit("c-sdk",
             report.toolchain.sdk_name + " " + report.toolchain.sdk_version);
        emit("c-includes",
             "count=" +
                 std::to_string(report.toolchain.system_include_paths.size()));
        emit("libclang",
             report.libclang_path + " sha256=" + report.libclang_digest);
        emit("cffi-probe", report.probe_description);
        emit("doctor", "ready");
        return 0;
      }
    }
  } else if (config_path.empty()) {
    error = "--config is required";
    usage_error = true;
  } else if (!chtholly::loadCFFIConfig(config_path, config, error,
                                       cache_dir)) {
  } else if (action == "generate") {
    if (output_path.empty() || !input_path.empty() || !receipt_path.empty() ||
        keep_temp || write) {
      error = "generate requires exactly --config and -o";
      usage_error = true;
    } else {
      if (state_path.empty())
        state_path = chtholly::defaultCFFIStatePath(output_path);
      chtholly::CFFIGeneration generation;
      if (chtholly::generateCFFIWithState(config, generation, error) &&
          chtholly::writeCFFIGeneration(output_path, state_path, generation,
                                        error)) {
        if (output_format == "human")
          std::cout << "generated\t" << output_path << "\nstate\t" << state_path
                    << '\n';
        else
          std::cout << "{\"event\":\"generated\",\"path\":\""
                    << jsonEscape(output_path) << "\",\"state\":\""
                    << jsonEscape(state_path) << "\"}\n";
        return 0;
      }
    }
  } else if (action == "regenerate") {
    if (input_path.empty() || !output_path.empty() || !receipt_path.empty() ||
        keep_temp) {
      error = "regenerate requires --config and one CFDL input";
      usage_error = true;
    } else {
      if (state_path.empty())
        state_path = chtholly::defaultCFFIStatePath(input_path);
      chtholly::CFFIRegeneration regeneration;
      if (chtholly::regenerateCFFI(config, input_path, state_path, regeneration,
                                   error)) {
        for (const auto &change : regeneration.changes) {
          const auto kind = chtholly::cffiRegenerationChangeName(change.kind);
          if (output_format == "human") {
            std::cout << kind << '\t' << change.declaration_kind << '\t'
                      << change.key;
            if (!change.detail.empty())
              std::cout << '\t' << change.detail;
            std::cout << '\n';
          } else {
            std::cout << "{\"event\":\"regeneration-change\",\"kind\":\""
                      << kind << "\",\"declaration\":\""
                      << jsonEscape(change.declaration_kind) << "\",\"key\":\""
                      << jsonEscape(change.key) << "\",\"detail\":\""
                      << jsonEscape(change.detail) << "\"}\n";
          }
        }
        if (write && !chtholly::applyCFFIRegeneration(input_path, state_path,
                                                      regeneration, error)) {
          // The shared error path below reports a failed atomic update.
        } else if (write) {
          if (output_format == "human")
            std::cout << "regenerated\t" << input_path << "\nstate\t"
                      << state_path << '\n';
          else
            std::cout << "{\"event\":\"regenerated\",\"path\":\""
                      << jsonEscape(input_path) << "\",\"state\":\""
                      << jsonEscape(state_path) << "\"}\n";
          return 0;
        } else {
          return regeneration.changed ? 3 : 0;
        }
      }
    }
  } else if (action == "verify") {
    if (input_path.empty() || receipt_path.empty() || !output_path.empty() ||
        !state_path.empty() || write) {
      error = "verify requires a CFDL input and --receipt";
      usage_error = true;
    } else {
      chtholly::CFFIVerification verification;
      if (chtholly::verifyCFFI(config, input_path, keep_temp, verification,
                               error) &&
          chtholly::writeTextFile(receipt_path, verification.receipt, error)) {
        if (output_format == "human")
          std::cout << "verified\t" << input_path << "\nreceipt\t"
                    << receipt_path << '\n';
        else
          std::cout << "{\"event\":\"verified\",\"input\":\""
                    << jsonEscape(input_path) << "\",\"receipt\":\""
                    << jsonEscape(receipt_path) << "\"}\n";
        return 0;
      }
    }
  } else {
    error = "unknown action: " + action;
    usage_error = true;
  }
  std::cerr << "chtholly-cffi: " << error << '\n';
  return usage_error ? 2 : 1;
}
