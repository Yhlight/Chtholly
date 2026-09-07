#pragma once

#include "chtholly/Compiler/PublicInterface.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace chtholly::compiler {

inline constexpr std::uint32_t ComponentAbi2Epoch = 2;
inline constexpr std::uint16_t ComponentAbi2DescriptorVersion = 1;

enum class ComponentAbi2OperationKind : std::uint8_t {
  Send = 1,
  Receive = 2,
  Invoke = 3,
};

enum class ComponentAbi2TerminalCardinality : std::uint8_t {
  OneShot = 1,
  MultiSubmit = 2,
};

enum class ComponentAbi2LeasePolicy : std::uint8_t {
  Exclusive = 1,
  Shared = 2,
};

enum class ComponentAbi2DescriptorError : std::uint8_t {
  None,
  InvalidMagic,
  UnsupportedVersion,
  Truncated,
  SizeOverflow,
  InvalidField,
  NonCanonical,
  DigestMismatch,
  AbiMismatch,
  IoError,
};

struct ComponentAbi2Descriptor {
  std::uint32_t abi_epoch = ComponentAbi2Epoch;
  std::uint16_t descriptor_version = ComponentAbi2DescriptorVersion;
  ComponentAbi2OperationKind operation_kind = ComponentAbi2OperationKind::Invoke;
  ComponentAbi2TerminalCardinality terminal_cardinality =
      ComponentAbi2TerminalCardinality::OneShot;
  ComponentAbi2LeasePolicy lease_policy = ComponentAbi2LeasePolicy::Exclusive;
  std::uint8_t ownership_flags = 0;
  std::string component_identity;
  std::string entity_identity;
  std::string resource_identity;
  StableFingerprint payload_type_digest;
  StableFingerprint layout_digest;
  StableFingerprint lifecycle_digest;
  StableFingerprint contract_digest;
  StableFingerprint runtime_abi_digest;
  StableFingerprint descriptor_digest;

  [[nodiscard]] bool canonicalize(std::string &error);
  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] std::string encode(std::string &error) const;
  [[nodiscard]] static std::optional<ComponentAbi2Descriptor>
  decode(std::string_view bytes, ComponentAbi2DescriptorError &kind,
         std::string &error);
};

[[nodiscard]] StableFingerprint
componentAbi2DescriptorDigest(const ComponentAbi2Descriptor &descriptor);

[[nodiscard]] StableFingerprint componentAbi2PayloadPlanDigest(
    const ComponentAbi2Descriptor &descriptor,
    const StableFingerprint &outcome_fingerprint,
    std::uint32_t source_lane, std::uint32_t destination_lane,
    std::uint32_t token_lane, bool source_preserved_until_commit,
    bool destination_initializes_on_commit);

[[nodiscard]] const char *
componentAbi2DescriptorErrorText(ComponentAbi2DescriptorError error);

// Design-gate model for Component ABI-2 operation capabilities. This is a
// compiler-owned state machine, not a published ABI or source-language type.
enum class ComponentAbi2OperationState : std::uint8_t {
  Created,
  Armed,
  Committed,
  Failed,
  Cancelled,
  Released,
};

enum class ComponentAbi2OperationEvent : std::uint8_t {
  Arm,
  Commit,
  Fail,
  Cancel,
  Release,
};

[[nodiscard]] bool componentAbi2IsTerminal(
    ComponentAbi2OperationState state);

// Applies exactly one protocol transition. Invalid or duplicate transitions
// return false and leave the state unchanged.
[[nodiscard]] bool componentAbi2Advance(
    ComponentAbi2OperationState &state,
    ComponentAbi2OperationEvent event);

enum class ComponentAbi2LeaseState : std::uint8_t {
  Available,
  Leased,
  Closing,
  Closed,
};

enum class ComponentAbi2LeaseEvent : std::uint8_t {
  Acquire,
  Release,
  BeginClose,
  Quiesce,
};

struct ComponentAbi2ResourceLease {
  ComponentAbi2LeaseState state = ComponentAbi2LeaseState::Available;
  std::uint32_t active_operations = 0;
};

[[nodiscard]] bool componentAbi2LeaseIsClosed(ComponentAbi2LeaseState state);

// A lease is advanced at the host's single linearization point. Invalid or
// duplicate events leave state unchanged.
[[nodiscard]] bool componentAbi2AdvanceLease(
    ComponentAbi2LeaseState &state, ComponentAbi2LeaseEvent event);

[[nodiscard]] bool componentAbi2LeaseAcquire(
    ComponentAbi2ResourceLease &lease);
[[nodiscard]] bool componentAbi2LeaseRelease(
    ComponentAbi2ResourceLease &lease);
[[nodiscard]] bool componentAbi2LeaseBeginClose(
    ComponentAbi2ResourceLease &lease);
[[nodiscard]] bool componentAbi2LeaseQuiesce(
    ComponentAbi2ResourceLease &lease);

} // namespace chtholly::compiler
