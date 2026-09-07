#include "chtholly/component_loader_v2.h"
#include "chtholly/Compiler/ComponentABI2Artifact.h"
#include "chtholly/Support/FileSystem.h"

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

static void check(bool condition, const char *expression, int line) {
  if (!condition) {
    std::fprintf(stderr, "ABI-2 loader check failed at line %d: %s\n", line,
                 expression);
    std::abort();
  }
}
#define CHECK(condition) check((condition), #condition, __LINE__)

static void fixtureMode(const char *mode) {
#if defined(_WIN32)
  CHECK(_putenv_s("CHTHOLLY_ABI2_FIXTURE_MODE", mode) == 0);
#else
  CHECK(setenv("CHTHOLLY_ABI2_FIXTURE_MODE", mode, 1) == 0);
#endif
}

int main(int argc, char **argv) {
  CHECK(argc == 3);
  std::array<char, 256> diagnostic{};
  std::uint64_t diagnostic_size = 0;
  chtholly_component_module_v2 *module = nullptr;
  CHECK(chtholly_component_load_v2(
            "relative-component", &module, diagnostic.data(), diagnostic.size(),
            &diagnostic_size) == CHTHOLLY_COMPONENT_LOADER_V2_INVALID_ARGUMENT);
  CHECK(module == nullptr);
  CHECK(chtholly_component_load_v2(
            argv[1], &module, diagnostic.data(), diagnostic.size(),
            &diagnostic_size) == CHTHOLLY_COMPONENT_LOADER_V2_QUERY_MISSING);
  CHECK(module == nullptr);
  std::string error;
  chtholly::compiler::ComponentAbi2Descriptor descriptor;
  descriptor.component_identity = "abi2.pending.fixture";
  descriptor.entity_identity = "abi2.pending.invoke";
  descriptor.resource_identity = "abi2.pending.resource";
  descriptor.operation_kind = chtholly::compiler::ComponentAbi2OperationKind::Send;
  descriptor.terminal_cardinality = chtholly::compiler::ComponentAbi2TerminalCardinality::OneShot;
  descriptor.lease_policy = chtholly::compiler::ComponentAbi2LeasePolicy::Shared;
  descriptor.payload_type_digest = chtholly::compiler::StableFingerprint::fromCanonicalBytes("payload");
  descriptor.layout_digest = chtholly::compiler::StableFingerprint::fromCanonicalBytes("layout");
  descriptor.lifecycle_digest = chtholly::compiler::StableFingerprint::fromCanonicalBytes("lifecycle");
  descriptor.contract_digest = chtholly::compiler::StableFingerprint::fromCanonicalBytes("contract");
  descriptor.runtime_abi_digest = chtholly::compiler::StableFingerprint::fromCanonicalBytes("runtime-v2");
  const auto artifact = std::filesystem::temp_directory_path() / "abi2-loader.artifact";
  CHECK(chtholly::compiler::writeComponentAbi2Artifact(artifact.string(), descriptor, error));
  chtholly::compiler::ComponentAbi2DescriptorError descriptor_error{};
  auto canonical_descriptor = chtholly::compiler::ComponentAbi2Descriptor::decode(
      descriptor.encode(error), descriptor_error, error);
  CHECK(canonical_descriptor.has_value());
  const auto valid_status = chtholly_component_load_v2_from_artifact(
      argv[2], artifact.string().c_str(), &module, diagnostic.data(), diagnostic.size(), &diagnostic_size);
  CHECK(valid_status == CHTHOLLY_COMPONENT_LOADER_V2_OK);
  CHECK(module != nullptr);
  std::array<uint8_t, 32> descriptor_digest{};
  std::copy(canonical_descriptor->descriptor_digest.bytes().begin(),
            canonical_descriptor->descriptor_digest.bytes().end(), descriptor_digest.begin());
  chtholly_next_resource_lease_v2 *payload_lease = nullptr;
  CHECK(chtholly_next_resource_lease_v2_create(
            2, descriptor_digest.data(), &payload_lease) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
  chtholly_next_payload_transport_v2 *payload_transport = nullptr;
  CHECK(chtholly_next_payload_transport_v2_create(
            payload_lease, descriptor_digest.data(), 1, 4,
            &payload_transport) == CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
  chtholly_next_resource_operation_v2 *payload_operation = nullptr;
  const auto payload_invoke_status = chtholly_component_invoke_payload_v2(
            module, 1, payload_transport, &payload_operation, diagnostic.data(),
            diagnostic.size(), &diagnostic_size);
  if (payload_invoke_status != CHTHOLLY_COMPONENT_LOADER_V2_OK)
    std::fprintf(stderr, "payload invoke status=%u diag=%s\n",
                 payload_invoke_status, diagnostic.data());
  CHECK(payload_invoke_status == CHTHOLLY_COMPONENT_LOADER_V2_OK);
  CHECK(chtholly_next_payload_transport_v2_size(payload_transport) == 1);
  void *receive_token = nullptr;
  const void *received = nullptr;
  uint64_t received_size = 0;
  CHECK(chtholly_next_payload_transport_v2_receive_acquire(
            payload_transport, &receive_token, &received, &received_size) == 0);
  std::array<uint8_t, 4> received_bytes{};
  CHECK(chtholly_next_payload_transport_v2_receive_commit(
            receive_token, received_bytes.data(), received_bytes.size()) == 0);
  const std::array<uint8_t, 4> expected_bytes{4, 3, 2, 1};
  CHECK(received_bytes == expected_bytes);
  CHECK(chtholly_next_resource_operation_v2_destroy(payload_operation) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
  CHECK(chtholly_next_payload_transport_v2_close(payload_transport) == 0);
  CHECK(chtholly_next_payload_transport_v2_destroy(payload_transport) == 0);
  CHECK(chtholly_next_resource_lease_v2_destroy(payload_lease) == 0);
  fixtureMode("pending");
  chtholly_next_resource_operation_v2 *operation = nullptr;
  CHECK(chtholly_component_invoke_v2(
            module, 1, &operation, diagnostic.data(), diagnostic.size(),
            &diagnostic_size) == CHTHOLLY_COMPONENT_LOADER_V2_OK);
  CHECK(operation != nullptr);
  CHECK(chtholly_component_close_v2(module, diagnostic.data(), diagnostic.size(),
                                     &diagnostic_size) ==
        CHTHOLLY_COMPONENT_LOADER_V2_NOT_READY);
  CHECK(chtholly_next_resource_operation_v2_complete(
            operation, CHTHOLLY_NEXT_RESOURCE_OPERATION_COMMITTED) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
  CHECK(chtholly_next_resource_operation_v2_destroy(operation) ==
        CHTHOLLY_NEXT_RESOURCE_LEASE_OK);
  CHECK(chtholly_component_unload_v2(module, diagnostic.data(), diagnostic.size(),
                                      &diagnostic_size) ==
        CHTHOLLY_COMPONENT_LOADER_V2_OK);
  fixtureMode("background");
  module = nullptr;
  CHECK(chtholly_component_load_v2(argv[2], &module, nullptr, 0, nullptr) == 0);
  CHECK(chtholly_component_invoke_v2(module, 1, &operation, nullptr, 0, nullptr) == 0);
  CHECK(chtholly_component_retain_owner_v2(module) == 0);
  CHECK(chtholly_component_close_v2(module, nullptr, 0, nullptr) == CHTHOLLY_COMPONENT_LOADER_V2_NOT_READY);
  CHECK(chtholly_next_resource_operation_v2_state(operation) == CHTHOLLY_NEXT_RESOURCE_OPERATION_CANCELLED);
  CHECK(chtholly_next_resource_operation_v2_destroy(operation) == 0);
  chtholly_component_release_owner_v2(module);
  CHECK(chtholly_component_unload_v2(module, nullptr, 0, nullptr) == 0);
  fixtureMode("close-fail-once");
  CHECK(chtholly_component_load_v2(argv[2], &module, nullptr, 0, nullptr) == 0);
  CHECK(chtholly_component_close_v2(module, nullptr, 0, nullptr) == CHTHOLLY_COMPONENT_LOADER_V2_PLATFORM_ERROR);
  CHECK(chtholly_component_unload_v2(module, nullptr, 0, nullptr) == 0);
  fixtureMode("commit");
  std::error_code artifact_error;
  std::filesystem::remove(artifact, artifact_error);
  return 0;
}
