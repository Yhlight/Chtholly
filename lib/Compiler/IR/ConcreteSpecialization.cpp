#include "chtholly/Compiler/ConcreteSpecialization.h"

#include "ArtifactDecodeInternal.h"
#include "chtholly/Compiler/BuiltinOperator.h"
#include "chtholly/Compiler/Outcome.h"
#include "chtholly/Compiler/SemIR.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <limits>
#include <ranges>
#include <set>
#include <unordered_set>

namespace chtholly::compiler {
namespace {
#include "ConcreteSpecializationSupportInternal.h"

bool verifyTypedChannelDescriptor(
    const ConcreteTypedChannelDescriptor &descriptor, std::string &error) {
  std::string outcome_error;
  // Typed-channel descriptors are MultiSubmit operations. Reconstructing and
  // validating the canonical protocol on artifact load prevents a stale or
  // hand-built descriptor from silently falling back to byte-copy semantics.
  if (!makeChannelOutcome().verify(outcome_error)) {
    error = "typed channel descriptor has invalid outcome facts: " +
            outcome_error;
    return false;
  }
  if (!concreteType(descriptor.payload_type, false) ||
      !descriptor.payload_type_fingerprint.hasValue() ||
      !descriptor.layout_fingerprint.hasValue() ||
      !descriptor.lifecycle_fingerprint.hasValue() ||
      descriptor.outcome_fingerprint !=
          canonicalTypedChannelOutcomeFingerprint() ||
      descriptor.runtime_abi_epoch == 0 ||
      descriptor.representation.value_repr >= ValueReprKind::Count ||
      descriptor.representation.init_repr >= InitReprKind::Count ||
      descriptor.representation.ownership >= OwnershipReprKind::Count ||
      descriptor.representation.copy >= CopyReprKind::Count ||
      descriptor.representation.object_repr >= ObjectReprKind::Count ||
      !descriptor.concurrency.transferable ||
      descriptor.representation.move == MoveReprKind::None ||
      descriptor.representation.move == MoveReprKind::Unavailable ||
      descriptor.representation.move == MoveReprKind::Dependent ||
      descriptor.representation.destroy == DestroyReprKind::None ||
      descriptor.representation.destroy == DestroyReprKind::Dependent ||
      (descriptor.representation.destroy == DestroyReprKind::Custom &&
       !descriptor.drop_target) ||
      descriptor.payload_type.kind == PublicTypeKind::Reference ||
      descriptor.payload_type.kind == PublicTypeKind::Slice ||
      descriptor.payload_type.kind == PublicTypeKind::RawPointer ||
      descriptor.payload_type.kind == PublicTypeKind::Function ||
      descriptor.payload_type.kind == PublicTypeKind::CFunctionPointer ||
      descriptor.payload_type.kind == PublicTypeKind::TypeParameter ||
      descriptor.payload_type.kind == PublicTypeKind::TypeProjection ||
      (descriptor.move_target &&
       (!validEntity(*descriptor.move_target) ||
        descriptor.move_target->kind != PublicEntityKind::Function)) ||
      (descriptor.drop_target &&
       (!validEntity(*descriptor.drop_target) ||
        descriptor.drop_target->kind != PublicEntityKind::Function)) ||
      descriptor.component_identity.empty() || descriptor.operation_identity.empty() ||
      descriptor.operation_kind == ComponentAbi2OperationKind::Invoke ||
      descriptor.lease_policy > ComponentAbi2LeasePolicy::Shared ||
      !descriptor.component_descriptor_digest.hasValue()) {
    error = "typed channel descriptor has invalid payload or witness facts";
    return false;
  }
  return true;
}

void appendOptionalTypedChannelEntity(
    std::string &out,
    const std::optional<PublicEntityReferenceArtifact> &entity) {
  out.push_back(entity.has_value() ? 1 : 0);
  if (entity)
    appendEntity(out, *entity);
}

bool readOptionalTypedChannelEntity(
    Reader &reader, std::optional<PublicEntityReferenceArtifact> &entity) {
  std::uint8_t present = 0;
  if (!reader.u8(present) || present > 1)
    return false;
  if (!present) {
    entity.reset();
    return true;
  }
  PublicEntityReferenceArtifact value;
  if (!reader.entity(value))
    return false;
  entity = std::move(value);
  return true;
}

} // namespace

bool verifyConcreteTypedChannelDescriptor(
    const ConcreteTypedChannelDescriptor &descriptor, std::string &error) {
  return verifyTypedChannelDescriptor(descriptor, error);
}

ConcreteSpecializationComponentArtifact::
    ConcreteSpecializationComponentArtifact(
        StableFingerprint semantic_options_fingerprint,
        std::vector<ConcreteSpecificNodeArtifact> nodes)
    : semantic_options_fingerprint_(semantic_options_fingerprint),
      nodes_(std::move(nodes)) {
  std::ranges::sort(nodes_, {}, [](const auto &node) {
    return node.request_fingerprint.hex();
  });
}

const ConcreteSpecificNodeArtifact *
ConcreteSpecializationComponentArtifact::findNode(
    const StableFingerprint &request_fingerprint) const {
  const auto key = request_fingerprint.hex();
  const auto found = std::ranges::lower_bound(
      nodes_, key, {}, [](const ConcreteSpecificNodeArtifact &node) {
        return node.request_fingerprint.hex();
      });
  return found != nodes_.end() &&
                 found->request_fingerprint == request_fingerprint
             ? &*found
             : nullptr;
}

std::vector<StableFingerprint>
ConcreteSpecializationComponentArtifact::dependencies() const {
  std::vector<StableFingerprint> result;
  for (const auto &node : nodes_)
    for (const auto &callee : node.callees)
      if (callee.kind == ConcreteCallTargetKind::ExternalComponent)
        result.push_back(callee.component_fingerprint);
  std::ranges::sort(result, {},
                    [](const StableFingerprint &value) { return value.hex(); });
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

bool ConcreteSpecializationComponentArtifact::verify(std::string &error) const {
  error.clear();
  if (!semantic_options_fingerprint_.hasValue() || nodes_.empty() ||
      nodes_.size() > MaxComponentNodes) {
    error = "concrete specialization component has an invalid header";
    return false;
  }
  std::string previous;
  for (const auto &node : nodes_) {
    const auto key = node.request_fingerprint.hex();
    if (!node.request_fingerprint.hasValue() ||
        !validEntity(node.template_entity) ||
        (!previous.empty() && previous >= key) || node.arguments.empty() ||
        std::ranges::any_of(
            node.arguments,
            [](PublicType type) { return !concreteType(type, false); }) ||
        std::ranges::any_of(node.constraint_witnesses,
                            [](const StableFingerprint &witness) {
                              return !witness.hasValue();
                            }) ||
        node.request_fingerprint != fingerprintConcreteSpecializationRequest(
                                        node.template_entity, node.arguments,
                                        node.constraint_witnesses,
                                        semantic_options_fingerprint_) ||
        node.typed_channels.size() > MaxTypedChannelDescriptors ||
        !verifyBody(node, error)) {
      if (error.empty())
        error = "concrete specialization component has an invalid node";
      return false;
    }
    for (const auto &descriptor : node.typed_channels) {
      if (!verifyTypedChannelDescriptor(descriptor, error))
        return false;
    }
    for (const auto &callee : node.callees) {
      bool valid = callee.kind < ConcreteCallTargetKind::Count &&
                   concreteType(callee.callable_type, false) &&
                   callee.callable_type.kind == PublicTypeKind::Function;
      switch (callee.kind) {
      case ConcreteCallTargetKind::ComponentNode:
        valid = valid && callee.node_index < nodes_.size() &&
                callee.request_fingerprint.hasValue() &&
                callee.request_fingerprint ==
                    nodes_[callee.node_index].request_fingerprint &&
                !callee.component_fingerprint.hasValue() &&
                callee.type_arguments.empty();
        if (valid) {
          const auto &target = nodes_[callee.node_index];
          valid = target.body.results.size() == target.parameter_count + 1;
          if (valid) {
            std::vector<PublicType> parameters(target.body.results.begin(),
                                               target.body.results.begin() +
                                                   target.parameter_count);
            valid = callee.callable_type ==
                    PublicType::function(std::move(parameters),
                                         target.body.results.back());
          }
        }
        break;
      case ConcreteCallTargetKind::ExternalComponent:
        valid = valid && callee.node_index == core::AnyId::InvalidIndex &&
                callee.request_fingerprint.hasValue() &&
                callee.component_fingerprint.hasValue() &&
                callee.type_arguments.empty();
        break;
      case ConcreteCallTargetKind::PublicEntity:
        valid = valid && callee.node_index == core::AnyId::InvalidIndex &&
                !callee.request_fingerprint.hasValue() &&
                !callee.component_fingerprint.hasValue() &&
                validEntity(callee.public_entity) &&
                std::ranges::all_of(callee.type_arguments,
                                    [](const auto &argument) {
                                      return concreteType(argument, false);
                                    });
        break;
      case ConcreteCallTargetKind::Count:
        valid = false;
        break;
      }
      if (!valid) {
        error = "concrete specialization component has an invalid callee";
        return false;
      }
    }
    previous = key;
  }
  return true;
}

std::string
ConcreteSpecializationComponentArtifact::encode(std::string &error) const {
  if (!verify(error))
    return {};
  std::string out(ComponentMagic);
  appendU32(out, ComponentVersion);
  appendFingerprint(out, semantic_options_fingerprint_);
  appendU32(out, static_cast<std::uint32_t>(nodes_.size()));
  for (const auto &node : nodes_) {
    appendFingerprint(out, node.request_fingerprint);
    appendEntity(out, node.template_entity);
    appendU32(out, static_cast<std::uint32_t>(node.arguments.size()));
    for (const auto type : node.arguments)
      appendType(out, type);
    appendU32(out,
              static_cast<std::uint32_t>(node.constraint_witnesses.size()));
    for (const auto &witness : node.constraint_witnesses)
      appendFingerprint(out, witness);
    appendU32(out, node.parameter_count);
    appendSemanticContract(out, node.semantic_contract);
    appendOwnershipSummary(out, node.ownership_summary);
    appendU32(out, static_cast<std::uint32_t>(node.local_types.size()));
    for (const auto type : node.local_types)
      appendType(out, type);
    for (const auto flags : node.local_flags)
      appendU32(out, flags);
    appendU32(out, static_cast<std::uint32_t>(node.integers.size()));
    for (const auto value : node.integers) {
      const auto bits = static_cast<std::uint64_t>(value);
      appendU32(out, static_cast<std::uint32_t>(bits));
      appendU32(out, static_cast<std::uint32_t>(bits >> 32U));
    }
    appendU32(out, static_cast<std::uint32_t>(node.strings.size()));
    for (const auto &value : node.strings)
      appendField(out, value);
    appendU32(out, static_cast<std::uint32_t>(node.callees.size()));
    for (const auto &callee : node.callees) {
      out.push_back(static_cast<char>(callee.kind));
      appendU32(out, callee.node_index);
      appendFingerprint(out, callee.request_fingerprint);
      appendFingerprint(out, callee.component_fingerprint);
      appendEntity(out, callee.public_entity);
      appendU32(out, static_cast<std::uint32_t>(callee.type_arguments.size()));
      for (const auto &argument : callee.type_arguments)
        appendType(out, argument);
      appendType(out, callee.callable_type);
    }
    appendU32(out, static_cast<std::uint32_t>(node.typed_channels.size()));
    for (const auto &descriptor : node.typed_channels) {
      appendType(out, descriptor.payload_type);
      appendFingerprint(out, descriptor.payload_type_fingerprint);
      appendFingerprint(out, descriptor.layout_fingerprint);
      appendFingerprint(out, descriptor.lifecycle_fingerprint);
      appendFingerprint(out, descriptor.outcome_fingerprint);
      out.push_back(static_cast<char>(descriptor.representation.value_repr));
      out.push_back(static_cast<char>(descriptor.representation.init_repr));
      out.push_back(static_cast<char>(descriptor.representation.ownership));
      out.push_back(static_cast<char>(descriptor.representation.copy));
      out.push_back(static_cast<char>(descriptor.representation.move));
      out.push_back(static_cast<char>(descriptor.representation.destroy));
      out.push_back(static_cast<char>(descriptor.representation.object_repr));
      out.push_back(descriptor.concurrency.transferable ? 1 : 0);
      out.push_back(descriptor.concurrency.shareable ? 1 : 0);
      appendU32(out, descriptor.runtime_abi_epoch);
      appendOptionalTypedChannelEntity(out, descriptor.move_target);
      appendOptionalTypedChannelEntity(out, descriptor.drop_target);
      appendField(out, descriptor.component_identity);
      appendField(out, descriptor.operation_identity);
      out.push_back(static_cast<char>(descriptor.operation_kind));
      out.push_back(static_cast<char>(descriptor.lease_policy));
      out.push_back(static_cast<char>(descriptor.ownership_flags));
      appendFingerprint(out, descriptor.component_descriptor_digest);
    }
    appendRegion(out, node.body);
  }
  return out;
}

std::optional<ConcreteSpecializationComponentArtifact>
ConcreteSpecializationComponentArtifact::decode(std::string_view bytes,
                                                std::string &error) {
  error.clear();
  internal::ArtifactDecodeContext decode_context(bytes.size());
  Reader reader(bytes, decode_context, error);
  if (decode_context.failed())
    return std::nullopt;
  std::string_view magic;
  std::uint32_t version = 0;
  StableFingerprint semantic_options;
  std::uint32_t node_count = 0;
  if (!reader.bytes(ComponentMagic.size(), magic) || magic != ComponentMagic ||
      !reader.u32(version) || version != ComponentVersion ||
      !reader.fingerprint(semantic_options) ||
      !readCount(reader, node_count, 64) || node_count == 0 ||
      node_count > MaxComponentNodes) {
    error = "concrete specialization component has an invalid header";
    return std::nullopt;
  }
  std::vector<ConcreteSpecificNodeArtifact> nodes(node_count);
  for (auto &node : nodes) {
    std::uint32_t count = 0;
    if (!reader.fingerprint(node.request_fingerprint) ||
        !reader.entity(node.template_entity) || !readCount(reader, count, 5)) {
      error = "concrete specialization component is truncated";
      return std::nullopt;
    }
    node.arguments.resize(count);
    for (auto &type : node.arguments)
      if (!reader.type(type)) {
        error = "concrete specialization component is truncated";
        return std::nullopt;
      }
    if (!readCount(reader, count, StableFingerprint::ByteCount)) {
      error = "concrete specialization component is truncated";
      return std::nullopt;
    }
    node.constraint_witnesses.resize(count);
    for (auto &witness : node.constraint_witnesses)
      if (!reader.fingerprint(witness)) {
        error = "concrete specialization component is truncated";
        return std::nullopt;
      }
    if (!reader.u32(node.parameter_count) ||
        !readSemanticContract(reader, node.semantic_contract) ||
        !readOwnershipSummary(reader, node.ownership_summary,
                              node.parameter_count) ||
        !readCount(reader, count, 5)) {
      error = "concrete specialization component is truncated";
      return std::nullopt;
    }
    node.local_types.resize(count);
    for (auto &type : node.local_types)
      if (!reader.type(type)) {
        error = "concrete specialization component is truncated";
        return std::nullopt;
      }
    if (!reader.records(count, 4)) {
      error = "concrete specialization component is truncated";
      return std::nullopt;
    }
    node.local_flags.resize(count);
    for (auto &flags : node.local_flags)
      if (!reader.u32(flags)) {
        error = "concrete specialization component is truncated";
        return std::nullopt;
      }
    if (!readCount(reader, count, 8)) {
      error = "concrete specialization component is truncated";
      return std::nullopt;
    }
    node.integers.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
      std::uint32_t low = 0;
      std::uint32_t high = 0;
      if (!reader.u32(low) || !reader.u32(high)) {
        error = "concrete specialization component is truncated";
        return std::nullopt;
      }
      const auto bits = static_cast<std::uint64_t>(low) |
                        (static_cast<std::uint64_t>(high) << 32U);
      node.integers.push_back(std::bit_cast<std::int64_t>(bits));
    }
    if (!readCount(reader, count, 4)) {
      error = "concrete specialization component is truncated";
      return std::nullopt;
    }
    node.strings.resize(count);
    for (auto &value : node.strings)
      if (!reader.string(value)) {
        error = "concrete specialization component is truncated";
        return std::nullopt;
      }
    if (!readCount(reader, count, MinimumCalleeEncodingSize)) {
      error = "concrete specialization component is truncated";
      return std::nullopt;
    }
    node.callees.resize(count);
    for (auto &callee : node.callees) {
      std::uint8_t kind = 0;
      if (!reader.u8(kind) || !reader.u32(callee.node_index) ||
          !reader.fingerprint(callee.request_fingerprint) ||
          !reader.fingerprint(callee.component_fingerprint) ||
          !reader.entity(callee.public_entity)) {
        error = "concrete specialization component is truncated";
        return std::nullopt;
      }
      callee.kind = static_cast<ConcreteCallTargetKind>(kind);
      std::uint32_t argument_count = 0;
      if (!readCount(reader, argument_count, 5)) {
        error = "concrete specialization component is truncated";
        return std::nullopt;
      }
      callee.type_arguments.resize(argument_count);
      for (auto &argument : callee.type_arguments)
        if (!reader.type(argument)) {
          error = "concrete specialization component is truncated";
          return std::nullopt;
        }
      if (!reader.type(callee.callable_type)) {
        error = "concrete specialization component is truncated";
        return std::nullopt;
      }
    }
    if (!readCount(reader, count, 1) || count > MaxTypedChannelDescriptors) {
      error = "concrete specialization component is truncated";
      return std::nullopt;
    }
    node.typed_channels.resize(count);
    for (auto &descriptor : node.typed_channels) {
      std::uint8_t transferable = 0;
      std::uint8_t shareable = 0;
      std::uint8_t operation_kind = 0;
      std::uint8_t lease_policy = 0;
      std::uint8_t ownership_flags = 0;
      std::array<std::uint8_t, 7> representation{};
      if (!reader.type(descriptor.payload_type) ||
          !reader.fingerprint(descriptor.payload_type_fingerprint) ||
          !reader.fingerprint(descriptor.layout_fingerprint) ||
          !reader.fingerprint(descriptor.lifecycle_fingerprint) ||
          !reader.fingerprint(descriptor.outcome_fingerprint) ||
          !reader.u8(representation[0]) || !reader.u8(representation[1]) ||
          !reader.u8(representation[2]) || !reader.u8(representation[3]) ||
          !reader.u8(representation[4]) || !reader.u8(representation[5]) ||
          !reader.u8(representation[6]) ||
          !reader.u8(transferable) || transferable > 1 ||
          !reader.u8(shareable) || shareable > 1 ||
          !reader.u32(descriptor.runtime_abi_epoch) ||
          !readOptionalTypedChannelEntity(reader, descriptor.move_target) ||
          !readOptionalTypedChannelEntity(reader, descriptor.drop_target) ||
          !reader.string(descriptor.component_identity) ||
          !reader.string(descriptor.operation_identity) ||
          !reader.u8(operation_kind) || !reader.u8(lease_policy) ||
          !reader.u8(ownership_flags) ||
          !reader.fingerprint(descriptor.component_descriptor_digest)) {
        error = "concrete specialization component is truncated";
        return std::nullopt;
      }
      descriptor.representation = {
          static_cast<ValueReprKind>(representation[0]),
          static_cast<InitReprKind>(representation[1]),
          static_cast<OwnershipReprKind>(representation[2]),
          static_cast<CopyReprKind>(representation[3]),
          static_cast<MoveReprKind>(representation[4]),
          static_cast<DestroyReprKind>(representation[5]),
          static_cast<ObjectReprKind>(representation[6])};
      descriptor.concurrency = {transferable != 0, shareable != 0};
      descriptor.operation_kind =
          static_cast<ComponentAbi2OperationKind>(operation_kind);
      descriptor.lease_policy =
          static_cast<ComponentAbi2LeasePolicy>(lease_policy);
      descriptor.ownership_flags = ownership_flags;
    }
    if (!readRegion(reader, node.body)) {
      error = "concrete specialization component is truncated";
      return std::nullopt;
    }
  }
  if (!reader.done()) {
    error = "concrete specialization component has trailing bytes";
    return std::nullopt;
  }
  ConcreteSpecializationComponentArtifact result(semantic_options,
                                                 std::move(nodes));
  if (!result.verify(error))
    return std::nullopt;
  return result;
}

StableFingerprint ConcreteSpecializationComponentArtifact::fingerprint() const {
  std::string error;
  const auto bytes = encode(error);
  return error.empty()
             ? StableFingerprint::fromCanonicalBytes(
                   std::string("chtholly.next.specialization-component.v19") +
                   bytes)
             : StableFingerprint{};
}

StableFingerprint fingerprintConcreteSemanticOptions(
    std::span<const std::string> resolved_features) {
  std::vector<std::string_view> ordered;
  ordered.reserve(resolved_features.size());
  for (const auto &feature : resolved_features)
    ordered.push_back(feature);
  std::ranges::sort(ordered);
  std::string canonical;
  appendField(canonical, "chtholly.next.concrete-semantic-options.v1");
  appendU32(canonical, static_cast<std::uint32_t>(ordered.size()));
  for (const auto feature : ordered)
    appendField(canonical, feature);
  return StableFingerprint::fromCanonicalBytes(canonical);
}

StableFingerprint
fingerprintConcreteTypeArguments(std::span<const PublicType> arguments) {
  std::string canonical;
  appendField(canonical, "chtholly.next.concrete-type-arguments.v3");
  appendU32(canonical, static_cast<std::uint32_t>(arguments.size()));
  for (const auto argument : arguments)
    appendType(canonical, argument);
  return StableFingerprint::fromCanonicalBytes(canonical);
}

StableFingerprint fingerprintConcreteSpecializationRequest(
    const PublicEntityReferenceArtifact &entity,
    std::span<const PublicType> arguments,
    std::span<const StableFingerprint> constraint_witnesses,
    StableFingerprint semantic_options_fingerprint) {
  if (!validEntity(entity) || !semantic_options_fingerprint.hasValue() ||
      arguments.empty() ||
      std::ranges::any_of(
          arguments,
          [](PublicType type) { return !concreteType(type, false); }) ||
      std::ranges::any_of(
          constraint_witnesses,
          [](const StableFingerprint &witness) { return !witness.hasValue(); }))
    return {};
  std::string canonical;
  appendField(canonical, "chtholly.next.concrete-specialization-request.v4");
  appendEntity(canonical, entity);
  appendFingerprint(canonical, fingerprintConcreteTypeArguments(arguments));
  appendU32(canonical, static_cast<std::uint32_t>(constraint_witnesses.size()));
  for (const auto &witness : constraint_witnesses)
    appendFingerprint(canonical, witness);
  appendFingerprint(canonical, semantic_options_fingerprint);
  return StableFingerprint::fromCanonicalBytes(canonical);
}

StableFingerprint fingerprintConcreteSpecializationRequest(
    const PublicEntityReferenceArtifact &entity,
    std::span<const PublicType> arguments,
    StableFingerprint semantic_options_fingerprint) {
  return fingerprintConcreteSpecializationRequest(
      entity, arguments, std::span<const StableFingerprint>{},
      semantic_options_fingerprint);
}

} // namespace chtholly::compiler
