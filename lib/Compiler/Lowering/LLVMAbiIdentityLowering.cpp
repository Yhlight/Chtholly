#include "LLVMInternal.h"

#include "chtholly/Compiler/ProgramModel.h"

#include <string>
#include <string_view>
#include <tuple>

namespace chtholly::compiler {
namespace {

void appendAbiU32(std::string &out, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8)
    out.push_back(static_cast<char>((value >> shift) & 0xFFU));
}

void appendAbiField(std::string &out, std::string_view value) {
  appendAbiU32(out, static_cast<std::uint32_t>(value.size()));
  out.append(value);
}

void appendAbiRegion(std::string &out, const OwnershipRegion &region) {
  appendAbiU32(out, region.parameter_index);
  appendAbiU32(out, static_cast<std::uint32_t>(region.path.size()));
  for (const auto &step : region.path) {
    appendAbiU32(out, static_cast<std::uint32_t>(step.kind));
    appendAbiU32(out, step.index);
  }
  appendAbiU32(out, region.has_bit_range ? 1U : 0U);
  appendAbiU32(out, region.bit_begin);
  appendAbiU32(out, region.bit_end);
}

void appendAbiOwnership(std::string &out,
                        const CallableOwnershipSummary &summary) {
  appendAbiU32(out, static_cast<std::uint32_t>(summary.effects.size()));
  for (const auto &effect : summary.effects) {
    appendAbiU32(out, static_cast<std::uint32_t>(effect.kind));
    appendAbiRegion(out, effect.region);
  }
  appendAbiU32(out, static_cast<std::uint32_t>(summary.postconditions.size()));
  for (const auto &postcondition : summary.postconditions) {
    appendAbiRegion(out, postcondition.region);
    appendAbiU32(out, postcondition.outcomes);
    appendAbiU32(
        out, static_cast<std::uint32_t>(postcondition.condition.clauses.size()));
    for (const auto &clause : postcondition.condition.clauses) {
      appendAbiU32(out, static_cast<std::uint32_t>(clause.atoms.size()));
      for (const auto &atom : clause.atoms) {
        appendAbiU32(out, atom.parameter_index);
        appendAbiU32(out, atom.expected ? 1U : 0U);
      }
    }
  }
  appendAbiU32(out, summary.returns_owned ? 1U : 0U);
  appendAbiU32(out,
               static_cast<std::uint32_t>(summary.return_provenance.size()));
  for (const auto &source : summary.return_provenance) {
    appendAbiRegion(out, source.region);
    appendAbiU32(out, static_cast<std::uint32_t>(source.carrier_path.size()));
    for (const auto &step : source.carrier_path) {
      appendAbiU32(out, static_cast<std::uint32_t>(step.kind));
      appendAbiU32(out, step.index);
    }
    appendAbiU32(out,
                 static_cast<std::uint32_t>(source.condition.clauses.size()));
    for (const auto &clause : source.condition.clauses) {
      appendAbiU32(out, static_cast<std::uint32_t>(clause.atoms.size()));
      for (const auto &atom : clause.atoms) {
        appendAbiU32(out, atom.parameter_index);
        appendAbiU32(out, atom.expected ? 1U : 0U);
      }
    }
  }
}

void appendCanonicalType(const SemIR &sem_ir, std::string_view package_name,
                         CanonicalTypeId id, std::string &bytes,
                         bool normalize_type_parameters) {
  const auto &value = sem_ir.genericValues().type(id);
  appendAbiU32(bytes, static_cast<std::uint32_t>(value.kind));
  auto nominal_key = value.nominal_key;
  if (!value.nominal_key.empty() &&
      value.nominal_key.find('/') == std::string::npos)
    nominal_key = std::string(package_name) + "/" + nominal_key;
  appendAbiField(bytes, nominal_key);
  switch (value.kind) {
  case CanonicalTypeKind::Integer:
    appendAbiU32(bytes, value.arg0);
    appendAbiU32(bytes, value.arg1);
    break;
  case CanonicalTypeKind::TypeParameter:
    appendAbiU32(bytes, normalize_type_parameters ? 0U : value.arg0);
    appendAbiU32(bytes, value.arg1);
    break;
  case CanonicalTypeKind::Float:
  case CanonicalTypeKind::Reference:
  case CanonicalTypeKind::CoroutineTaskCompletionSet:
  case CanonicalTypeKind::CoroutineTaskSelection:
    appendAbiU32(bytes, value.arg0);
    break;
  case CanonicalTypeKind::Array:
    appendCanonicalType(sem_ir, package_name, CanonicalTypeId(value.arg0), bytes,
                        normalize_type_parameters);
    appendAbiU32(bytes, value.arg1);
    break;
  case CanonicalTypeKind::Slice:
    appendAbiU32(bytes, value.arg0);
    break;
  case CanonicalTypeKind::AsyncFunction:
    appendAbiU32(bytes, value.arg0);
    break;
  case CanonicalTypeKind::RawPointer:
    appendAbiU32(bytes, value.arg1);
    break;
  default:
    break;
  }
  appendAbiU32(bytes, value.callable_context_parameter);
  appendAbiOwnership(bytes, value.callable_contract);
  appendAbiU32(bytes, value.registration_authority);
  appendAbiU32(bytes, value.registration_entry_parameter);
  appendAbiU32(bytes, value.registration_userdata_parameter);
  appendAbiU32(bytes, value.registration_release_parameter);
  appendAbiU32(bytes,
               static_cast<std::uint32_t>(value.registration_bindings.size()));
  for (const auto &binding : value.registration_bindings) {
    appendAbiField(bytes, binding.name);
    appendAbiU32(bytes, binding.parameter_index);
  }
  for (const auto parameter : value.registration_arm_parameters)
    appendAbiU32(bytes, parameter);
  for (const auto parameter : value.registration_detach_parameters)
    appendAbiU32(bytes, parameter);
  appendAbiU32(bytes, static_cast<std::uint32_t>(value.elements.size()));
  for (const auto element : value.elements)
    appendCanonicalType(sem_ir, package_name, element, bytes,
                        normalize_type_parameters);
  appendAbiU32(bytes, value.foreign_resource_protocol.hasValue() ? 1U : 0U);
  if (value.foreign_resource_protocol.hasValue()) {
    const auto &protocol =
        sem_ir.genericValues().foreignResourceProtocol(
            value.foreign_resource_protocol);
    appendAbiField(bytes, encodeForeignResourceProtocol(protocol.facts));
    appendAbiU32(bytes, static_cast<std::uint32_t>(protocol.types.size()));
    for (const auto protocol_type : protocol.types)
      appendCanonicalType(sem_ir, package_name, protocol_type, bytes,
                          normalize_type_parameters);
  }
}

void appendSourceOverloadIdentity(const SemIR &sem_ir,
                                 std::string_view fallback_package,
                                 std::string_view package,
                                 const SemFunctionRef &reference,
                                 IdentifierId module, std::string_view name,
                                 std::string &bytes) {
  appendAbiField(bytes, package);
  appendAbiField(bytes, sem_ir.identifier(module));
  const auto specific_suffix = name.find("$specific$");
  appendAbiField(bytes, name.substr(0, specific_suffix));
  TypeId pattern_type = reference.local_type;
  std::uint32_t generic_parameter_count = 0;
  if (reference.generic.hasValue()) {
    const auto &generic = sem_ir.genericValues().generic(reference.generic);
    generic_parameter_count = generic.binding_count;
    for (std::uint32_t reference_index = 0;
         reference_index < sem_ir.functionRefCount(); ++reference_index) {
      const auto &candidate =
          sem_ir.functionRef(FunctionRefId(reference_index));
      if (candidate.generic == reference.generic &&
          candidate.specific == generic.self_specific) {
        pattern_type = candidate.local_type;
        break;
      }
    }
    for (std::uint32_t function_index = 0;
         function_index < sem_ir.functionCount(); ++function_index) {
      const auto &candidate = sem_ir.function(FunctionId(function_index));
      if (pattern_type == reference.local_type &&
          candidate.generic == reference.generic &&
          (candidate.flags & SemFunctionTemplate) != 0) {
        pattern_type = candidate.type;
        break;
      }
    }
  }
  appendAbiU32(bytes, generic_parameter_count);
  std::string owner_key;
  auto member_kind = PublicFunctionArtifact::MemberKind::None;
  if (reference.local_function.hasValue()) {
    const auto &function = sem_ir.function(reference.local_function);
    if (function.semantic_owner.hasValue()) {
      member_kind = PublicFunctionArtifact::MemberKind::Instance;
      const auto &owner = sem_ir.nominalType(function.semantic_owner);
      if (const auto *entity =
              sem_ir.importIRs().tryGetEntity(owner.canonical_entity)) {
        owner_key = std::string(sem_ir.identifier(entity->package_name)) +
                    "/" + std::string(sem_ir.identifier(entity->module_name)) +
                    "::" + std::string(sem_ir.identifier(entity->name));
      } else {
        owner_key = std::string(fallback_package) + "/" +
                    std::string(sem_ir.identifier(sem_ir.moduleName())) +
                    "::" + std::string(sem_ir.identifier(sem_ir.name(owner.name).text));
      }
      for (const auto &member : owner.member_functions) {
        const auto &target = sem_ir.functionRef(member.target);
        if ((reference.generic.hasValue() &&
             target.generic == reference.generic) ||
            (!reference.generic.hasValue() &&
             target.local_function == reference.local_function)) {
          member_kind =
              (member.flags & SemNominalMemberFunctionAssociated) != 0
                  ? PublicFunctionArtifact::MemberKind::Associated
                  : PublicFunctionArtifact::MemberKind::Instance;
          break;
        }
      }
    }
  }
  if (owner_key.empty()) {
    if (const auto *entity =
            sem_ir.importIRs().tryGetEntity(reference.public_entity)) {
      member_kind = entity->member_kind;
      if (entity->member_owner)
        owner_key = entity->member_owner->canonical_package + "/" +
                    entity->member_owner->canonical_module + "::" +
                    entity->member_owner->canonical_name;
    }
  }
  appendAbiField(bytes, owner_key);
  appendAbiU32(bytes, static_cast<std::uint32_t>(member_kind));
  const auto &function_type = sem_ir.type(pattern_type);
  const auto parameters = sem_ir.typeBlock(TypeBlockId(function_type.arg0));
  appendAbiU32(bytes, static_cast<std::uint32_t>(parameters.size()));
  for (const auto parameter : parameters)
    appendCanonicalType(sem_ir, fallback_package,
                        sem_ir.canonicalType(parameter), bytes, true);
}

} // namespace

