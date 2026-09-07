#include "chtholly/component_loader_v1.h"
#include "chtholly/Driver/ComponentDeployment.h"
#include "deployment_manifest.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace {

int fail(std::string_view stage, const std::array<char, 512>& diagnostic) {
  std::fprintf(stderr, "telemetry component host: %.*s: %s\n",
               static_cast<int>(stage.size()), stage.data(), diagnostic.data());
  return 1;
}

bool read_contract(const std::filesystem::path& path, std::string& identity,
                   std::array<std::uint8_t, 32>& digest) {
  std::FILE* file = nullptr;
#if defined(_WIN32)
  if (fopen_s(&file, path.string().c_str(), "rb") != 0 || file == nullptr)
    return false;
#else
  file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr)
    return false;
#endif
  std::string text;
  char buffer[256];
  while (std::fgets(buffer, sizeof(buffer), file) != nullptr)
    text += buffer;
  std::fclose(file);
  for (const auto& line : {std::string_view("identity\t"),
                           std::string_view("contract-digest\t")}) {
    const auto begin = text.find(line);
    if (begin == std::string::npos)
      return false;
    const auto end = text.find('\n', begin);
    const auto value = text.substr(begin + line.size(), end - begin - line.size());
    if (line.starts_with("identity")) {
      identity = value;
    } else {
      if (value.size() != digest.size() * 2)
        return false;
      for (std::size_t index = 0; index < digest.size(); ++index) {
        const auto hex = [](char character) -> int {
          if (character >= '0' && character <= '9')
            return character - '0';
          if (character >= 'a' && character <= 'f')
            return character - 'a' + 10;
          if (character >= 'A' && character <= 'F')
            return character - 'A' + 10;
          return -1;
        };
        const int high = hex(value[index * 2]);
        const int low = hex(value[index * 2 + 1]);
        if (high < 0 || low < 0)
          return false;
        digest[index] = static_cast<std::uint8_t>((high << 4) | low);
      }
    }
  }
  return !identity.empty();
}

}  // namespace

