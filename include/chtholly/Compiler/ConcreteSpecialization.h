#pragma once

#include "chtholly/Compiler/NominalTypeArtifact.h"
#include "chtholly/Compiler/Outcome.h"
#include "chtholly/Compiler/ComponentABI2Protocol.h"
#include "chtholly/Compiler/PublicInterface.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly::compiler {

inline constexpr std::uint32_t ConcreteSpecializationComponentFormat = 51;

enum class ConcreteCallTargetKind : std::uint8_t {
  ComponentNode,
  ExternalComponent,
  PublicEntity,
  Count,
};

struct ConcreteCallTargetArtifact {
  ConcreteCallTargetKind kind = ConcreteCallTargetKind::Count;
  std::uint32_t node_index = core::AnyId::InvalidIndex;
  StableFingerprint request_fingerprint;
  StableFingerprint component_fingerprint;
  PublicEntityReferenceArtifact public_entity;
  std::vector<PublicType> type_arguments;
  PublicType callable_type;

  friend bool operator==(const ConcreteCallTargetArtifact &,
                         const ConcreteCallTargetArtifact &) = default;
};

// Target-independent descriptor for a concrete typed-channel payload. Runtime
// callback addresses are intentionally absent; lowering resolves the canonical
// lifecycle identities for the current target.
struct ConcreteTypedChannelDescriptor {
  PublicType payload_type;
  StableFingerprint payload_type_fingerprint;
  StableFingerprint layout_fingerprint;
  StableFingerprint lifecycle_fingerprint;
  StableFingerprint outcome_fingerprint =
      canonicalTypedChannelOutcomeFingerprint();
  TypeRepresentationFacts representation;
  TypeConcurrencyFacts concurrency;
  std::uint32_t runtime_abi_epoch = 1;
  std::optional<PublicEntityReferenceArtifact> move_target;
  std::optional<PublicEntityReferenceArtifact> drop_target;
  std::string component_identity;
  std::string operation_identity;
  ComponentAbi2OperationKind operation_kind = ComponentAbi2OperationKind::Send;
  ComponentAbi2LeasePolicy lease_policy = ComponentAbi2LeasePolicy::Exclusive;
  std::uint8_t ownership_flags = 0;
  StableFingerprint component_descriptor_digest;

  friend bool operator==(const ConcreteTypedChannelDescriptor &,
                         const ConcreteTypedChannelDescriptor &) = default;
};

[[nodiscard]] bool verifyConcreteTypedChannelDescriptor(
    const ConcreteTypedChannelDescriptor &descriptor, std::string &error);

struct ConcreteSpecificNodeArtifact {
  StableFingerprint request_fingerprint;
  PublicEntityReferenceArtifact template_entity;
  std::vector<PublicType> arguments;
  std::vector<StableFingerprint> constraint_witnesses;
  std::uint32_t parameter_count = 0;
  CallableSemanticContract semantic_contract;
  CallableOwnershipSummary ownership_summary;
  std::vector<PublicType> local_types;
  std::vector<std::uint32_t> local_flags;
  std::vector<std::int64_t> integers;
  std::vector<std::string> strings;
  std::vector<ConcreteCallTargetArtifact> callees;
  std::vector<ConcreteTypedChannelDescriptor> typed_channels;
  GenericEvaluationRegionArtifact body;

  friend bool operator==(const ConcreteSpecificNodeArtifact &,
                         const ConcreteSpecificNodeArtifact &) = default;
};

class ConcreteSpecializationComponentArtifact {
public:
  ConcreteSpecializationComponentArtifact() = default;
  ConcreteSpecializationComponentArtifact(
      StableFingerprint semantic_options_fingerprint,
      std::vector<ConcreteSpecificNodeArtifact> nodes);

  [[nodiscard]] const StableFingerprint &semanticOptionsFingerprint() const {
    return semantic_options_fingerprint_;
  }
  [[nodiscard]] std::span<const ConcreteSpecificNodeArtifact> nodes() const {
    return nodes_;
  }
  [[nodiscard]] const ConcreteSpecificNodeArtifact *
  findNode(const StableFingerprint &request_fingerprint) const;
  [[nodiscard]] std::vector<StableFingerprint> dependencies() const;
  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] std::string encode(std::string &error) const;
  [[nodiscard]] static std::optional<ConcreteSpecializationComponentArtifact>
  decode(std::string_view bytes, std::string &error);
  [[nodiscard]] StableFingerprint fingerprint() const;

private:
  StableFingerprint semantic_options_fingerprint_;
  std::vector<ConcreteSpecificNodeArtifact> nodes_;
};

struct ConcreteSpecializationReference {
  StableFingerprint request_fingerprint;
  StableFingerprint component_fingerprint;

  friend bool operator==(const ConcreteSpecializationReference &,
                         const ConcreteSpecializationReference &) = default;
};

enum class ConcreteSpecializationLoadStatus : std::uint8_t {
  Missing,
  Found,
  Corrupt,
  Error,
};

struct ConcreteSpecializationLoadResult {
  ConcreteSpecializationLoadStatus status =
      ConcreteSpecializationLoadStatus::Missing;
  std::vector<ConcreteSpecializationComponentArtifact> components;
  std::string error;
};

using ConcreteSpecializationLoader =
    std::function<ConcreteSpecializationLoadResult(
        const StableFingerprint &request_fingerprint)>;

struct ConcreteSpecializationCacheStats {
  std::size_t lookups = 0;
  std::size_t hits = 0;
  std::size_t misses = 0;
  std::size_t semantic_rejections = 0;
  std::size_t rebuilt_components = 0;
};

[[nodiscard]] StableFingerprint fingerprintConcreteSemanticOptions(
    std::span<const std::string> resolved_features);
[[nodiscard]] StableFingerprint
fingerprintConcreteTypeArguments(std::span<const PublicType> arguments);
[[nodiscard]] StableFingerprint fingerprintConcreteSpecializationRequest(
    const PublicEntityReferenceArtifact &entity,
    std::span<const PublicType> arguments,
    std::span<const StableFingerprint> constraint_witnesses,
    StableFingerprint semantic_options_fingerprint);
[[nodiscard]] StableFingerprint fingerprintConcreteSpecializationRequest(
    const PublicEntityReferenceArtifact &entity,
    std::span<const PublicType> arguments,
    StableFingerprint semantic_options_fingerprint);

} // namespace chtholly::compiler
