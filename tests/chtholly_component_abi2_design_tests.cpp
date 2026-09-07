#include "chtholly/Compiler/ComponentABI2Protocol.h"
#include "chtholly/Compiler/ComponentABI2Registry.h"
#include "chtholly/Support/FileSystem.h"
#include "chtholly/component_abi_v2.h"

#include <array>
#include <cstddef>
#include "test_check.h"
#include <filesystem>
#include <string>

using namespace chtholly::compiler;

static_assert(sizeof(chtholly_component_invocation_v2) == 248);
static_assert(alignof(chtholly_component_invocation_v2) == 8);
static_assert(offsetof(chtholly_component_invocation_v2, descriptor_digest) == 12);
static_assert(offsetof(chtholly_component_invocation_v2, payload_size) == 208);
static_assert(offsetof(chtholly_component_invocation_v2, operation) == 232);
static_assert(offsetof(chtholly_component_invocation_v2, transport) == 240);

int main() {
  using E = ComponentAbi2OperationEvent;
  using S = ComponentAbi2OperationState;

  S state = S::Created;
  CHTHOLLY_TEST_CHECK(!componentAbi2IsTerminal(state));
  CHTHOLLY_TEST_CHECK(!componentAbi2Advance(state, E::Commit));
  CHTHOLLY_TEST_CHECK(state == S::Created);
  CHTHOLLY_TEST_CHECK(componentAbi2Advance(state, E::Arm));
  CHTHOLLY_TEST_CHECK(state == S::Armed);
  CHTHOLLY_TEST_CHECK(!componentAbi2Advance(state, E::Arm));
  CHTHOLLY_TEST_CHECK(componentAbi2Advance(state, E::Commit));
  CHTHOLLY_TEST_CHECK(componentAbi2IsTerminal(state));
  CHTHOLLY_TEST_CHECK(!componentAbi2Advance(state, E::Cancel));
  CHTHOLLY_TEST_CHECK(componentAbi2Advance(state, E::Release));
  CHTHOLLY_TEST_CHECK(state == S::Released);
  CHTHOLLY_TEST_CHECK(!componentAbi2Advance(state, E::Release));

  for (const auto terminal : {S::Failed, S::Cancelled}) {
    state = S::Created;
    CHTHOLLY_TEST_CHECK(componentAbi2Advance(
        state, terminal == S::Failed ? E::Fail : E::Cancel));
    CHTHOLLY_TEST_CHECK(componentAbi2IsTerminal(state));
    CHTHOLLY_TEST_CHECK(componentAbi2Advance(state, E::Release));
    CHTHOLLY_TEST_CHECK(state == S::Released);
  }

  // Cancellation/failure before commit is terminal and cannot be replaced by
  // a later commit. This is the precedence required by the design document.
  for (const auto terminal_event : {E::Fail, E::Cancel}) {
    state = S::Created;
    CHTHOLLY_TEST_CHECK(componentAbi2Advance(state, E::Arm));
    CHTHOLLY_TEST_CHECK(componentAbi2Advance(state, terminal_event));
    CHTHOLLY_TEST_CHECK(!componentAbi2Advance(state, E::Commit));
  }

  // Every non-terminal transition is intentionally enumerated so adding a
  // new event or state forces this gate test to be updated.
  constexpr std::array<S, 2> pending = {S::Created, S::Armed};
  for (S pending_state : pending) {
    state = pending_state;
    CHTHOLLY_TEST_CHECK(!componentAbi2Advance(state, E::Release));
    CHTHOLLY_TEST_CHECK(state == pending_state);
  }

  ComponentAbi2Descriptor descriptor;
  descriptor.component_identity = "telemetry.component";
  descriptor.entity_identity = "telemetry.channel.send";
  descriptor.resource_identity = "telemetry.samples";
  descriptor.operation_kind = ComponentAbi2OperationKind::Send;
  descriptor.terminal_cardinality = ComponentAbi2TerminalCardinality::OneShot;
  descriptor.lease_policy = ComponentAbi2LeasePolicy::Exclusive;
  descriptor.payload_type_digest =
      StableFingerprint::fromCanonicalBytes("payload-type");
  descriptor.layout_digest = StableFingerprint::fromCanonicalBytes("layout");
  descriptor.lifecycle_digest = StableFingerprint::fromCanonicalBytes("lifecycle");
  descriptor.contract_digest = StableFingerprint::fromCanonicalBytes("contract");
  descriptor.runtime_abi_digest = StableFingerprint::fromCanonicalBytes("runtime-v2");

  std::string error;
  const std::string encoded = descriptor.encode(error);
  CHTHOLLY_TEST_CHECK(!encoded.empty());
  CHTHOLLY_TEST_CHECK(error.empty());
  ComponentAbi2DescriptorError decode_error = ComponentAbi2DescriptorError::None;
  auto decoded = ComponentAbi2Descriptor::decode(encoded, decode_error, error);
  CHTHOLLY_TEST_CHECK(decoded.has_value());
  CHTHOLLY_TEST_CHECK(decode_error == ComponentAbi2DescriptorError::None);
  CHTHOLLY_TEST_CHECK(decoded->component_identity == descriptor.component_identity);
  CHTHOLLY_TEST_CHECK(decoded->descriptor_digest ==
         componentAbi2DescriptorDigest(*decoded));
  const auto original_digest = decoded->descriptor_digest;

  auto tampered = encoded;
  tampered.back() ^= 1;
  decoded = ComponentAbi2Descriptor::decode(tampered, decode_error, error);
  CHTHOLLY_TEST_CHECK(!decoded.has_value());
  CHTHOLLY_TEST_CHECK(decode_error == ComponentAbi2DescriptorError::DigestMismatch);

  auto wrong_version = encoded;
  wrong_version[7] = 2; // descriptor version, little endian (after 7-byte magic)
  decoded = ComponentAbi2Descriptor::decode(wrong_version, decode_error, error);
  CHTHOLLY_TEST_CHECK(!decoded.has_value());
  CHTHOLLY_TEST_CHECK(decode_error == ComponentAbi2DescriptorError::UnsupportedVersion);

  auto truncated = encoded.substr(0, encoded.size() - 1);
  decoded = ComponentAbi2Descriptor::decode(truncated, decode_error, error);
  CHTHOLLY_TEST_CHECK(!decoded.has_value());
  CHTHOLLY_TEST_CHECK(decode_error == ComponentAbi2DescriptorError::Truncated);

  auto bad_magic = encoded;
  bad_magic[0] = 'X';
  decoded = ComponentAbi2Descriptor::decode(bad_magic, decode_error, error);
  CHTHOLLY_TEST_CHECK(!decoded.has_value());
  CHTHOLLY_TEST_CHECK(decode_error == ComponentAbi2DescriptorError::InvalidMagic);

  auto bad_epoch = encoded;
  // The body starts after magic + version + flags + size + digest.
  bad_epoch[47] = 3;
  decoded = ComponentAbi2Descriptor::decode(bad_epoch, decode_error, error);
  CHTHOLLY_TEST_CHECK(!decoded.has_value());
  CHTHOLLY_TEST_CHECK(decode_error == ComponentAbi2DescriptorError::AbiMismatch);

  descriptor.entity_identity = "bad\nentity";
  CHTHOLLY_TEST_CHECK(descriptor.encode(error).empty());
  CHTHOLLY_TEST_CHECK(!error.empty());
  descriptor.entity_identity = "telemetry.channel.send";

  ComponentAbi2DescriptorRegistry registry;
  CHTHOLLY_TEST_CHECK(registry.registerDescriptor(encoded, decode_error, error) ==
         ComponentAbi2RegistryResult::Inserted);
  CHTHOLLY_TEST_CHECK(registry.registerDescriptor(encoded, decode_error, error) ==
         ComponentAbi2RegistryResult::AlreadyRegistered);
  CHTHOLLY_TEST_CHECK(registry.lookup(descriptor.component_identity,
                         original_digest)
             .has_value());
  CHTHOLLY_TEST_CHECK(registry.retainLease(descriptor.component_identity,
                              original_digest) ==
         ComponentAbi2RegistryResult::Retained);
  CHTHOLLY_TEST_CHECK(registry.erase(descriptor.component_identity,
                        original_digest) ==
         ComponentAbi2RegistryResult::Busy);
  CHTHOLLY_TEST_CHECK(registry.releaseLease(descriptor.component_identity,
                               original_digest) ==
         ComponentAbi2RegistryResult::Released);
  CHTHOLLY_TEST_CHECK(!registry.lookup(descriptor.component_identity,
                          StableFingerprint::fromCanonicalBytes("wrong"))
              .has_value());

  descriptor.entity_identity = "telemetry.channel.receive";
  const auto conflicting = descriptor.encode(error);
  CHTHOLLY_TEST_CHECK(!conflicting.empty());
  CHTHOLLY_TEST_CHECK(registry.registerDescriptor(conflicting, decode_error, error) ==
         ComponentAbi2RegistryResult::Conflict);
  descriptor.entity_identity = "telemetry.channel.send";
  CHTHOLLY_TEST_CHECK(registry.erase(descriptor.component_identity, original_digest) ==
         ComponentAbi2RegistryResult::Erased);

  const auto artifact_path =
      std::filesystem::temp_directory_path() / "chtholly-abi2-replay.bin";
  CHTHOLLY_TEST_CHECK(registry.writeArtifactFile(artifact_path.string(), descriptor, error) ==
         ComponentAbi2RegistryResult::Inserted);
  ComponentAbi2DescriptorRegistry replay_registry;
  CHTHOLLY_TEST_CHECK(replay_registry.replayArtifactFile(artifact_path.string(), decode_error,
                                            error) ==
         ComponentAbi2RegistryResult::Inserted);
  CHTHOLLY_TEST_CHECK(replay_registry.size() == 1);
  auto artifact_bytes = chtholly::readTextFile(artifact_path.string(), error);
  CHTHOLLY_TEST_CHECK(artifact_bytes.has_value());
  artifact_bytes->back() ^= 1;
  CHTHOLLY_TEST_CHECK(chtholly::writeTextFile(artifact_path.string(), *artifact_bytes, error));
  CHTHOLLY_TEST_CHECK(replay_registry.replayArtifactFile(artifact_path.string(), decode_error,
                                            error) ==
         ComponentAbi2RegistryResult::Invalid);
  std::error_code remove_error;
  std::filesystem::remove(artifact_path, remove_error);

  ComponentAbi2LeaseState lease = ComponentAbi2LeaseState::Available;
  using LE = ComponentAbi2LeaseEvent;
  CHTHOLLY_TEST_CHECK(componentAbi2AdvanceLease(lease, LE::Acquire));
  CHTHOLLY_TEST_CHECK(!componentAbi2AdvanceLease(lease, LE::Acquire));
  CHTHOLLY_TEST_CHECK(componentAbi2AdvanceLease(lease, LE::BeginClose));
  CHTHOLLY_TEST_CHECK(!componentAbi2AdvanceLease(lease, LE::Acquire));
  CHTHOLLY_TEST_CHECK(!componentAbi2AdvanceLease(lease, LE::Release));
  CHTHOLLY_TEST_CHECK(componentAbi2AdvanceLease(lease, LE::Quiesce));
  CHTHOLLY_TEST_CHECK(componentAbi2LeaseIsClosed(lease));
  CHTHOLLY_TEST_CHECK(!componentAbi2AdvanceLease(lease, LE::Quiesce));

  ComponentAbi2ResourceLease resource;
  CHTHOLLY_TEST_CHECK(componentAbi2LeaseAcquire(resource));
  CHTHOLLY_TEST_CHECK(resource.active_operations == 1);
  CHTHOLLY_TEST_CHECK(componentAbi2LeaseBeginClose(resource));
  CHTHOLLY_TEST_CHECK(!componentAbi2LeaseQuiesce(resource));
  CHTHOLLY_TEST_CHECK(componentAbi2LeaseRelease(resource));
  CHTHOLLY_TEST_CHECK(resource.active_operations == 0);
  CHTHOLLY_TEST_CHECK(componentAbi2LeaseQuiesce(resource));
  CHTHOLLY_TEST_CHECK(resource.state == ComponentAbi2LeaseState::Closed);
  CHTHOLLY_TEST_CHECK(!componentAbi2LeaseAcquire(resource));
  CHTHOLLY_TEST_CHECK(!componentAbi2LeaseRelease(resource));
  return 0;
}