int main(int argc, char** argv) {
  std::string deployment_version;
  std::vector<std::string> normalized;
  std::vector<char*> normalized_argv;
  if (argc == 3 && (std::string_view(argv[1]) == "--deployment" ||
                    std::string_view(argv[1]) == "--deployment-root")) {
    TelemetryDeploymentManifest deployment;
    std::string error;
    if (std::string_view(argv[1]) == "--deployment") {
      if (!loadTelemetryDeploymentManifest(argv[2], deployment, error)) {
        std::fprintf(stderr, "telemetry component host: deployment: %s\n",
                     error.c_str());
        return 1;
      }
    } else {
      chtholly::ComponentGenerationInfo generation;
      if (!chtholly::activeComponentGeneration(argv[2], generation, error)) {
        std::fprintf(stderr, "telemetry component host: deployment: %s\n",
                     error.c_str());
        return 1;
      }
      deployment = std::move(generation.manifest);
    }
    std::string contract_identity;
    std::array<std::uint8_t, 32> contract_digest{};
    if (!read_contract(deployment.contract, contract_identity,
                       contract_digest) ||
        contract_identity != deployment.identity ||
        telemetryDigestHex(contract_digest) != deployment.contract_digest) {
      std::fprintf(stderr,
                   "telemetry component host: deployment contract mismatch\n");
      return 1;
    }
    normalized = {argv[0], deployment.library.string(),
                  deployment.contract.string(), deployment.target,
                  deployment.runtime};
    deployment_version = deployment.version;
    normalized_argv.reserve(normalized.size());
    for (auto& value : normalized)
      normalized_argv.push_back(value.data());
    argc = static_cast<int>(normalized_argv.size());
    argv = normalized_argv.data();
  }
  if (argc != 5) {
    std::fprintf(stderr,
                 "usage: telemetry-host LIBRARY CONTRACT TARGET RUNTIME\n");
    return 2;
  }
  std::string identity;
  std::array<std::uint8_t, 32> contract_digest{};
  if (!read_contract(argv[2], identity, contract_digest)) {
    std::fprintf(stderr, "telemetry component host: invalid contract\n");
    return 1;
  }
  std::array<char, 512> diagnostic{};
  std::uint64_t diagnostic_size = 0;
  chtholly_component_requirement_v1 requirement{};
  if (chtholly_component_requirement_init_v1(
          identity.data(), identity.size(), contract_digest.data(), argv[3],
          std::strlen(argv[3]), argv[4], std::strlen(argv[4]), &requirement,
          diagnostic.data(), diagnostic.size(), &diagnostic_size) !=
      CHTHOLLY_COMPONENT_LOADER_OK_V1)
    return fail("requirement", diagnostic);

  chtholly_component_module_v1* module = nullptr;
  if (chtholly_component_load_v1(argv[1], &requirement, &module,
                                 diagnostic.data(), diagnostic.size(),
                                 &diagnostic_size) !=
      CHTHOLLY_COMPONENT_LOADER_OK_V1)
    return fail("load", diagnostic);

  std::uint64_t export_count = 0;
  if (chtholly_component_export_count_v1(module, &export_count) !=
          CHTHOLLY_COMPONENT_LOADER_OK_V1 ||
      export_count == 0)
    return fail("export count", diagnostic);
  chtholly_component_export_info_v1 info{};
  info.struct_size = sizeof(info);
  std::array<char, 256> name{};
  std::uint64_t name_size = 0;
  if (chtholly_component_export_info_v1_get(
          module, 0, &info, name.data(), name.size(), &name_size) !=
          CHTHOLLY_COMPONENT_LOADER_OK_V1 ||
      std::string_view(name.data(), name_size) !=
          "telemetry::component::checksum")
    return fail("export identity", diagnostic);

  const std::array<std::uint8_t, 8> bytes{1, 3, 5, 7, 11, 13, 17, 19};
  chtholly_component_value_v1 argument{};
  argument.struct_size = sizeof(argument);
  argument.kind = CHTHOLLY_COMPONENT_VALUE_BYTES_V1;
  argument.payload.bytes = {bytes.data(), bytes.size()};
  const auto expected_checksum =
      deployment_version == "0.1.1" ? 77ULL : 76ULL;
  chtholly_component_value_v1 result{};
  result.struct_size = sizeof(result);
  if (chtholly_component_invoke_v1(module, info.export_id, &argument, 1,
                                   &result, diagnostic.data(), diagnostic.size(),
                                   &diagnostic_size) !=
          CHTHOLLY_COMPONENT_LOADER_OK_V1 ||
      result.kind != CHTHOLLY_COMPONENT_VALUE_U64_V1 ||
      result.payload.bits != expected_checksum)
    return fail("checksum", diagnostic);

  argument.payload.bytes = {bytes.data(), 0};
  result.struct_size = sizeof(result);
  if (chtholly_component_invoke_v1(module, info.export_id, &argument, 1,
                                   &result, diagnostic.data(), diagnostic.size(),
                                   &diagnostic_size) !=
          CHTHOLLY_COMPONENT_LOADER_OK_V1 ||
      result.payload.bits != (deployment_version == "0.1.1" ? 1ULL : 0ULL))
    return fail("empty checksum", diagnostic);

  const auto close = chtholly_component_close_v1(
      module, diagnostic.data(), diagnostic.size(), &diagnostic_size);
  const auto release = chtholly_component_release_v1(module);
  if (close != CHTHOLLY_COMPONENT_LOADER_OK_V1 ||
      release != CHTHOLLY_COMPONENT_LOADER_OK_V1)
    return fail("close", diagnostic);
  std::printf("{\"schema\":\"chtholly-telemetry-component-v1\","
              "\"deployment_version\":\"%s\","
              "\"samples\":8,\"checksum\":%llu,\"empty\":0}\n",
              deployment_version.c_str(),
              static_cast<unsigned long long>(expected_checksum));
  return 0;
}
