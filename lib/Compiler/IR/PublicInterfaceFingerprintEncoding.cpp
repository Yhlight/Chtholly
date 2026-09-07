#include "PublicInterfaceEncodingInternal.h"
#include "chtholly/Compiler/SharedValueStores.h"
#include "chtholly/Support/Digest.h"

#include <array>
#include <cassert>
#include <charconv>
#include <limits>
#include <utility>

namespace chtholly::compiler::internal {

void appendU32(std::string &out, std::uint32_t value) {
  for (std::uint32_t shift = 0; shift != 32; shift += 8)
    out.push_back(static_cast<char>((value >> shift) & 0xffU));
}

void appendU64(std::string &out, std::uint64_t value) {
  for (std::uint32_t shift = 0; shift != 64; shift += 8)
    out.push_back(static_cast<char>((value >> shift) & 0xffU));
}

void appendField(std::string &out, std::string_view value) {
  assert(value.size() <= std::numeric_limits<std::uint32_t>::max());
  appendU32(out, static_cast<std::uint32_t>(value.size()));
  out.append(value);
}

void appendEntityReference(std::string &out,
                           const PublicEntityReferenceArtifact &entity) {
  appendU32(out, static_cast<std::uint32_t>(entity.kind));
  appendField(out, entity.canonical_package);
  appendField(out, entity.canonical_module);
  appendField(out, entity.canonical_name);
  appendField(out, entity.expected_fingerprint.hex());
}

void appendOwnershipSummary(std::string &out,
                            const CallableOwnershipSummary &summary);

void appendType(std::string &out, const PublicType &type) {
  appendU32(out, static_cast<std::uint32_t>(type.kind));
  appendU32(out, type.binding_index);
  if (type.kind == PublicTypeKind::Integer) {
    appendU32(out, type.scalar_width);
    appendU32(out, type.integer_signed ? 1U : 0U);
  } else if (type.kind == PublicTypeKind::Float) {
    appendU32(out, type.scalar_width);
  } else if (type.kind == PublicTypeKind::Nominal) {
    appendU32(out, static_cast<std::uint32_t>(type.nominal_entity.kind));
    appendField(out, type.nominal_entity.canonical_package);
    appendField(out, type.nominal_entity.canonical_module);
    appendField(out, type.nominal_entity.canonical_name);
    appendField(out, type.nominal_entity.expected_fingerprint.hex());
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
  } else if (type.kind == PublicTypeKind::Reference) {
    appendU32(out, static_cast<std::uint32_t>(type.reference_mutability));
    appendU32(out, static_cast<std::uint32_t>(type.reference_provenance.kind));
    appendU32(out, type.reference_provenance.index);
    assert(type.arguments.size() == 1);
    appendType(out, type.arguments.front());
  } else if (type.kind == PublicTypeKind::Array) {
    appendU32(out, type.array_bound);
    assert(type.arguments.size() == 1);
    appendType(out, type.arguments.front());
  } else if (type.kind == PublicTypeKind::Tuple) {
    appendU32(out, type.abi_union ? 1U : 0U);
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
  } else if (type.kind == PublicTypeKind::Slice) {
    appendU32(out, type.slice_mutable ? 1U : 0U);
    assert(type.arguments.size() == 1);
    appendType(out, type.arguments.front());
  } else if (type.kind == PublicTypeKind::TypeProjection) {
    appendU32(out, static_cast<std::uint32_t>(type.projection_kind));
    appendU32(out, type.projection_index);
    if (type.projection_kind == PublicTypeProjectionKind::Associated)
      appendEntityReference(out, type.nominal_entity);
    assert(type.arguments.size() == 1);
    appendType(out, type.arguments.front());
  } else if (type.kind == PublicTypeKind::RawPointer) {
    appendU32(out, type.pointer_const ? 1U : 0U);
    assert(type.arguments.size() == 1);
    appendType(out, type.arguments.front());
  } else if (type.kind == PublicTypeKind::Function) {
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
  } else if (type.kind == PublicTypeKind::CFunctionPointer) {
    appendU32(out, type.callable_variadic ? 1U : 0U);
    appendU32(out, static_cast<std::uint32_t>(type.foreign_calling_convention));
    appendU32(out, type.callable_context_parameter);
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
    appendOwnershipSummary(out, type.callable_contract);
  } else if (type.kind == PublicTypeKind::CallbackAdapter) {
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
  } else if (type.kind == PublicTypeKind::CallbackRegistration) {
    appendU32(out, type.registration_authority);
    appendU32(out, type.registration_entry_parameter);
    appendU32(out, type.registration_userdata_parameter);
    appendU32(out, type.registration_release_parameter);
    appendU32(out,
              static_cast<std::uint32_t>(type.registration_bindings.size()));
    for (const auto &binding : type.registration_bindings) {
      appendField(out, binding.name);
      appendU32(out, binding.parameter_index);
    }
    for (const auto parameter : type.registration_arm_parameters)
      appendU32(out, parameter);
    for (const auto parameter : type.registration_detach_parameters)
      appendU32(out, parameter);
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
    appendField(out,
                encodeForeignResourceProtocol(type.foreign_resource_protocol));
  } else if (type.kind == PublicTypeKind::CallbackCompletion) {
    appendU32(out, type.registration_authority);
    for (const auto parameter : type.registration_arm_parameters)
      appendU32(out, parameter);
    for (const auto parameter : type.registration_detach_parameters)
      appendU32(out, parameter);
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
    appendField(out,
                encodeForeignResourceProtocol(type.foreign_resource_protocol));
  } else if (type.kind == PublicTypeKind::CallbackWake) {
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
  } else if (type.kind == PublicTypeKind::ForeignCompletion ||
             type.kind == PublicTypeKind::ForeignWake) {
    appendU32(out, static_cast<std::uint32_t>(type.nominal_entity.kind));
    appendField(out, type.nominal_entity.canonical_package);
    appendField(out, type.nominal_entity.canonical_module);
    appendField(out, type.nominal_entity.canonical_name);
    appendField(out, type.nominal_entity.expected_fingerprint.hex());
  } else if (type.kind == PublicTypeKind::ForeignOperationState) {
    appendEntityReference(out, type.nominal_entity);
    appendU32(out, static_cast<std::uint32_t>(type.foreign_operation_state));
  }
}

std::uint16_t capabilityForRole(CallableSemanticRole role) {
  if (role == CallableSemanticRole::None || role >= CallableSemanticRole::Count)
    return CallableCapabilityNone;
  return static_cast<std::uint16_t>(1U << (static_cast<unsigned>(role) - 1U));
}

void appendSemanticContract(std::string &out,
                            const CallableSemanticContract &contract) {
  appendU32(out, static_cast<std::uint32_t>(contract.domain));
  appendU32(out, static_cast<std::uint32_t>(contract.role));
  appendU32(out, contract.capability);
  appendType(out, contract.owner);
  appendU32(out, contract.projector_field);
  appendU32(out, contract.whole_carrier ? 1U : 0U);
  appendU32(out, static_cast<std::uint32_t>(contract.carrier_path.size()));
  for (const auto field : contract.carrier_path)
    appendU32(out, field);
  appendU32(out, contract.has_bit_range ? 1U : 0U);
  appendU32(out, contract.bit_begin);
  appendU32(out, contract.bit_end);
}

void appendRegion(std::string &out, const OwnershipRegion &region) {
  appendU32(out, region.parameter_index);
  appendU32(out, static_cast<std::uint32_t>(region.path.size()));
  for (const auto step : region.path) {
    appendU32(out, static_cast<std::uint32_t>(step.kind));
    appendU32(out, step.index);
  }
  appendU32(out, region.has_bit_range ? 1U : 0U);
  appendU32(out, region.bit_begin);
  appendU32(out, region.bit_end);
}

void appendOwnershipSummary(std::string &out,
                            const CallableOwnershipSummary &summary) {
  appendU32(out, static_cast<std::uint32_t>(summary.effects.size()));
  for (const auto &effect : summary.effects) {
    appendU32(out, static_cast<std::uint32_t>(effect.kind));
    appendRegion(out, effect.region);
  }
  appendU32(out, static_cast<std::uint32_t>(summary.postconditions.size()));
  for (const auto &postcondition : summary.postconditions) {
    appendU32(out, postcondition.result_variant);
    appendRegion(out, postcondition.region);
    appendU32(out, postcondition.outcomes);
    appendU32(
        out, static_cast<std::uint32_t>(postcondition.condition.clauses.size()));
    for (const auto &clause : postcondition.condition.clauses) {
      appendU32(out, static_cast<std::uint32_t>(clause.atoms.size()));
      for (const auto &atom : clause.atoms) {
        appendU32(out, atom.variant);
        appendU32(out, atom.parameter_index);
        appendU32(out, atom.expected ? 1U : 0U);
      }
    }
  }
  appendU32(out, summary.returns_owned ? 1U : 0U);
  appendU32(out, static_cast<std::uint32_t>(summary.return_provenance.size()));
  for (const auto &source : summary.return_provenance) {
    appendRegion(out, source.region);
    appendU32(out, static_cast<std::uint32_t>(source.carrier_path.size()));
    for (const auto &step : source.carrier_path) {
      appendU32(out, static_cast<std::uint32_t>(step.kind));
      appendU32(out, step.index);
    }
    appendU32(out, static_cast<std::uint32_t>(source.condition.clauses.size()));
    for (const auto &clause : source.condition.clauses) {
      appendU32(out, static_cast<std::uint32_t>(clause.atoms.size()));
      for (const auto &atom : clause.atoms) {
        appendU32(out, atom.variant);
        appendU32(out, atom.parameter_index);
        appendU32(out, atom.expected ? 1U : 0U);
      }
    }
  }
}

void appendForeignSignature(std::string &out,
                            const ForeignAbiSignature &signature) {
  appendU32(out, signature.is_variadic ? 1U : 0U);
  appendU32(out, static_cast<std::uint32_t>(signature.unwind_policy));
  appendU32(out, static_cast<std::uint32_t>(signature.calling_convention));
  const auto append_value = [&](const ForeignAbiValue &value) {
    appendU32(out, static_cast<std::uint32_t>(value.kind));
    appendU32(out, value.width);
    appendU32(out, value.pointee_const ? 1U : 0U);
  };
  append_value(signature.result);
  appendU32(out, static_cast<std::uint32_t>(signature.parameters.size()));
  for (const auto &parameter : signature.parameters)
    append_value(parameter);
}

void appendTemplate(std::string &out,
                    const GenericTemplateArtifact &generic_template) {
  appendU32(out, generic_template.generic_parameter_count);
  appendU32(out, generic_template.parameter_count);
  appendU32(out,
            static_cast<std::uint32_t>(generic_template.local_types.size()));
  for (const auto type : generic_template.local_types)
    appendType(out, type);
  for (const auto flags : generic_template.local_flags)
    appendU32(out, flags);
  appendU32(out, static_cast<std::uint32_t>(generic_template.integers.size()));
  for (const auto value : generic_template.integers) {
    const auto bits = static_cast<std::uint64_t>(value);
    appendU32(out, static_cast<std::uint32_t>(bits));
    appendU32(out, static_cast<std::uint32_t>(bits >> 32U));
  }
  appendU32(out, static_cast<std::uint32_t>(generic_template.strings.size()));
  for (const auto &value : generic_template.strings)
    appendField(out, value);
  appendU32(out,
            static_cast<std::uint32_t>(generic_template.type_queries.size()));
  for (const auto &query : generic_template.type_queries) {
    out.push_back(static_cast<char>(query.kind));
    appendType(out, query.source);
    appendType(out, query.other);
    appendField(out, query.property);
  }
  appendU32(out, static_cast<std::uint32_t>(generic_template.callees.size()));
  for (const auto &callee : generic_template.callees) {
    appendU32(out, static_cast<std::uint32_t>(callee.kind));
    appendField(out, callee.canonical_package);
    appendField(out, callee.canonical_module);
    appendField(out, callee.canonical_name);
    appendField(out, callee.expected_fingerprint.hex());
  }
  appendU32(out, static_cast<std::uint32_t>(
                     generic_template.callee_type_arguments.size()));
  for (const auto &arguments : generic_template.callee_type_arguments) {
    appendU32(out, static_cast<std::uint32_t>(arguments.size()));
    for (const auto &argument : arguments)
      appendType(out, argument);
  }
  const auto append_region =
      [&](const GenericEvaluationRegionArtifact &region) {
        appendU32(out, region.entry_block);
        appendU32(out, static_cast<std::uint32_t>(region.results.size()));
        for (const auto type : region.results)
          appendType(out, type);
        appendU32(out, static_cast<std::uint32_t>(region.instructions.size()));
        for (const auto &instruction : region.instructions) {
          appendU32(out, static_cast<std::uint32_t>(instruction.opcode));
          appendType(out, instruction.type);
          appendU32(out, instruction.arg0);
          appendU32(out, instruction.arg1);
        }
        appendU32(out, static_cast<std::uint32_t>(region.blocks.size()));
        for (const auto &block : region.blocks) {
          appendU32(out, static_cast<std::uint32_t>(block.size()));
          for (const auto instruction : block)
            appendU32(out, instruction);
        }
        appendU32(out, static_cast<std::uint32_t>(
                           region.instruction_value_blocks.size()));
        for (const auto &block : region.instruction_value_blocks) {
          appendU32(out, static_cast<std::uint32_t>(block.size()));
          for (const auto instruction : block)
            appendU32(out, instruction);
        }
      };
  append_region(generic_template.declaration);
  append_region(generic_template.definition);
}

void appendConstantValue(std::string &out, const PublicConstantValue &value) {
  appendU32(out, static_cast<std::uint32_t>(value.kind));
  appendType(out, value.type);
  appendU32(out, static_cast<std::uint32_t>(value.payload));
  appendU32(out, static_cast<std::uint32_t>(value.payload >> 32U));
  appendField(out, value.string_payload);
  appendU32(out, value.target_dependent ? 1U : 0U);
  appendU32(out, static_cast<std::uint32_t>(value.elements.size()));
  for (const auto &element : value.elements)
    appendConstantValue(out, element);
}

StableFingerprint entityFingerprint(
    std::string_view package, std::string_view module, std::string_view name,
    const std::optional<PublicEntityReferenceArtifact> &member_owner,
    PublicFunctionArtifact::MemberKind member_kind,
    std::uint32_t generic_parameter_count,
    std::span<const PublicType> parameters, PublicType return_type,
    const std::optional<PublicType> &error_type,
    PublicFunctionExecutionKind execution_kind,
    const PublicCoroutineConstructorABI &coroutine_constructor,
    const PublicNominalConstructorABI &nominal_constructor,
    const CallableSemanticContract &semantic_contract,
    CompilerIntrinsicRole intrinsic_role,
    const CallableOwnershipSummary &ownership_summary,
    const std::optional<GenericTemplateArtifact> &generic_template,
    PublicCallableDeclarationKind declaration_kind, bool is_unsafe, bool is_const,
    std::string_view foreign_abi,
    const std::optional<ForeignAbiSignature> &foreign_signature,
    std::span<const std::string> parameter_names,
    std::span<const std::optional<PublicConstantValue>> default_arguments,
    std::span<const PublicInterfaceConstraintArtifact> constraints,
    const std::optional<interop::ArtifactReference> &interop_artifact,
    std::string_view external_symbol) {
  std::string input;
  appendField(input, "chtholly.next.public-entity.v33");
  appendField(input, package);
  appendField(input, module);
  appendU32(input, 0); // Function entity kind.
  appendField(input, name);
  appendU32(input, member_owner.has_value() ? 1U : 0U);
  if (member_owner)
    appendEntityReference(input, *member_owner);
  appendU32(input, static_cast<std::uint32_t>(member_kind));
  appendU32(input, generic_parameter_count);
  appendU32(input, static_cast<std::uint32_t>(parameters.size()));
  for (const auto parameter : parameters)
    appendType(input, parameter);
  appendType(input, return_type);
  appendU32(input, static_cast<std::uint32_t>(execution_kind));
  appendU32(input, coroutine_constructor.epoch);
  appendU32(input, coroutine_constructor.eager_start ? 1U : 0U);
  appendU32(input, coroutine_constructor.left_to_right_exactly_once ? 1U : 0U);
  appendU32(input, coroutine_constructor.supports_root ? 1U : 0U);
  appendU32(input, coroutine_constructor.supports_child ? 1U : 0U);
  appendU32(input, nominal_constructor.epoch);
  appendU32(input, static_cast<std::uint32_t>(nominal_constructor.result_kind));
  appendU32(input, error_type.has_value() ? 1U : 0U);
  if (error_type)
    appendType(input, *error_type);
  appendSemanticContract(input, semantic_contract);
  appendU32(input, static_cast<std::uint32_t>(intrinsic_role));
  appendOwnershipSummary(input, ownership_summary);
  appendU32(input, static_cast<std::uint32_t>(declaration_kind));
  appendU32(input, is_unsafe ? 1U : 0U);
  appendU32(input, is_const ? 1U : 0U);
  appendField(input, foreign_abi);
  appendField(input, external_symbol);
  appendU32(input, foreign_signature.has_value() ? 1U : 0U);
  if (foreign_signature)
    appendForeignSignature(input, *foreign_signature);
  appendU32(input, static_cast<std::uint32_t>(parameter_names.size()));
  for (const auto &parameter_name : parameter_names)
    appendField(input, parameter_name);
  appendU32(input, static_cast<std::uint32_t>(default_arguments.size()));
  for (const auto &default_argument : default_arguments) {
    appendU32(input, default_argument.has_value() ? 1U : 0U);
    if (default_argument)
      appendConstantValue(input, *default_argument);
  }
  appendU32(input, generic_template.has_value() ? 1U : 0U);
  if (generic_template)
    appendTemplate(input, *generic_template);
  appendU32(input, static_cast<std::uint32_t>(constraints.size()));
  for (const auto &constraint : constraints) {
    appendType(input, constraint.subject);
    appendEntityReference(input, constraint.interface_entity);
    appendU32(input, static_cast<std::uint32_t>(constraint.arguments.size()));
    for (const auto &argument : constraint.arguments)
      appendType(input, argument);
  }
  appendU32(input, interop_artifact.has_value() ? 1U : 0U);
  if (interop_artifact) {
    appendU32(input, interop_artifact->schema_epoch);
    appendField(input, interop_artifact->canonical_package);
    appendField(input, interop_artifact->canonical_module);
    appendField(input, interop_artifact->canonical_name);
    appendField(input, interop_artifact->fingerprint.hex());
  }
  return StableFingerprint::fromCanonicalBytes(input);
}

StableFingerprint valueFingerprint(const PublicValueArtifact &value) {
  std::string input;
  appendField(input, "chtholly.next.public-value.v2");
  appendU32(input, static_cast<std::uint32_t>(value.kind));
  appendField(input, value.canonical_package);
  appendField(input, value.canonical_module);
  appendField(input, value.canonical_name);
  appendType(input, value.type);
  appendConstantValue(input, value.value);
  return StableFingerprint::fromCanonicalBytes(input);
}

StableFingerprint artifactFingerprint(
    std::string_view package, std::string_view module,
    std::span<const PublicFunctionArtifact> functions,
    std::span<const PublicNominalTypeArtifact> nominal_types,
    std::span<const PublicValueArtifact> values,
    std::span<const PublicInterfaceDeclarationArtifact> interfaces,
    std::span<const PublicTypeAliasArtifact> type_aliases,
    std::span<const PublicInterfaceWitnessArtifact> interface_witnesses) {
  std::string input;
  appendField(input, "chtholly.next.public-interface.v31");
  appendField(input, package);
  appendField(input, module);
  appendU32(input, static_cast<std::uint32_t>(functions.size()));
  for (const auto &function : functions) {
    appendField(input, function.name);
    appendU32(input, function.member_owner.has_value() ? 1U : 0U);
    if (function.member_owner)
      appendEntityReference(input, *function.member_owner);
    appendU32(input, static_cast<std::uint32_t>(function.member_kind));
    appendField(input, function.canonical_package);
    appendField(input, function.canonical_module);
    appendU32(input, 0);
    appendField(input, function.canonical_name);
    appendField(input, function.entity_fingerprint.hex());
  }
  appendU32(input, static_cast<std::uint32_t>(nominal_types.size()));
  for (const auto &nominal : nominal_types) {
    appendU32(input, static_cast<std::uint32_t>(PublicEntityKind::NominalType));
    appendField(input, nominal.entity.canonical_package);
    appendField(input, nominal.entity.canonical_module);
    appendField(input, nominal.entity.canonical_name);
    appendField(input, nominal.definition_fingerprint.hex());
  }
  appendU32(input, static_cast<std::uint32_t>(values.size()));
  for (const auto &value : values) {
    appendU32(input, static_cast<std::uint32_t>(value.kind));
    appendField(input, value.name);
    appendField(input, value.canonical_package);
    appendField(input, value.canonical_module);
    appendField(input, value.canonical_name);
    appendField(input, value.entity_fingerprint.hex());
  }
  appendU32(input, static_cast<std::uint32_t>(interfaces.size()));
  for (const auto &declaration : interfaces) {
    appendEntityReference(input, declaration.entity);
    appendU32(input, declaration.generic_parameter_count);
    appendU32(input, declaration.explicit_parameter_count);
  }
  appendU32(input, static_cast<std::uint32_t>(type_aliases.size()));
  for (const auto &alias : type_aliases) {
    appendEntityReference(input, alias.entity);
    appendType(input, alias.target);
  }
  appendU32(input, static_cast<std::uint32_t>(interface_witnesses.size()));
  for (const auto &witness : interface_witnesses)
    appendField(input, witness.fingerprint.hex());
  return StableFingerprint::fromCanonicalBytes(input);
}

StableFingerprint interfaceFingerprint(
    const PublicInterface &interface_value,
    const PublicInterfaceRegistry &registry, const SharedValueStores &values) {
  std::vector<PublicFunctionArtifact> functions;
  functions.reserve(interface_value.bindingCount());
  for (std::uint32_t index = 0; index < interface_value.bindingCount(); ++index) {
    const auto &binding = interface_value.function(PublicBindingId(index));
    const auto *entity = registry.tryGetEntity(binding.canonical_entity);
    assert(entity);
    functions.push_back(
        {.name = std::string(values.identifier(binding.name)),
         .member_owner = binding.member_owner,
         .member_kind = binding.member_kind,
         .canonical_package =
             std::string(values.identifier(entity->package_name)),
         .canonical_module =
             std::string(values.identifier(entity->module_name)),
         .canonical_name = std::string(values.identifier(entity->name)),
         .generic_parameter_count = entity->generic_parameter_count,
         .parameters = entity->parameters,
         .entity_fingerprint = entity->fingerprint});
  }
  std::ranges::sort(functions, [](const auto &lhs, const auto &rhs) {
    const auto key = [](const auto &function) {
      std::string signature;
      for (const auto &parameter : function.parameters)
        appendType(signature, parameter);
      return std::tuple(
          function.member_owner.has_value(),
          function.member_owner ? function.member_owner->canonical_package
                                : std::string{},
          function.member_owner ? function.member_owner->canonical_module
                                : std::string{},
          function.member_owner ? function.member_owner->canonical_name
                                : std::string{},
          function.name, function.generic_parameter_count,
          std::move(signature));
    };
    return key(lhs) < key(rhs);
  });
  auto nominal_types = std::vector<PublicNominalTypeArtifact>(
      interface_value.nominalArtifacts().begin(),
      interface_value.nominalArtifacts().end());
  std::ranges::sort(nominal_types, {}, [](const auto &value) {
    return std::tie(value.entity.canonical_package,
                    value.entity.canonical_module, value.entity.canonical_name);
  });
  auto public_values = std::vector<PublicValueArtifact>(
      interface_value.valueArtifacts().begin(), interface_value.valueArtifacts().end());
  std::ranges::sort(public_values, {}, &PublicValueArtifact::name);
  auto interface_witnesses = std::vector<PublicInterfaceWitnessArtifact>(
      interface_value.interfaceWitnessArtifacts().begin(),
      interface_value.interfaceWitnessArtifacts().end());
  // PublicInterfaceArtifact canonicalizes witness order by fingerprint. Keep
  // the enclosing interface fingerprint on the same canonical order so a
  // module with more than one primitive conformance cannot fail round-trip
  // verification merely because source declaration order differs.
  std::ranges::sort(interface_witnesses, {},
                    [](const auto &value) { return value.fingerprint.hex(); });
  auto interface_declarations = std::vector<PublicInterfaceDeclarationArtifact>(
      interface_value.interfaceArtifacts().begin(),
      interface_value.interfaceArtifacts().end());
  std::ranges::sort(interface_declarations, {},
                    [](const auto &value) { return value.entity.canonical_name; });
  auto type_aliases = std::vector<PublicTypeAliasArtifact>(
      interface_value.typeAliasArtifacts().begin(),
      interface_value.typeAliasArtifacts().end());
  std::ranges::sort(type_aliases, {},
                    [](const auto &value) { return value.entity.canonical_name; });
  return artifactFingerprint(
      values.identifier(interface_value.packageName()),
      values.identifier(interface_value.moduleName()), functions, nominal_types,
      public_values, interface_declarations, type_aliases, interface_witnesses);
}

} // namespace chtholly::compiler::internal

namespace chtholly::compiler {

StableFingerprint
StableFingerprint::fromCanonicalBytes(std::string_view input) {
  const auto hex = chtholly::sha256Hex(input);
  std::array<std::uint8_t, StableFingerprint::ByteCount> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    unsigned int value = 0;
    const auto first = hex.data() + index * 2;
    const auto parsed = std::from_chars(first, first + 2, value, 16);
    assert(parsed.ec == std::errc{} && parsed.ptr == first + 2);
    bytes[index] = static_cast<std::uint8_t>(value);
  }
  return StableFingerprint(bytes);
}

StableFingerprint publicTypeFingerprint(const PublicType &type) {
  std::string canonical;
  internal::appendField(canonical, "chtholly.next.public-type.v2");
  internal::appendType(canonical, type);
  return StableFingerprint::fromCanonicalBytes(canonical);
}

} // namespace chtholly::compiler