std::string LLVMAbiIdentityService::callableAbiSuffix(
    const SemFunctionRef &reference, std::string_view package,
    IdentifierId module, std::string_view name, bool canonical_semantic_target,
    LLVMAbiIdentityState &state) {
  std::string source = "chtholly.next.source-overload-identity.v4:";
  if (canonical_semantic_target) {
    appendAbiField(source, package);
    appendAbiField(source, state.sem_ir.identifier(module));
    appendAbiField(source, name);
  } else {
    appendSourceOverloadIdentity(state.sem_ir, state.package_name, package,
                                 reference, module, name, source);
  }
  std::string concrete = "chtholly.next.concrete-callable-abi.v3:";
  appendCanonicalType(state.sem_ir, state.package_name,
                      state.sem_ir.canonicalType(reference.local_type),
                      concrete, false);
  return StableFingerprint::fromCanonicalBytes(source).hex().substr(0, 12) +
         StableFingerprint::fromCanonicalBytes(concrete).hex().substr(0, 12);
}

std::string LLVMAbiIdentityService::concreteTypeSuffix(
    TypeId type, LLVMAbiIdentityState &state) {
  std::string bytes = "chtholly.next.canonical-abi-type.v3:";
  appendCanonicalType(state.sem_ir, state.package_name,
                      state.sem_ir.canonicalType(type), bytes,
                      false);
  return StableFingerprint::fromCanonicalBytes(bytes).hex().substr(0, 24);
}

} // namespace chtholly::compiler
