#include "chtholly/component_loader_v1.h"
#include "chtholly/Driver/DeploymentManifest.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Deployment {
  std::filesystem::path source;
  std::string identity;
  std::array<std::uint8_t, 32> contract{};
  std::int32_t increment = 0;
};

struct Loaded {
  chtholly_component_module_v1 *module = nullptr;
  std::array<std::uint8_t, 32> process{};
  std::array<std::uint8_t, 32> hold{};
  std::filesystem::path generation_path;
};

struct Metrics {
  std::uint64_t cycles = 0;
  std::atomic<std::uint64_t> calls_alpha = 0;
  std::atomic<std::uint64_t> calls_beta = 0;
  std::uint64_t closing_rejections = 0;
  std::uint64_t cross_component_rejections = 0;
  std::uint64_t max_unload_wait_us = 0;
  std::uint64_t cleanup_failures = 0;
};

bool hexDigest(std::string_view text, std::array<std::uint8_t, 32> &output) {
  if (text.size() != 64)
    return false;
  const auto digit = [](char value) -> int {
    if (value >= '0' && value <= '9')
      return value - '0';
    if (value >= 'a' && value <= 'f')
      return value - 'a' + 10;
    return -1;
  };
  for (std::size_t index = 0; index < output.size(); ++index) {
    const auto high = digit(text[index * 2]);
    const auto low = digit(text[index * 2 + 1]);
    if (high < 0 || low < 0)
      return false;
    output[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

std::uint64_t resourceCount() {
#if defined(_WIN32)
  DWORD count = 0;
  return GetProcessHandleCount(GetCurrentProcess(), &count) ? count : 0;
#else
  std::uint64_t count = 0;
  if (auto *directory = opendir("/proc/self/fd")) {
    while (readdir(directory))
      ++count;
    closedir(directory);
    if (count >= 2)
      count -= 2;
  }
  return count;
#endif
}

bool mapped(const std::filesystem::path &path) {
#if defined(_WIN32)
  (void)path;
  return false;
#else
  std::ifstream input("/proc/self/maps");
  const auto expected = path.string();
  std::string line;
  while (std::getline(input, line))
    if (line.find(expected) != std::string::npos)
      return true;
  return false;
#endif
}

bool discover(Loaded &loaded) {
  std::uint64_t count = 0;
  if (chtholly_component_export_count_v1(loaded.module, &count) !=
      CHTHOLLY_COMPONENT_LOADER_OK_V1)
    return false;
  for (std::uint64_t index = 0; index < count; ++index) {
    chtholly_component_export_info_v1 info{};
    info.struct_size = sizeof(info);
    std::array<char, 128> name{};
    std::uint64_t size = 0;
    if (chtholly_component_export_info_v1_get(
            loaded.module, index, &info, name.data(), name.size(), &size) !=
        CHTHOLLY_COMPONENT_LOADER_OK_V1)
      return false;
    const std::string_view value(name.data(), size);
    if (value == "plugin::process")
      std::ranges::copy(info.export_id, loaded.process.begin());
    else if (value == "plugin::hold")
      std::ranges::copy(info.export_id, loaded.hold.begin());
  }
  return std::ranges::any_of(loaded.process,
                             [](auto value) { return value; }) &&
         std::ranges::any_of(loaded.hold, [](auto value) { return value; });
}

bool load(const Deployment &deployment, const std::filesystem::path &copy,
          std::string_view target, std::string_view runtime, Loaded &loaded) {
  std::error_code error;
  std::filesystem::copy_file(deployment.source, copy,
                             std::filesystem::copy_options::overwrite_existing,
                             error);
  if (error)
    return false;
  chtholly_component_requirement_v1 requirement{};
  std::array<char, 256> diagnostic{};
  std::uint64_t diagnostic_size = 0;
  if (chtholly_component_requirement_init_v1(
          deployment.identity.data(), deployment.identity.size(),
          deployment.contract.data(), target.data(), target.size(),
          runtime.data(), runtime.size(), &requirement, diagnostic.data(),
          diagnostic.size(),
          &diagnostic_size) != CHTHOLLY_COMPONENT_LOADER_OK_V1)
    return false;
  if (chtholly_component_load_v1(copy.string().c_str(), &requirement,
                                 &loaded.module, diagnostic.data(),
                                 diagnostic.size(), &diagnostic_size) !=
      CHTHOLLY_COMPONENT_LOADER_OK_V1)
    return false;
  loaded.generation_path = copy;
  return discover(loaded);
}

std::uint32_t process(Loaded &loaded, std::int32_t value,
                      std::int32_t expected) {
  chtholly_component_value_v1 argument{};
  argument.struct_size = sizeof(argument);
  argument.kind = CHTHOLLY_COMPONENT_VALUE_I32_V1;
  argument.payload.bits = static_cast<std::uint32_t>(value);
  chtholly_component_value_v1 result{};
  result.struct_size = sizeof(result);
  const auto status =
      chtholly_component_invoke_v1(loaded.module, loaded.process.data(),
                                   &argument, 1, &result, nullptr, 0, nullptr);
  if (status == CHTHOLLY_COMPONENT_LOADER_OK_V1 &&
      (result.kind != CHTHOLLY_COMPONENT_VALUE_I32_V1 ||
       static_cast<std::int32_t>(result.payload.bits) != expected))
    return CHTHOLLY_COMPONENT_LOADER_INVOKE_FAILED_V1;
  return status;
}

std::uint32_t hold(Loaded &loaded, std::uint64_t iterations) {
  chtholly_component_value_v1 argument{};
  argument.struct_size = sizeof(argument);
  argument.kind = CHTHOLLY_COMPONENT_VALUE_U64_V1;
  argument.payload.bits = iterations;
  chtholly_component_value_v1 result{};
  result.struct_size = sizeof(result);
  const auto status =
      chtholly_component_invoke_v1(loaded.module, loaded.hold.data(), &argument,
                                   1, &result, nullptr, 0, nullptr);
  if (status == CHTHOLLY_COMPONENT_LOADER_OK_V1 &&
      result.payload.bits != iterations)
    return CHTHOLLY_COMPONENT_LOADER_INVOKE_FAILED_V1;
  return status;
}

bool releaseAndCleanup(Loaded &loaded, Metrics &metrics) {
  if (process(loaded, 1, 1) != CHTHOLLY_COMPONENT_LOADER_CLOSING_V1)
    return false;
  ++metrics.closing_rejections;
  if (chtholly_component_release_v1(loaded.module) !=
      CHTHOLLY_COMPONENT_LOADER_OK_V1)
    return false;
  loaded.module = nullptr;
  if (mapped(loaded.generation_path))
    return false;
  std::error_code error;
  std::filesystem::remove(loaded.generation_path, error);
  if (error || std::filesystem::exists(loaded.generation_path)) {
    ++metrics.cleanup_failures;
    return false;
  }
  return true;
}

bool closeAndRelease(Loaded &loaded, Metrics &metrics) {
  const auto begin = Clock::now();
  if (chtholly_component_close_v1(loaded.module, nullptr, 0, nullptr) !=
      CHTHOLLY_COMPONENT_LOADER_OK_V1)
    return false;
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                           Clock::now() - begin)
                           .count();
  metrics.max_unload_wait_us =
      std::max(metrics.max_unload_wait_us, static_cast<std::uint64_t>(elapsed));
  return releaseAndCleanup(loaded, metrics);
}

} // namespace

int main(int argc, char **argv) {
  const auto fail = [](std::string_view stage) {
    std::cerr << "component-host failure: " << stage << '\n';
    return 1;
  };
  if (argc == 3 && std::string_view(argv[1]) == "--deployment") {
    chtholly::DeploymentManifest manifest; std::string error;
    if (!chtholly::loadDeploymentManifest(argv[2], manifest, error) ||
        !chtholly::validateDeploymentFiles(manifest, error)) {
      std::cerr << "component-host deployment: " << error << '\n';
      return 1;
    }
    std::cout << "deployment\t" << manifest.identity << "\nversion\t"
              << manifest.version << "\ndigest\t" << manifest.contract_digest << '\n';
    return 0;
  }
  if (argc != 12 && argc != 14) {
    std::cerr << "usage: component-host <duration-seconds> <threads> "
                 "<generation-root> <target> <runtime-abi> "
                 "<alpha-path> <alpha-identity> <alpha-contract> "
                 "<beta-path> <beta-identity> <beta-contract> "
                 "[<alpha-process-increment> <beta-process-increment>]\n";
    return 2;
  }
  const auto duration = std::stoull(argv[1]);
  const auto thread_count = std::stoull(argv[2]);
  if (duration == 0 || thread_count < 2)
    return 2;
  const std::filesystem::path generation_root = argv[3];
  const std::string target = argv[4];
  const std::string runtime = argv[5];
  const auto alpha_increment = argc == 14 ? std::stoi(argv[12]) : 1;
  const auto beta_increment = argc == 14 ? std::stoi(argv[13]) : 2;
  Deployment alpha{std::filesystem::absolute(argv[6]), argv[7], {},
                   alpha_increment};
  Deployment beta{std::filesystem::absolute(argv[9]), argv[10], {},
                  beta_increment};
  if (!hexDigest(argv[8], alpha.contract) ||
      !hexDigest(argv[11], beta.contract))
    return 2;
  std::error_code file_error;
  std::filesystem::create_directories(generation_root, file_error);
  if (file_error)
    return 1;
  std::uint64_t resource_before = 0;
  Metrics metrics;
  const auto deadline = Clock::now() + std::chrono::seconds(duration);
  const auto extension = alpha.source.extension();
  while (Clock::now() < deadline || metrics.cycles < 4) {
    const auto cycle_root = generation_root / std::to_string(metrics.cycles);
    std::filesystem::create_directories(cycle_root, file_error);
    if (file_error)
      return fail("generation directory create");
    Loaded loaded_alpha, loaded_beta;
    if (!load(alpha, cycle_root / ("alpha" + extension.string()), target,
              runtime, loaded_alpha) ||
        !load(beta, cycle_root / ("beta" + extension.string()), target, runtime,
              loaded_beta))
      return fail("load or discover");
    if (loaded_alpha.process == loaded_beta.process)
      return fail("identity-scoped export IDs collided");
    chtholly_component_value_v1 cross_argument{};
    cross_argument.struct_size = sizeof(cross_argument);
    cross_argument.kind = CHTHOLLY_COMPONENT_VALUE_I32_V1;
    chtholly_component_value_v1 cross_result{};
    cross_result.struct_size = sizeof(cross_result);
    if (chtholly_component_invoke_v1(
            loaded_beta.module, loaded_alpha.process.data(), &cross_argument, 1,
            &cross_result, nullptr, 0,
            nullptr) != CHTHOLLY_COMPONENT_LOADER_EXPORT_NOT_FOUND_V1)
      return fail("cross-component export ID was accepted");
    ++metrics.cross_component_rejections;

    std::atomic<bool> stop_alpha = false, stop_beta = false;
    std::atomic<bool> failed = false;
    std::vector<std::thread> alpha_workers, beta_workers;
    const auto workers_per_component =
        std::max<std::uint64_t>(1, thread_count / 2);
    for (std::uint64_t index = 0; index < workers_per_component; ++index) {
      alpha_workers.emplace_back([&] {
        std::int32_t value = 0;
        while (!stop_alpha) {
          if (process(loaded_alpha, value, value + alpha.increment) !=
              CHTHOLLY_COMPONENT_LOADER_OK_V1) {
            failed = true;
            break;
          }
          value = value == 1000000 ? 0 : value + 1;
          ++metrics.calls_alpha;
        }
      });
      beta_workers.emplace_back([&] {
        std::int32_t value = 0;
        while (!stop_beta) {
          if (process(loaded_beta, value, value + beta.increment) !=
              CHTHOLLY_COMPONENT_LOADER_OK_V1) {
            failed = true;
            break;
          }
          value = value == 1000000 ? 0 : value + 1;
          ++metrics.calls_beta;
        }
      });
    }
    std::atomic<bool> hold_started = false;
    std::atomic<std::uint32_t> hold_status = UINT32_MAX;
    std::thread holder([&] {
      hold_started = true;
      hold_status = hold(loaded_alpha, 100000000);
    });
    while (!hold_started)
      std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stop_alpha = true;
    for (auto &worker : alpha_workers)
      worker.join();
    std::atomic<std::uint32_t> close_status = UINT32_MAX;
    std::atomic<std::uint64_t> close_wait_us = 0;
    std::thread closer([&] {
      const auto begin = Clock::now();
      close_status =
          chtholly_component_close_v1(loaded_alpha.module, nullptr, 0, nullptr);
      close_wait_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          Clock::now() - begin)
                          .count();
    });
    const auto closing_deadline = Clock::now() + std::chrono::seconds(2);
    bool saw_closing = false;
    while (Clock::now() < closing_deadline) {
      const auto status = process(loaded_alpha, 1, 2);
      if (status == CHTHOLLY_COMPONENT_LOADER_CLOSING_V1) {
        saw_closing = true;
        ++metrics.closing_rejections;
        break;
      }
      if (status != CHTHOLLY_COMPONENT_LOADER_OK_V1) {
        failed = true;
        break;
      }
      std::this_thread::yield();
    }
    holder.join();
    closer.join();
    metrics.max_unload_wait_us =
        std::max(metrics.max_unload_wait_us, close_wait_us.load());
    if (!saw_closing || close_status != CHTHOLLY_COMPONENT_LOADER_OK_V1 ||
        chtholly_component_release_v1(loaded_alpha.module) !=
            CHTHOLLY_COMPONENT_LOADER_OK_V1 ||
        mapped(loaded_alpha.generation_path))
      return fail("alpha close wait or release");
    loaded_alpha.module = nullptr;
    std::filesystem::remove(loaded_alpha.generation_path, file_error);
    if (file_error || std::filesystem::exists(loaded_alpha.generation_path))
      return fail("alpha file cleanup");
    if (hold_status != CHTHOLLY_COMPONENT_LOADER_OK_V1 || failed)
      return fail("alpha worker or hold call");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    stop_beta = true;
    for (auto &worker : beta_workers)
      worker.join();
    if (!closeAndRelease(loaded_beta, metrics) || failed)
      return fail("beta worker, close, release, or file cleanup");
    std::filesystem::remove(cycle_root, file_error);
    if (file_error)
      return fail("generation directory cleanup");
    ++metrics.cycles;
    if (metrics.cycles == 1)
      resource_before = resourceCount();
  }
  const auto resource_after = resourceCount();
  if (metrics.calls_alpha == 0 || metrics.calls_beta == 0 ||
      metrics.cross_component_rejections != metrics.cycles ||
      metrics.closing_rejections != metrics.cycles * 2 ||
      metrics.cleanup_failures != 0 || resource_after > resource_before + 4) {
    std::cerr << "cycles=" << metrics.cycles
              << " calls_alpha=" << metrics.calls_alpha
              << " calls_beta=" << metrics.calls_beta
              << " closing=" << metrics.closing_rejections
              << " cross=" << metrics.cross_component_rejections
              << " cleanup=" << metrics.cleanup_failures
              << " resource_before=" << resource_before
              << " resource_after=" << resource_after << '\n';
    return fail("final metrics invariant");
  }
  std::cout << "{\"schema\":\"chtholly-component-host-soak-v1\","
            << "\"cycles\":" << metrics.cycles << ','
            << "\"calls_alpha\":" << metrics.calls_alpha << ','
            << "\"calls_beta\":" << metrics.calls_beta << ','
            << "\"closing_rejections\":" << metrics.closing_rejections << ','
            << "\"cross_component_rejections\":"
            << metrics.cross_component_rejections << ','
            << "\"max_unload_wait_us\":" << metrics.max_unload_wait_us << ','
            << "\"resource_before\":" << resource_before << ','
            << "\"resource_after\":" << resource_after << ','
            << "\"cleanup_failures\":" << metrics.cleanup_failures << "}\n";
  std::filesystem::remove(generation_root, file_error);
  return file_error ? 1 : 0;
}
