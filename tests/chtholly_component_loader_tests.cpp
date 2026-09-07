#include "chtholly/Driver/ArtifactCompatibility.h"
#include "chtholly/Compiler/ComponentABI.h"
#include "chtholly/Support/FileSystem.h"
#include "chtholly/component_loader_v1.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

extern "C" int chtholly_component_c_header_probe(void);

void checkAt(bool condition, const char *expression, int line) {
  if (!condition) {
    std::fprintf(stderr, "CHECK failed at line %d: %s\n", line, expression);
    std::abort();
  }
}
#define CHECK(condition) checkAt((condition), #condition, __LINE__)

} // namespace

int main(int argc, char **argv) {
  CHECK(chtholly_component_c_header_probe() == 1);
  CHECK(argc == 4);
  std::string error;
  const auto encoded = chtholly::readTextFile(argv[2], error);
  CHECK(encoded.has_value());
  const auto contract =
      chtholly::compiler::ComponentContractArtifact::decode(*encoded, error);
  CHECK(contract.has_value());
  CHECK(!chtholly::compiler::ComponentContractArtifact::decode(
      *encoded + "unknown\trecord\n", error));

  std::array<std::uint8_t, 32> contract_digest{};
  std::ranges::copy(contract->contract_digest.bytes(), contract_digest.begin());
  chtholly_component_requirement_v1 requirement{};
  std::array<char, 512> diagnostic{};
  std::uint64_t diagnostic_size = 0;
  CHECK(chtholly_component_requirement_init_v1(
            contract->identity.data(), contract->identity.size(),
            contract_digest.data(), argv[3], std::strlen(argv[3]),
            chtholly::HostedRuntimeAbiVersion.data(),
            chtholly::HostedRuntimeAbiVersion.size(), &requirement,
            diagnostic.data(), diagnostic.size(),
            &diagnostic_size) == CHTHOLLY_COMPONENT_LOADER_OK_V1);
  std::array<std::uint8_t, 32> zero_digest{};
  CHECK(chtholly_component_requirement_init_v1(
            contract->identity.data(), contract->identity.size(),
            zero_digest.data(), argv[3], std::strlen(argv[3]), "v1", 2,
            &requirement, diagnostic.data(), diagnostic.size(),
            &diagnostic_size) == CHTHOLLY_COMPONENT_LOADER_INVALID_ARGUMENT_V1);
  const char invalid_utf8[] = {static_cast<char>(0xc0),
                               static_cast<char>(0x80)};
  CHECK(chtholly_component_requirement_init_v1(
            invalid_utf8, sizeof(invalid_utf8), contract_digest.data(), argv[3],
            std::strlen(argv[3]), "v1", 2, &requirement, diagnostic.data(),
            diagnostic.size(),
            &diagnostic_size) == CHTHOLLY_COMPONENT_LOADER_INVALID_ARGUMENT_V1);
  CHECK(chtholly_component_requirement_init_v1(
            contract->identity.data(), contract->identity.size(),
            contract_digest.data(), argv[3], std::strlen(argv[3]), "v1", 2,
            &requirement, diagnostic.data(), diagnostic.size(),
            &diagnostic_size) == CHTHOLLY_COMPONENT_LOADER_OK_V1);
  chtholly_component_module_v1 *module = nullptr;
  CHECK(chtholly_component_load_v1(argv[1], &requirement, &module,
                                   diagnostic.data(), diagnostic.size(),
                                   &diagnostic_size) ==
        CHTHOLLY_COMPONENT_LOADER_OK_V1);
  CHECK(module != nullptr);
  chtholly_component_module_v1 *relative_rejected = nullptr;
  CHECK(chtholly_component_load_v1("relative-component", &requirement,
                                   &relative_rejected, diagnostic.data(),
                                   diagnostic.size(), &diagnostic_size) ==
        CHTHOLLY_COMPONENT_LOADER_INVALID_ARGUMENT_V1);
  chtholly_component_requirement_v1 scratch_requirement{};
  const char embedded_nul[] = {'i', 'd', '\0', 'x'};
  CHECK(chtholly_component_requirement_init_v1(
            embedded_nul, sizeof(embedded_nul), contract_digest.data(), argv[3],
            std::strlen(argv[3]), "v1", 2, &scratch_requirement,
            diagnostic.data(), diagnostic.size(),
            &diagnostic_size) == CHTHOLLY_COMPONENT_LOADER_INVALID_ARGUMENT_V1);
  std::array<char, 1> tiny_diagnostic{'x'};
  CHECK(chtholly_component_requirement_init_v1(
            contract->identity.data(), contract->identity.size(),
            contract_digest.data(), "unsupported", 11, "v1", 2,
            &scratch_requirement, tiny_diagnostic.data(),
            tiny_diagnostic.size(),
            &diagnostic_size) == CHTHOLLY_COMPONENT_LOADER_INVALID_ARGUMENT_V1);
  CHECK(tiny_diagnostic[0] == 0);
  CHECK(diagnostic_size != 0);
  std::uint64_t count = 0;
  CHECK(chtholly_component_export_count_v1(module, &count) ==
        CHTHOLLY_COMPONENT_LOADER_OK_V1);
  CHECK(count == 5);
  std::array<std::uint8_t, 32> spin_id{};

  for (std::uint64_t index = 0; index < count; ++index) {
    chtholly_component_export_info_v1 info{};
    info.struct_size = sizeof(info);
    std::array<char, 256> name{};
    std::uint64_t name_size = 0;
    CHECK(chtholly_component_export_info_v1_get(
              module, index, &info, name.data(), name.size(), &name_size) ==
          CHTHOLLY_COMPONENT_LOADER_OK_V1);
    chtholly_component_export_info_v1 small_info{};
    small_info.struct_size = sizeof(small_info);
    std::array<char, 1> small_name{};
    CHECK(chtholly_component_export_info_v1_get(
              module, index, &small_info, small_name.data(), small_name.size(),
              &name_size) == CHTHOLLY_COMPONENT_LOADER_BUFFER_TOO_SMALL_V1);
    const std::string canonical_name(name.data(), name_size);
    chtholly_component_value_v1 result{};
    result.struct_size = sizeof(result);
    if (canonical_name == "component::math::add") {
      chtholly_component_value_v1 arguments[2]{};
      for (auto &argument : arguments)
        argument.struct_size = sizeof(argument);
      arguments[0].kind = CHTHOLLY_COMPONENT_VALUE_I32_V1;
      arguments[0].payload.bits = 20;
      arguments[1].kind = CHTHOLLY_COMPONENT_VALUE_I32_V1;
      arguments[1].payload.bits = 22;
      CHECK(chtholly_component_invoke_v1(module, info.export_id, arguments, 2,
                                         &result, diagnostic.data(),
                                         diagnostic.size(), &diagnostic_size) ==
            CHTHOLLY_COMPONENT_LOADER_OK_V1);
      CHECK(result.kind == CHTHOLLY_COMPONENT_VALUE_I32_V1);
      CHECK(result.payload.bits == 42);
      arguments[0].kind = CHTHOLLY_COMPONENT_VALUE_U32_V1;
      result.struct_size = sizeof(result);
      CHECK(chtholly_component_invoke_v1(module, info.export_id, arguments, 2,
                                         &result, diagnostic.data(),
                                         diagnostic.size(), &diagnostic_size) ==
            CHTHOLLY_COMPONENT_LOADER_INVOKE_FAILED_V1);
    } else if (canonical_name == "component::bytes::probe") {
      const std::uint8_t bytes[] = {1, 2, 3, 4};
      chtholly_component_value_v1 argument{};
      argument.struct_size = sizeof(argument);
      argument.kind = CHTHOLLY_COMPONENT_VALUE_BYTES_V1;
      argument.payload.bytes = {bytes, sizeof(bytes)};
      CHECK(chtholly_component_invoke_v1(module, info.export_id, &argument, 1,
                                         &result, diagnostic.data(),
                                         diagnostic.size(), &diagnostic_size) ==
            CHTHOLLY_COMPONENT_LOADER_OK_V1);
      CHECK(result.kind == CHTHOLLY_COMPONENT_VALUE_U64_V1);
      CHECK(result.payload.bits == 7);
    } else if (canonical_name == "component::math::echo_bool") {
      chtholly_component_value_v1 argument{};
      argument.struct_size = sizeof(argument);
      argument.kind = CHTHOLLY_COMPONENT_VALUE_BOOL_V1;
      argument.payload.bits = 1;
      CHECK(chtholly_component_invoke_v1(module, info.export_id, &argument, 1,
                                         &result, diagnostic.data(),
                                         diagnostic.size(), &diagnostic_size) ==
            CHTHOLLY_COMPONENT_LOADER_OK_V1);
      CHECK(result.kind == CHTHOLLY_COMPONENT_VALUE_BOOL_V1);
      CHECK(result.payload.bits == 1);
    } else if (canonical_name == "component::math::echo_f64") {
      constexpr double expected = 3.25;
      chtholly_component_value_v1 argument{};
      argument.struct_size = sizeof(argument);
      argument.kind = CHTHOLLY_COMPONENT_VALUE_F64_V1;
      std::memcpy(&argument.payload.bits, &expected, sizeof(expected));
      CHECK(chtholly_component_invoke_v1(module, info.export_id, &argument, 1,
                                         &result, diagnostic.data(),
                                         diagnostic.size(), &diagnostic_size) ==
            CHTHOLLY_COMPONENT_LOADER_OK_V1);
      double actual = 0;
      std::memcpy(&actual, &result.payload.bits, sizeof(actual));
      CHECK(result.kind == CHTHOLLY_COMPONENT_VALUE_F64_V1);
      CHECK(actual == expected);
    } else if (canonical_name == "component::math::spin") {
      std::ranges::copy(info.export_id, spin_id.begin());
    } else {
      CHECK(false);
    }
  }

  auto wrong = requirement;
  wrong.contract_digest[0] ^= 1;
  chtholly_component_module_v1 *rejected = nullptr;
  CHECK(chtholly_component_load_v1(argv[1], &wrong, &rejected,
                                   diagnostic.data(), diagnostic.size(),
                                   &diagnostic_size) ==
        CHTHOLLY_COMPONENT_LOADER_CONTRACT_MISMATCH_V1);
  CHECK(rejected == nullptr);
  CHECK(std::ranges::any_of(spin_id, [](auto value) { return value != 0; }));
  std::atomic<bool> started = false;
  std::atomic<std::uint32_t> invoke_status = UINT32_MAX;
  std::thread worker([&] {
    chtholly_component_value_v1 argument{};
    argument.struct_size = sizeof(argument);
    argument.kind = CHTHOLLY_COMPONENT_VALUE_U64_V1;
    argument.payload.bits = 100000000;
    chtholly_component_value_v1 result{};
    result.struct_size = sizeof(result);
    std::array<char, 128> worker_diagnostic{};
    std::uint64_t worker_diagnostic_size = 0;
    started = true;
    invoke_status = chtholly_component_invoke_v1(
        module, spin_id.data(), &argument, 1, &result, worker_diagnostic.data(),
        worker_diagnostic.size(), &worker_diagnostic_size);
    CHECK(result.payload.bits == 100000000);
  });
  while (!started)
    std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(chtholly_component_unload_v1(module, diagnostic.data(),
                                     diagnostic.size(), &diagnostic_size) ==
        CHTHOLLY_COMPONENT_LOADER_OK_V1);
  worker.join();
  CHECK(invoke_status == CHTHOLLY_COMPONENT_LOADER_OK_V1);
  return 0;
}
