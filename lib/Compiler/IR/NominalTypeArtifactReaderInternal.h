#pragma once

#include "chtholly/Compiler/NominalTypeArtifact.h"
#include "ArtifactDecodeInternal.h"

#include <array>
#include <bit>
#include <limits>

namespace chtholly::compiler::internal {

class NominalArtifactReader {
public:
  NominalArtifactReader(std::string_view input, internal::ArtifactDecodeContext &context,
         std::string &error)
      : reader_(input, context), context_(context), error_(error) {}
  ~NominalArtifactReader() {
    context_.preferBudgetError(error_);
  }

  bool bytes(std::size_t count, std::string_view &value) {
    return reader_.bytes(count, value);
  }
  bool magic(std::string_view value) {
    std::string_view found;
    return bytes(value.size(), found) && found == value;
  }
  bool u8(std::uint8_t &value) {
    return reader_.u8(value);
  }
  bool u32(std::uint32_t &value) {
    return reader_.u32(value);
  }
  bool u64(std::uint64_t &value) {
    return reader_.u64(value);
  }
  bool string(std::string &value) {
    return reader_.string(value);
  }
  bool fingerprint(StableFingerprint &value) {
    std::string_view data;
    if (!bytes(StableFingerprint::ByteCount, data))
      return false;
    std::array<std::uint8_t, StableFingerprint::ByteCount> result{};
    for (std::size_t index = 0; index < result.size(); ++index)
      result[index] = static_cast<std::uint8_t>(data[index]);
    value = StableFingerprint(result);
    return true;
  }
  bool entity(PublicEntityReferenceArtifact &value) {
    std::uint8_t kind = 0;
    if (!u8(kind) || !string(value.canonical_package) ||
        !string(value.canonical_module) || !string(value.canonical_name) ||
        !fingerprint(value.expected_fingerprint))
      return false;
    value.kind = static_cast<PublicEntityKind>(kind);
    return true;
  }
  bool type(PublicType &value) {
    internal::ArtifactDecodeRecursionScope recursion(context_);
    std::uint8_t kind = 0;
    if (!recursion.entered() || !u8(kind) || !u32(value.binding_index))
      return false;
    value.kind = static_cast<PublicTypeKind>(kind);
    if (value.kind == PublicTypeKind::Integer) {
      std::uint8_t is_signed = 0;
      if (!u32(value.scalar_width) || !u8(is_signed) || is_signed > 1)
        return false;
      value.integer_signed = is_signed != 0;
      return true;
    }
    if (value.kind == PublicTypeKind::Float)
      return u32(value.scalar_width);
    if (value.kind == PublicTypeKind::Reference) {
      std::uint8_t mutability = 0;
      std::uint8_t provenance_kind = 0;
      value.arguments.resize(1);
      if (!u8(mutability) || !u8(provenance_kind) ||
          !u32(value.reference_provenance.index) ||
          !type(value.arguments.front()))
        return false;
      value.reference_mutability =
          static_cast<PublicReferenceMutability>(mutability);
      value.reference_provenance.kind =
          static_cast<PublicReferenceProvenanceKind>(provenance_kind);
      return true;
    }
    if (value.kind == PublicTypeKind::Array) {
      value.arguments.resize(1);
      return u32(value.array_bound) && value.array_bound != 0 &&
             type(value.arguments.front());
    }
    if (value.kind == PublicTypeKind::Tuple) {
      std::uint8_t abi_union = 0;
      std::uint32_t count = 0;
      if (!u8(abi_union) || abi_union > 1 || !u32(count) || !records(count, 5))
        return false;
      value.abi_union = abi_union != 0;
      value.arguments.resize(count);
      for (auto &argument : value.arguments)
        if (!type(argument))
          return false;
      return true;
    }
    if (value.kind == PublicTypeKind::Slice) {
      std::uint8_t mutable_view = 0;
      value.arguments.resize(1);
      if (!u8(mutable_view) || mutable_view > 1 ||
          !type(value.arguments.front()))
        return false;
      value.slice_mutable = mutable_view != 0;
      return true;
    }
    if (value.kind == PublicTypeKind::TypeProjection) {
      std::uint8_t projection = 0;
      value.arguments.resize(1);
      if (!u8(projection) ||
          projection >=
              static_cast<std::uint8_t>(PublicTypeProjectionKind::Count) ||
          !u32(value.projection_index))
        return false;
      value.projection_kind = static_cast<PublicTypeProjectionKind>(projection);
      return (value.projection_kind != PublicTypeProjectionKind::Associated ||
              entity(value.nominal_entity)) &&
             type(value.arguments.front());
    }
    if (value.kind == PublicTypeKind::RawPointer) {
      std::uint8_t pointee_const = 0;
      value.arguments.resize(1);
      if (!u8(pointee_const) || pointee_const > 1 ||
          !type(value.arguments.front()))
        return false;
      value.pointer_const = pointee_const != 0;
      return true;
    }
    if (value.kind == PublicTypeKind::Function) {
      std::uint32_t count = 0;
      if (!u32(count) || count == 0 || !records(count, 5))
        return false;
      value.arguments.resize(count);
      for (auto &argument : value.arguments)
        if (!type(argument))
          return false;
      return true;
    }
    if (value.kind == PublicTypeKind::CFunctionPointer) {
      std::uint8_t variadic = 0;
      std::uint8_t convention = 0;
      std::uint32_t count = 0;
      if (!u8(variadic) || variadic > 1 || !u8(convention) ||
          convention >=
              static_cast<std::uint8_t>(ForeignCallingConvention::Count) ||
          !u32(value.callable_context_parameter) || !u32(count) || count == 0 ||
          !records(count, 5))
        return false;
      value.callable_variadic = variadic != 0;
      value.foreign_calling_convention =
          static_cast<ForeignCallingConvention>(convention);
      value.arguments.resize(count);
      for (auto &argument : value.arguments)
        if (!type(argument))
          return false;
      return ownershipSummary(value.callable_contract, count - 1);
    }
    if (value.kind == PublicTypeKind::CallbackAdapter) {
      std::uint32_t count = 0;
      if (!u32(count) || count != 3 || !records(count, 5))
        return false;
      value.arguments.resize(count);
      for (auto &argument : value.arguments)
        if (!type(argument))
          return false;
      return true;
    }
    if (value.kind == PublicTypeKind::CallbackRegistration) {
      std::uint8_t authority = 0;
      std::uint32_t binding_count = 0, count = 0;
      if (!u8(authority) || authority > 1 ||
          !u32(value.registration_entry_parameter) ||
          !u32(value.registration_userdata_parameter) ||
          !u32(value.registration_release_parameter) || !u32(binding_count) ||
          !records(binding_count, 8))
        return false;
      value.registration_bindings.resize(binding_count);
      for (auto &binding : value.registration_bindings)
        if (!string(binding.name) || !u32(binding.parameter_index))
          return false;
      for (auto &parameter : value.registration_arm_parameters)
        if (!u32(parameter))
          return false;
      for (auto &parameter : value.registration_detach_parameters)
        if (!u32(parameter))
          return false;
      if (!u32(count) || !records(count, 5) ||
          (count != 5 && count != 7 && count != 8 && count != 10))
        return false;
      value.registration_authority = authority;
      value.arguments.resize(count);
      for (auto &argument : value.arguments)
        if (!type(argument))
          return false;
      std::string protocol_bytes;
      std::string protocol_error;
      if (!string(protocol_bytes))
        return false;
      auto protocol = internal::decodeForeignResourceProtocol(
          protocol_bytes, count, protocol_error, context_);
      if (!protocol)
        return false;
      value.foreign_resource_protocol = std::move(*protocol);
      return value.foreign_resource_protocol ==
             makeCallbackRegistrationProtocol(
                 value.registration_authority,
                 value.registration_entry_parameter,
                 value.registration_userdata_parameter,
                 value.registration_release_parameter,
                 value.registration_bindings, count,
                 value.registration_arm_parameters,
                 value.registration_detach_parameters);
    }
    if (value.kind == PublicTypeKind::CallbackCompletion) {
      std::uint8_t authority = 0;
      std::uint32_t count = 0;
      if (!u8(authority) || authority > 1)
        return false;
      for (auto &parameter : value.registration_arm_parameters)
        if (!u32(parameter))
          return false;
      for (auto &parameter : value.registration_detach_parameters)
        if (!u32(parameter))
          return false;
      if (!u32(count) || !records(count, 5) ||
          (count != 4 && count != 5 && count != 7))
        return false;
      value.registration_authority = authority;
      value.arguments.resize(count);
      for (auto &argument : value.arguments)
        if (!type(argument))
          return false;
      std::string protocol_bytes;
      std::string protocol_error;
      if (!string(protocol_bytes))
        return false;
      auto protocol = internal::decodeForeignResourceProtocol(
          protocol_bytes, count, protocol_error, context_);
      if (!protocol)
        return false;
      value.foreign_resource_protocol = std::move(*protocol);
      return value.foreign_resource_protocol ==
             makeCallbackCompletionProtocol(
                 value.registration_authority, count,
                 value.registration_arm_parameters,
                 value.registration_detach_parameters);
    }
    if (value.kind == PublicTypeKind::CallbackWake) {
      std::uint32_t count = 0;
      if (!u32(count) || count != 1 || !records(count, 5))
        return false;
      value.arguments.resize(1);
      return type(value.arguments.front());
    }
    if (value.kind == PublicTypeKind::ForeignCompletion ||
        value.kind == PublicTypeKind::ForeignWake)
      return entity(value.nominal_entity);
    if (value.kind == PublicTypeKind::ForeignOperationState) {
      std::uint8_t state = 0;
      if (!entity(value.nominal_entity) || !u8(state) ||
          state >= static_cast<std::uint8_t>(ForeignOperationStateKind::Count))
        return false;
      value.foreign_operation_state =
          static_cast<ForeignOperationStateKind>(state);
      return true;
    }
    if (value.kind != PublicTypeKind::Nominal)
      return true;
    std::uint32_t count = 0;
    if (!entity(value.nominal_entity) || !u32(count) || !records(count, 5))
      return false;
    value.arguments.resize(count);
    for (auto &argument : value.arguments)
      if (!type(argument))
        return false;
    return true;
  }
  bool ownershipRegion(OwnershipRegion &region) {
    std::uint32_t count = 0;
    std::uint8_t has_bits = 0;
    if (!u32(region.parameter_index) || !u32(count) || count > 256 ||
        !records(count, 5))
      return false;
    region.path.resize(count);
    for (auto &step : region.path) {
      std::uint8_t kind = 0;
      if (!u8(kind) || !u32(step.index))
        return false;
      step.kind = static_cast<OwnershipRegionStepKind>(kind);
    }
    if (!u8(has_bits) || has_bits > 1 || !u32(region.bit_begin) ||
        !u32(region.bit_end))
      return false;
    region.has_bit_range = has_bits != 0;
    return true;
  }
  bool ownershipSummary(CallableOwnershipSummary &summary,
                        std::uint32_t parameter_count) {
    std::uint32_t count = 0;
    if (!u32(count) || !records(count, 18))
      return false;
    summary.effects.resize(count);
    for (auto &effect : summary.effects) {
      std::uint8_t kind = 0;
      if (!u8(kind) || !ownershipRegion(effect.region))
        return false;
      effect.kind = static_cast<CallableEffectKind>(kind);
    }
    if (!u32(count) || !records(count, 18))
      return false;
    summary.postconditions.resize(count);
    for (auto &postcondition : summary.postconditions) {
      std::uint32_t clause_count = 0;
      if (!u32(postcondition.result_variant) || !ownershipRegion(postcondition.region) ||
          !u8(postcondition.outcomes) || !u32(clause_count) ||
          clause_count > 32 || !records(clause_count, 4))
        return false;
      postcondition.condition.clauses.resize(clause_count);
      for (auto &clause : postcondition.condition.clauses) {
        std::uint32_t atom_count = 0;
        if (!u32(atom_count) || atom_count > 8 || !records(atom_count, 9))
          return false;
        clause.atoms.resize(atom_count);
        for (auto &atom : clause.atoms) {
          std::uint8_t expected = 0;
          if (!u32(atom.variant) || !u32(atom.parameter_index) || !u8(expected) || expected > 1)
            return false;
          atom.expected = expected != 0;
        }
      }
    }
    std::uint8_t owned = 0;
    if (!u8(owned) || owned > 1 || !u32(count) || !records(count, 17))
      return false;
    summary.returns_owned = owned != 0;
    summary.return_provenance.resize(count);
    for (auto &source : summary.return_provenance) {
      std::uint32_t clause_count = 0;
      std::uint32_t path_count = 0;
      if (!ownershipRegion(source.region) || !u32(path_count) ||
          path_count > 256 || !records(path_count, 5))
        return false;
      source.carrier_path.resize(path_count);
      for (auto &step : source.carrier_path) {
        std::uint8_t kind = 0;
        if (!u8(kind) || !u32(step.index))
          return false;
        step.kind = static_cast<CallableReturnSource::CarrierStepKind>(kind);
      }
      if (!u32(clause_count) || clause_count > 32 || !records(clause_count, 4))
        return false;
      source.condition.clauses.resize(clause_count);
      for (auto &clause : source.condition.clauses) {
        std::uint32_t atom_count = 0;
        if (!u32(atom_count) || atom_count > 8 || !records(atom_count, 9))
          return false;
        clause.atoms.resize(atom_count);
        for (auto &atom : clause.atoms) {
          std::uint8_t expected = 0;
          if (!u32(atom.variant) || !u32(atom.parameter_index) || !u8(expected) || expected > 1)
            return false;
          atom.expected = expected != 0;
        }
      }
    }
    std::string error;
    return summary.verify(parameter_count, error);
  }
  bool fields(std::vector<PublicNominalFieldArtifact> &value) {
    std::uint32_t count = 0;
    if (!u32(count) || !records(count, 9))
      return false;
    value.resize(count);
    for (auto &field : value) {
      std::uint32_t path_count = 0;
      if (!string(field.name) || !type(field.type) || !u32(path_count) ||
          !records(path_count, 4))
        return false;
      field.storage_path.resize(path_count);
      for (auto &component : field.storage_path)
        if (!string(component))
          return false;
      std::uint8_t projection_kind = 0;
      if (!u8(projection_kind) || !string(field.projector_name) ||
          !u32(path_count) || !records(path_count, 4))
        return false;
      field.projection_region_path.resize(path_count);
      for (auto &component : field.projection_region_path)
        if (!string(component))
          return false;
      std::uint8_t is_public = 0;
      if (!u32(field.bit_begin) || !u32(field.bit_end) || !u8(is_public) ||
          is_public > 1)
        return false;
      field.projection_kind =
          static_cast<PublicObjectProjectionKind>(projection_kind);
      field.is_public = is_public != 0;
    }
    return true;
  }
  bool variants(std::vector<PublicEnumVariantArtifact> &value) {
    std::uint32_t count = 0;
    if (!u32(count) || !records(count, 17))
      return false;
    value.resize(count);
    for (auto &variant : value) {
      std::uint8_t shape = 0;
      std::uint64_t discriminant = 0;
      if (!string(variant.name) || !u8(shape) || !u64(discriminant) ||
          shape >= static_cast<std::uint8_t>(PublicEnumPayloadShape::Count) ||
          !fields(variant.fields))
        return false;
      variant.shape = static_cast<PublicEnumPayloadShape>(shape);
      variant.discriminant = std::bit_cast<std::int64_t>(discriminant);
    }
    return true;
  }
  bool done() const {
    return reader_.done();
  }
  std::size_t remaining() const {
    return reader_.remaining();
  }
  bool records(std::uint32_t count, std::size_t minimum_size) {
    return reader_.records(count, minimum_size);
  }

private:
  internal::ArtifactDecodeReader reader_;
  internal::ArtifactDecodeContext &context_;
  std::string &error_;
};



} // namespace chtholly::compiler::internal
