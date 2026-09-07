#include "chtholly/Compiler/ComponentABI2Artifact.h"
#include "chtholly/Compiler/ComponentABI2Registry.h"
#include "chtholly/Support/FileSystem.h"
#include "chtholly/next_resource_lease_v2.h"
#include <array>
#include <algorithm>
#include "test_check.h"
#include <filesystem>
#include <string>

using namespace chtholly::compiler;

int main() {
  ComponentAbi2Descriptor provider;
  provider.component_identity = "fixture.provider";
  provider.entity_identity = "fixture.payload.send_receive";
  provider.resource_identity = "fixture.payload.bytes";
  provider.operation_kind = ComponentAbi2OperationKind::Send;
  provider.lease_policy = ComponentAbi2LeasePolicy::Shared;
  provider.payload_type_digest = StableFingerprint::fromCanonicalBytes("u32x4");
  provider.layout_digest = StableFingerprint::fromCanonicalBytes("layout-v1");
  provider.lifecycle_digest = StableFingerprint::fromCanonicalBytes("owned-bytes-v1");
  provider.contract_digest = StableFingerprint::fromCanonicalBytes("send-commit-cancel-v1");
  provider.runtime_abi_digest = StableFingerprint::fromCanonicalBytes("runtime-v2");
  std::string error;
  const auto path = std::filesystem::temp_directory_path() / "chtholly-provider-payload.a2";
  CHTHOLLY_TEST_CHECK(writeComponentAbi2Artifact(path.string(), provider, error));
  ComponentAbi2DescriptorError decode_error{};
  ComponentAbi2DescriptorRegistry consumer;
  CHTHOLLY_TEST_CHECK(consumer.replayArtifactFile(path.string(), decode_error, error) ==
         ComponentAbi2RegistryResult::Inserted);
  auto artifact_bytes = chtholly::readTextFile(path.string(), error);
  CHTHOLLY_TEST_CHECK(artifact_bytes.has_value());
  auto canonical = decodeComponentAbi2Artifact(*artifact_bytes, decode_error, error);
  CHTHOLLY_TEST_CHECK(canonical.has_value());
  auto replayed = consumer.lookup(provider.component_identity,
                                  canonical->descriptor_digest);
  CHTHOLLY_TEST_CHECK(replayed.has_value());
  std::array<uint8_t, 32> digest{};
  std::copy(canonical->descriptor_digest.bytes().begin(),
            canonical->descriptor_digest.bytes().end(), digest.begin());
  chtholly_next_resource_lease_v2 *lease = nullptr;
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_lease_v2_create(2, digest.data(), &lease) == 0);
  chtholly_next_payload_transport_v2 *transport = nullptr;
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_create(lease, digest.data(), 1, 4,
                                                    &transport) == 0);
  std::array<uint8_t, 4> source{9, 8, 7, 6};
  void *send = nullptr;
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_prepare(
             transport, source.data(), source.size(), &send) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_send_commit(send) == 0);
  void *receive = nullptr; const void *payload = nullptr; uint64_t size = 0;
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_receive_acquire(
             transport, &receive, &payload, &size) == 0);
  std::array<uint8_t, 4> destination{};
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_receive_commit(
             receive, destination.data(), destination.size()) == 0);
  CHTHOLLY_TEST_CHECK(destination == source);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_close(transport) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_payload_transport_v2_destroy(transport) == 0);
  CHTHOLLY_TEST_CHECK(chtholly_next_resource_lease_v2_destroy(lease) == 0);
  std::error_code ec; std::filesystem::remove(path, ec);
  return 0;
}
