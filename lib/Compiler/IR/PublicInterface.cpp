#include "chtholly/Compiler/PublicInterface.h"

#include "ArtifactDecodeInternal.h"
#include "PublicInterfaceConstructionInternal.h"
#include "PublicInterfaceEncodingInternal.h"
#include "chtholly/Compiler/BuiltinOperator.h"
#include "chtholly/Compiler/NominalTypeArtifact.h"
#include "chtholly/Compiler/SemIR.h"
#include "chtholly/Compiler/SharedValueStores.h"

#include "PublicInterfaceServices.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace chtholly::compiler {
using internal::appendEntityReference;
using internal::appendField;
using internal::appendForeignSignature;
using internal::appendOwnershipSummary;
using internal::appendSemanticContract;
using internal::appendTemplate;
using internal::appendType;
using internal::appendU32;
using internal::appendU64;
using internal::appendConstantValue;
using internal::artifactFingerprint;
using internal::entityFingerprint;
using internal::valueFingerprint;
using internal::interfaceFingerprint;
using internal::ForeignNominalResolver;
using internal::classifyForeignType;
using internal::foreignSignatureMatches;
using internal::memberSignatureMatchesOwner;
using internal::normalizePublicOwnershipSummary;
using internal::ownershipMatchesSignature;
using internal::semanticContractMatchesEffects;
using internal::semanticContractMatchesSignature;
using internal::validCoroutineConstructorABI;
using internal::validNominalConstructorABI;
using internal::validReferenceProvenance;
using internal::OwnershipNominalDefinition;
using internal::OwnershipNominalResolver;
using internal::validCallbackOwnershipTypes;
using internal::validOwnershipSummaryTypes;
using internal::validReturnLoanTypes;
using internal::templateMatchesSignature;
using internal::canonicalDefaultArguments;
using internal::canonicalParameterNames;
using internal::interfaceDeclarationFingerprint;
using internal::interfaceWitnessFingerprint;
using internal::interfaceWitnessesMayOverlap;
using internal::publicTypesMayOverlap;
using internal::sameSignature;
using internal::typeAliasFingerprint;

namespace {

std::string moduleKey(std::string_view package, std::string_view module) {
  return internal::PublicInterfaceIdentityService::moduleKey(package, module);
}

std::string entityKey(std::string_view package, std::string_view module,
                      std::string_view name,
                      PublicEntityKind kind = PublicEntityKind::Function) {
  return internal::PublicInterfaceIdentityService::entityKey(package, module,
                                                             name, kind);
}

std::string overloadEntityKey(
    std::string_view package, std::string_view module, std::string_view name,
    const std::optional<PublicEntityReferenceArtifact> &member_owner,
    PublicFunctionArtifact::MemberKind member_kind,
    std::uint32_t generic_parameter_count,
    std::span<const PublicType> parameters) {
  return internal::PublicInterfaceIdentityService::overloadEntityKey(
      package, module, name, member_owner, member_kind,
      generic_parameter_count, parameters);
}

std::string memberFunctionBindingKey(
    const PublicEntityReferenceArtifact &owner, std::string_view name) {
  return internal::PublicInterfaceIdentityService::memberFunctionBindingKey(
      owner, name);
}

void appendInterfaceConstraint(
    std::string &out, const PublicInterfaceConstraintArtifact &constraint) {
  appendType(out, constraint.subject);
  appendEntityReference(out, constraint.interface_entity);
  appendU32(out, static_cast<std::uint32_t>(constraint.arguments.size()));
  for (const auto &argument : constraint.arguments)
    appendType(out, argument);
}

bool validPublicType(const PublicType &type,
                     std::uint32_t generic_parameter_count, bool allow_void);
bool validEntityReference(const PublicEntityReferenceArtifact &entity,
                          PublicEntityKind expected);

bool validForeignOperation(const interop::ArtifactReference &reference,
                           std::span<const PublicType> parameters,
                           PublicType result) {
  (void)parameters;
  (void)result;
  std::string error;
  return reference.verify(error);
}

StableFingerprint foreignOperationFingerprint(
    std::span<const PublicType> parameters, PublicType result,
    interop::ForeignOperationArtifact operation) {
  return internal::PublicInterfaceForeignOperationService::fingerprint(
      parameters, result, std::move(operation));
}

bool validForeignOperation(const interop::ForeignOperationArtifact &operation,
                           std::span<const PublicType> parameters,
                           PublicType result) {
  return internal::PublicInterfaceForeignOperationService::valid(
      operation, parameters, result);
}

bool validPublicType(const PublicType &type,
                     std::uint32_t generic_parameter_count,
                     bool allow_void = false) {
  return internal::PublicInterfaceTypeValidationService::validPublicType(
      type, generic_parameter_count, allow_void);
}

bool validEntityReference(const PublicEntityReferenceArtifact &entity,
                          PublicEntityKind expected) {
  return internal::PublicInterfaceTypeValidationService::validEntityReference(
      entity, expected);
}

bool validConstantValue(const PublicConstantValue &value,
                        std::uint32_t depth = 0) {
  return internal::PublicInterfaceConstantValidationService::validValue(value,
                                                                         depth);
}

bool validConstantShape(
    const PublicConstantValue &value,
    std::span<const PublicNominalTypeArtifact> nominal_types) {
  return internal::PublicInterfaceConstantValidationService::validShape(
      value, nominal_types);
}

bool validInterfaceConstraint(
    const PublicInterfaceConstraintArtifact &constraint,
    std::uint32_t generic_parameter_count) {
  return internal::PublicInterfaceTypeValidationService::validInterfaceConstraint(
      constraint, generic_parameter_count);
}

} // namespace

bool internal::GenericTemplateValidationService::validType(
    const PublicType &type, std::uint32_t generic_count, bool allow_void) {
  return validPublicType(type, generic_count, allow_void);
}

bool internal::GenericTemplateValidationService::validReferenceType(
    const PublicType &type, std::size_t parameter_count,
    bool allow_parameter) {
  return validReferenceProvenance(type, parameter_count, allow_parameter);
}

bool validCallbackRegistrationContract(const PublicType &type) {
  return internal::PublicInterfaceTypeValidationService::
      validCallbackRegistrationContract(type);
}

bool validCallbackCompletionContract(const PublicType &type) {
  return internal::PublicInterfaceTypeValidationService::
      validCallbackCompletionContract(type);
}

bool validCallbackWakeContract(const PublicType &type) {
  return internal::PublicInterfaceTypeValidationService::
      validCallbackWakeContract(type);
}

bool CallableSemanticContract::verify(std::uint32_t generic_parameter_count,
                                      std::string &error) const {
  return internal::PublicInterfaceValidationService::callableSemanticContract(
      *this, generic_parameter_count, error,
      [](const PublicType &type, std::uint32_t generic_count) {
        return validPublicType(type, generic_count);
      });
}

PublicInterfaceArtifact::PublicInterfaceArtifact(
    std::string package_name, std::string module_name,
    StableFingerprint fingerprint,
    std::vector<PublicFunctionArtifact> functions,
    std::vector<PublicNominalTypeArtifact> nominal_types,
    std::vector<PublicValueArtifact> values,
    std::vector<PublicInterfaceDeclarationArtifact> interfaces,
    std::vector<PublicTypeAliasArtifact> type_aliases,
    std::vector<PublicInterfaceWitnessArtifact> interface_witnesses)
    : package_name_(std::move(package_name)),
      module_name_(std::move(module_name)), fingerprint_(fingerprint),
      functions_(std::move(functions)),
      nominal_types_(std::move(nominal_types)), values_(std::move(values)),
      interfaces_(std::move(interfaces)),
      type_aliases_(std::move(type_aliases)),
      interface_witnesses_(std::move(interface_witnesses)) {
  std::ranges::sort(functions_, [](const auto &lhs, const auto &rhs) {
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
          function.name, function.member_kind, function.generic_parameter_count,
          std::move(signature));
    };
    return key(lhs) < key(rhs);
  });
  std::ranges::sort(nominal_types_, {}, [](const auto &value) {
    return std::tie(value.entity.canonical_package,
                    value.entity.canonical_module, value.entity.canonical_name);
  });
  std::ranges::sort(values_, {}, &PublicValueArtifact::name);
  std::ranges::sort(interfaces_, {}, [](const auto &value) {
    return value.entity.canonical_name;
  });
  std::ranges::sort(type_aliases_, {}, [](const auto &value) {
    return value.entity.canonical_name;
  });
  std::ranges::sort(interface_witnesses_, {},
                    [](const auto &value) { return value.fingerprint.hex(); });
}

bool PublicInterfaceArtifact::verify(std::string &error) const {
  return internal::PublicInterfaceVerifyService::artifact(*this, error);
}

internal::PublicInterfaceArtifactVerificationService::VerificationCallbacks
makeArtifactVerificationCallbacks(const PublicInterfaceArtifact &artifact) {
  const auto *artifact_ptr = &artifact;
  ForeignNominalResolver resolve_foreign_nominal;
  resolve_foreign_nominal =
      [artifact_ptr](const PublicType &type) -> std::optional<PublicType> {
    return internal::PublicInterfaceResolverService::foreignNominal(*artifact_ptr,
                                                                      type);
  };
  const auto resolve_ownership_nominal =
      [artifact_ptr](const PublicEntityReferenceArtifact &ref)
      -> std::optional<OwnershipNominalDefinition> {
    const auto *nominal =
        internal::PublicInterfaceResolverService::ownershipNominal(*artifact_ptr, ref);
    if (!nominal)
      return std::nullopt;
    return OwnershipNominalDefinition{nominal->generic_parameter_count,
                                      nominal->fields, nominal->variants,
                                      nominal->definition_fingerprint};
  };
  const internal::PublicInterfaceArtifactVerificationService::
      FunctionValidationCallbacks function_callbacks{
          [](const PublicType &type, std::uint32_t generic_count,
             bool allow_void) {
            return validPublicType(type, generic_count, allow_void);
          },
          [](PublicFunctionExecutionKind kind,
             const PublicCoroutineConstructorABI &abi) {
            return validCoroutineConstructorABI(kind, abi);
          },
          [](const CallableSemanticContract &contract, const PublicType &result,
             const PublicNominalConstructorABI &abi) {
            return validNominalConstructorABI(contract, result, abi);
          },
          [](const PublicType &type, std::size_t parameter_count,
             bool allow_parameter) {
            return validReferenceProvenance(type, parameter_count,
                                            allow_parameter);
          },
          [](std::span<const PublicType> parameters, const PublicType &result,
             const CallableSemanticContract &contract) {
            return semanticContractMatchesSignature(parameters, result,
                                                    contract);
          },
          [](const CallableSemanticContract &contract,
             const CallableOwnershipSummary &summary) {
            return semanticContractMatchesEffects(contract, summary);
          },
          [](std::span<const PublicType> parameters, const PublicType &result,
             const CallableOwnershipSummary &summary) {
            return ownershipMatchesSignature(parameters, result, summary);
          },
          [resolve_ownership_nominal](
              std::span<const PublicType> parameters, const PublicType &result,
              const CallableOwnershipSummary &summary) {
            return validReturnLoanTypes(parameters, result, summary,
                                        resolve_ownership_nominal);
          },
          [resolve_foreign_nominal](
              std::span<const PublicType> parameters, const PublicType &result,
              const std::optional<ForeignAbiSignature> &signature,
              bool allows_hidden_parameters) {
            return foreignSignatureMatches(parameters, result, signature,
                                           resolve_foreign_nominal,
                                           allows_hidden_parameters);
          },
          [artifact_ptr](const PublicConstantValue &value) {
            return validConstantShape(value, artifact_ptr->nominalTypes());
          },
          [](const interop::ArtifactReference &reference,
             std::span<const PublicType> parameters, PublicType result) {
            return validForeignOperation(reference, parameters, result);
          },
          [](const std::optional<PublicEntityReferenceArtifact> &owner,
             PublicFunctionArtifact::MemberKind kind,
             std::span<const PublicType> parameters) {
            return memberSignatureMatchesOwner(owner, kind, parameters);
          },
          [](const GenericTemplateArtifact &generic, std::uint32_t count,
             std::span<const PublicType> parameters, PublicType result) {
            return templateMatchesSignature(generic, count, parameters,
                                            result);
          },
          [](const PublicInterfaceConstraintArtifact &constraint,
             std::uint32_t count) {
            return validInterfaceConstraint(constraint, count);
          }};
  const internal::PublicInterfaceArtifactVerificationService::VerificationCallbacks callbacks{
      function_callbacks,
      [&](const PublicFunctionArtifact &value) {
        std::string signature_key;
        for (const auto &parameter : value.parameters)
          appendType(signature_key, parameter);
        return std::tuple(
            value.member_owner.has_value(),
            value.member_owner
                ? std::string_view(value.member_owner->canonical_package)
                : std::string_view{},
            value.member_owner
                ? std::string_view(value.member_owner->canonical_module)
                : std::string_view{},
            value.member_owner
                ? std::string_view(value.member_owner->canonical_name)
                : std::string_view{},
            std::string_view(value.name), value.member_kind,
            value.generic_parameter_count, std::move(signature_key));
      },
      [&](const PublicFunctionArtifact &value) {
        return entityFingerprint(
            value.canonical_package, value.canonical_module,
            value.canonical_name, value.member_owner,
            value.member_kind, value.generic_parameter_count,
            value.parameters, value.return_type, value.error_type,
            value.execution_kind, value.coroutine_constructor,
            value.nominal_constructor, value.semantic_contract,
            value.intrinsic_role, value.ownership_summary,
            value.generic_template, value.declaration_kind,
            value.is_unsafe, value.is_const, value.foreign_abi,
            value.foreign_signature, value.parameter_names,
            value.default_arguments, value.constraints,
            value.interop_artifact, value.external_symbol);
      },
      [](const PublicNominalTypeArtifact &nominal, std::string &err) {
        return nominal.verify(err);
      },
      [](const PublicEntityReferenceArtifact &reference,
         PublicEntityKind kind) { return validEntityReference(reference, kind); },
      [](const PublicInterfaceConstraintArtifact &constraint,
         std::uint32_t generic_count) {
        return validInterfaceConstraint(constraint, generic_count);
      },
      [](const PublicType &type, std::uint32_t generic_count,
         bool allow_dependent) {
        return validPublicType(type, generic_count, allow_dependent);
      },
      [](const PublicInterfaceDeclarationArtifact &declaration) {
        return interfaceDeclarationFingerprint(declaration);
      },
      [](const PublicTypeAliasArtifact &alias) { return typeAliasFingerprint(alias); },
      [](const PublicInterfaceWitnessArtifact &witness) {
        return interfaceWitnessFingerprint(witness);
      },
      [](const PublicConstantValue &value,
         std::span<const PublicNominalTypeArtifact> nominals) {
        return validConstantShape(value, nominals);
      },
      [](const PublicValueArtifact &value) { return valueFingerprint(value); },
      [](std::string_view package, std::string_view module,
         std::span<const PublicFunctionArtifact> functions,
         std::span<const PublicNominalTypeArtifact> nominals,
         std::span<const PublicValueArtifact> values,
         std::span<const PublicInterfaceDeclarationArtifact> interfaces,
         std::span<const PublicTypeAliasArtifact> aliases,
         std::span<const PublicInterfaceWitnessArtifact> witnesses) {
        return artifactFingerprint(package, module, functions, nominals, values,
                                   interfaces, aliases, witnesses);
      }};
  return callbacks;
}

bool PublicInterface::verify(std::string &error) const {
  const internal::PublicInterfaceVerifyService::ValueVerificationState state{
      [](const PublicType &type, std::uint32_t generic_count,
         bool allow_void) {
        return validPublicType(type, generic_count, allow_void);
      },
      [](const PublicType &type, std::size_t parameter_count,
         bool allow_parameter) {
        return validReferenceProvenance(type, parameter_count, allow_parameter);
      },
      [](const PublicConstantValue &value,
         std::span<const PublicNominalTypeArtifact> nominals) {
        return validConstantShape(value, nominals);
      },
      [](const PublicValueArtifact &value) { return valueFingerprint(value); },
      [](const PublicEntityReferenceArtifact &owner, std::string_view name) {
        return internal::PublicInterfaceIdentityService::memberFunctionBindingKey(
            owner, name);
      }};
  return internal::PublicInterfaceVerifyService::interfaceValue(*this, error,
                                                                  state);
}

PublicInterfaceId PublicInterfaceRegistry::registerExternalArtifact(
    const PublicInterfaceArtifact &artifact, std::string &error) {
  return registerArtifact(CheckIRId::invalid(), artifact, error);
}

bool PublicInterfaceRegistry::registerArtifactClosure(
    std::span<const PublicInterfaceArtifact *const> artifacts,
    std::string &error) {
  return internal::PublicInterfaceRegistryService::registerArtifactClosure(
      *this, artifacts, error);
}

PublicInterfaceArtifact
PublicInterfaceRegistry::buildArtifact(PublicInterfaceId id,
                                       std::string &error) const {
  return internal::PublicInterfaceArtifactBuildService::build(*this, id, error);
}

const PublicInterface *
PublicInterfaceRegistry::tryGet(PublicInterfaceId id) const {
  return id.hasValue() && id.index < interfaces_.size()
             ? interfaces_[id.index].get()
             : nullptr;
}

const PublicEntity *
PublicInterfaceRegistry::tryGetEntity(PublicEntityId id) const {
  return entities_.tryGet(id);
}

PublicEntityId PublicInterfaceRegistry::findEntity(
    std::string_view canonical_package, std::string_view canonical_module,
    std::string_view canonical_name, PublicEntityKind kind,
    StableFingerprint expected_fingerprint) const {
  const auto found = entity_keys_.find(
      entityKey(canonical_package, canonical_module, canonical_name, kind));
  if (found != entity_keys_.end() &&
      (!expected_fingerprint.hasValue() ||
       entities_.get(found->second).fingerprint == expected_fingerprint))
    return found->second;
  PublicEntityId result;
  for (std::uint32_t index = 0; index < entities_.size(); ++index) {
    const auto id = PublicEntityId(index);
    const auto &entity = entities_.get(id);
    if (entity.kind != kind ||
        values_->identifier(entity.package_name) != canonical_package ||
        values_->identifier(entity.module_name) != canonical_module ||
        values_->identifier(entity.name) != canonical_name ||
        (expected_fingerprint.hasValue() &&
         entity.fingerprint != expected_fingerprint))
      continue;
    if (result.hasValue())
      return PublicEntityId::invalid();
    result = id;
  }
  return result;
}

PublicInterfaceId
PublicInterfaceRegistry::findByModule(IdentifierId package_name,
                                      IdentifierId module_name) const {
  if (!package_name.hasValue() || !module_name.hasValue() ||
      package_name.index >= values_->identifierCount() ||
      module_name.index >= values_->identifierCount())
    return PublicInterfaceId::invalid();
  const auto found = modules_.find(moduleKey(values_->identifier(package_name),
                                             values_->identifier(module_name)));
  return found == modules_.end() ? PublicInterfaceId::invalid() : found->second;
}

PublicInterfaceId
PublicInterfaceRegistry::findByCheckIR(CheckIRId check_ir_id) const {
  const auto found = check_irs_.find(check_ir_id.index);
  return found == check_irs_.end() ? PublicInterfaceId::invalid()
                                   : found->second;
}

bool PublicInterfaceRegistry::verifyOwnershipSummaryTypes(
    std::span<const PublicType> parameters, const PublicType &return_type,
    const CallableOwnershipSummary &summary, std::string &error) const {
  const auto resolve_nominal = [&](const PublicEntityReferenceArtifact &ref)
      -> std::optional<OwnershipNominalDefinition> {
    const auto id =
        findEntity(ref.canonical_package, ref.canonical_module,
                   ref.canonical_name, PublicEntityKind::NominalType);
    const auto *entity = tryGetEntity(id);
    if (!entity || entity->kind != PublicEntityKind::NominalType)
      return std::nullopt;
    return OwnershipNominalDefinition{
        entity->generic_parameter_count, entity->nominal_fields,
        entity->nominal_variants, entity->fingerprint};
  };
  if (!validOwnershipSummaryTypes(parameters, summary, resolve_nominal) ||
      !validReturnLoanTypes(parameters, return_type, summary,
                            resolve_nominal)) {
    error = "callable ownership summary has a type-invalid region path";
    return false;
  }
  return true;
}

bool PublicInterfaceRegistry::verify(std::string &error) const {
  return internal::PublicInterfaceRegistryService::verify(
      *this, error,
      {
          .template_matches_signature =
              [](const GenericTemplateArtifact &generic_template,
                 std::uint32_t generic_count,
                 std::span<const PublicType> parameters,
                 PublicType return_type) {
                return templateMatchesSignature(generic_template, generic_count,
                                                parameters, return_type);
              },
          .valid_type =
              [](const PublicType &type, std::uint32_t generic_count,
                 bool allow_void) {
                return validPublicType(type, generic_count, allow_void);
              },
          .valid_coroutine_constructor = validCoroutineConstructorABI,
          .valid_nominal_constructor = validNominalConstructorABI,
          .valid_reference_provenance = validReferenceProvenance,
          .semantic_contract_matches_signature =
              semanticContractMatchesSignature,
          .semantic_contract_matches_effects = semanticContractMatchesEffects,
          .ownership_matches_signature = ownershipMatchesSignature,
          .foreign_signature_matches =
              [](std::span<const PublicType> parameters,
                 const PublicType &return_type,
                 const std::optional<ForeignAbiSignature> &signature,
                 const internal::PublicInterfaceRegistryService::
                     NominalResolver &resolve_nominal,
                 bool has_interop_artifact) {
                return foreignSignatureMatches(
                    parameters, return_type, signature, resolve_nominal,
                    has_interop_artifact);
              },
          .valid_foreign_operation =
              [](const interop::ArtifactReference &reference,
                 std::span<const PublicType> parameters,
                 PublicType return_type) {
                return validForeignOperation(reference, parameters,
                                             return_type);
              },
          .valid_constraint = validInterfaceConstraint,
          .entity_key =
              [](const PublicEntity &entity, std::string_view package,
                 std::string_view module, std::string_view name) {
                return entity.kind == PublicEntityKind::Function
                           ? overloadEntityKey(
                                 package, module, name, entity.member_owner,
                                 entity.member_kind,
                                 entity.generic_parameter_count,
                                 entity.parameters)
                           : entityKey(package, module, name, entity.kind);
              },
          .entity_fingerprint =
              [this](const PublicEntity &entity, std::string_view package,
                     std::string_view module, std::string_view name) {
                StableFingerprint expected;
                if (entity.kind == PublicEntityKind::Function) {
                  expected = entityFingerprint(
                      package, module, name, entity.member_owner,
                      entity.member_kind, entity.generic_parameter_count,
                      entity.parameters, entity.return_type, entity.error_type,
                      entity.execution_kind, entity.coroutine_constructor,
                      entity.nominal_constructor, entity.semantic_contract,
                      entity.intrinsic_role, entity.ownership_summary,
                      entity.generic_template, entity.declaration_kind,
                      entity.is_unsafe, entity.is_const,
                      entity.foreign_abi.hasValue()
                          ? values_->identifier(entity.foreign_abi)
                          : std::string_view{},
                      entity.foreign_signature, entity.parameter_names,
                      entity.default_arguments, entity.constraints,
                      entity.interop_artifact,
                      entity.external_symbol.hasValue()
                          ? values_->identifier(entity.external_symbol)
                          : std::string_view{});
                } else if (entity.kind == PublicEntityKind::NominalType) {
                  auto nominal = buildPublicNominalTypeArtifact(
                      std::string(package), std::string(module),
                      std::string(name), entity.generic_parameter_count,
                      entity.nominal_fields, entity.nominal_value_repr_pattern,
                      entity.nominal_object_repr_pattern,
                      entity.nominal_representation_policy,
                      entity.nominal_kind, entity.nominal_variants,
                      entity.nominal_is_exported,
                      entity.nominal_is_value_enum);
                  nominal.foreign_representation =
                      entity.nominal_foreign_representation;
                  nominal.foreign_invalid_state =
                      entity.nominal_foreign_invalid_state;
                  nominal.foreign_invalid_integer =
                      entity.nominal_foreign_invalid_integer;
                  nominal.foreign_handle_type =
                      entity.nominal_foreign_handle_type;
                  nominal.foreign_completion_handle_type =
                      entity.nominal_foreign_completion_handle_type;
                  nominal.foreign_callback_type =
                      entity.nominal_foreign_callback_type;
                  nominal.foreign_waker_type =
                      entity.nominal_foreign_waker_type;
                  nominal.foreign_resource_protocol =
                      entity.nominal_foreign_resource_protocol;
                  nominal.foreign_resource_operations =
                      entity.nominal_foreign_resource_operations;
                  finalizePublicNominalTypeArtifact(nominal);
                  expected = nominal.definition_fingerprint;
                } else if (entity.kind == PublicEntityKind::Interface) {
                  expected = interfaceDeclarationFingerprint(
                      *entity.interface_declaration);
                } else if (entity.kind == PublicEntityKind::TypeAlias) {
                  expected = typeAliasFingerprint(*entity.type_alias);
                } else if (entity.kind ==
                               PublicEntityKind::ForeignOperation &&
                           entity.interop_artifact) {
                  expected = entity.interop_artifact->fingerprint;
                }
                return expected;
              },
          .interface_fingerprint =
              [this](const PublicInterface &interface_value) {
                return interfaceFingerprint(interface_value, *this, *values_);
              },
      });
}
PublicInterfaceId
registerPublicInterface(const SemIR &sem_ir, PublicInterfaceRegistry &registry,
                        interop::ArtifactRegistry &interop_registry,
                        IdentifierId package_name, std::string &error,
                        NativeDefinitionExportClosure *native_exports) {
  if (native_exports)
    native_exports->functions.clear();
  internal::PublicInterfaceTypeConstructionCallbacks type_callbacks{
          .entity_fingerprint =
              [](std::string_view package, std::string_view module,
                 std::string_view name,
                 const std::optional<PublicEntityReferenceArtifact>
                     &member_owner,
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
                 PublicCallableDeclarationKind declaration_kind,
                 bool is_unsafe, bool is_const, std::string_view foreign_abi,
                 const std::optional<ForeignAbiSignature> &foreign_signature,
                 std::span<const std::string> parameter_names,
                 std::span<const std::optional<PublicConstantValue>>
                     default_arguments,
                 std::span<const PublicInterfaceConstraintArtifact> constraints,
                 const std::optional<interop::ArtifactReference>
                     &interop_artifact,
                 std::string_view external_symbol) {
                return entityFingerprint(
                    package, module, name, member_owner, member_kind,
                    generic_parameter_count, parameters, std::move(return_type),
                    error_type, execution_kind, coroutine_constructor,
                    nominal_constructor, semantic_contract, intrinsic_role,
                    ownership_summary, generic_template, declaration_kind,
                    is_unsafe, is_const, foreign_abi, foreign_signature,
                    parameter_names, default_arguments, constraints,
                    interop_artifact, external_symbol);
              },
          .canonical_parameter_names =
              [](std::size_t parameter_count,
                 std::span<const std::string> names) {
                return canonicalParameterNames(parameter_count, names);
              },
          .canonical_default_arguments =
              [](std::size_t parameter_count,
                 std::span<const std::optional<PublicConstantValue>>
                     arguments) {
                return canonicalDefaultArguments(parameter_count, arguments);
              },
      };
  internal::PublicInterfaceTypeConstructionContext type_context(
      sem_ir, registry, package_name, error, type_callbacks);
  if (!type_context.buildNominals())
    return PublicInterfaceId::invalid();
  auto function_construction =
      internal::PublicInterfaceFunctionConstructionService::build(
          sem_ir, registry, interop_registry, package_name, error, type_context,
          {
              .entity_fingerprint = type_callbacks.entity_fingerprint,
              .build_generic_template =
                  [](const SemIR &semantic_ir,
                     const SemFunction &function,
                     std::string_view package,
                     const std::function<std::optional<PublicType>(TypeId)>
                         &map_public_type,
                     const std::unordered_map<std::uint32_t, IdentifierId>
                         &hidden_evaluator_targets,
                     std::string &build_error) {
                    return internal::
                        PublicInterfaceGenericTemplateConstructionService::build(
                        semantic_ir, function, package, map_public_type,
                        hidden_evaluator_targets, build_error);
                  },
              .normalize_ownership =
                  [](CallableOwnershipSummary summary,
                     std::span<const PublicType> parameters) {
                    return normalizePublicOwnershipSummary(std::move(summary),
                                                           parameters);
                  },
              .foreign_operation_fingerprint =
                  [](std::span<const PublicType> parameters,
                     PublicType result,
                     const interop::ForeignOperationArtifact &operation) {
                    return foreignOperationFingerprint(
                        parameters, std::move(result), operation);
                  },
          });
  if (!function_construction)
    return PublicInterfaceId::invalid();
  auto &functions = function_construction->functions;
  auto &public_nominals = function_construction->nominals;
  auto &public_values = function_construction->values;
  auto &public_interfaces = function_construction->interfaces;
  auto &public_aliases = function_construction->aliases;
  auto &public_witnesses = function_construction->witnesses;
  const auto &local_function_specs =
      function_construction->local_function_specs;
  const auto function_reference_for_spec = [&](const PublicFunctionBindingSpec
                                                   &spec) {
    if (spec.canonical_entity.hasValue()) {
      const auto *entity = registry.tryGetEntity(spec.canonical_entity);
      if (!entity || entity->kind != PublicEntityKind::Function)
        return std::optional<PublicEntityReferenceArtifact>{};
      return std::optional(PublicEntityReferenceArtifact{
          PublicEntityKind::Function,
          std::string(sem_ir.identifier(entity->package_name)),
          std::string(sem_ir.identifier(entity->module_name)),
          std::string(sem_ir.identifier(entity->name)), entity->fingerprint});
    }
    const auto canonical_name =
        spec.canonical_name.hasValue() ? spec.canonical_name : spec.name;
    const auto name = sem_ir.identifier(canonical_name);
    const auto parameter_names =
        canonicalParameterNames(spec.parameters.size(), spec.parameter_names);
    const auto default_arguments = canonicalDefaultArguments(
        spec.parameters.size(), spec.default_arguments);
    return std::optional(PublicEntityReferenceArtifact{
        PublicEntityKind::Function,
        std::string(sem_ir.identifier(package_name)),
        std::string(sem_ir.identifier(sem_ir.moduleName())), std::string(name),
        entityFingerprint(
            sem_ir.identifier(package_name),
            sem_ir.identifier(sem_ir.moduleName()), name, spec.member_owner,
            spec.member_kind, spec.generic_parameter_count, spec.parameters,
            spec.return_type, spec.error_type, spec.execution_kind,
            spec.coroutine_constructor, spec.nominal_constructor,
            spec.semantic_contract, spec.intrinsic_role, spec.ownership_summary,
            spec.generic_template, spec.declaration_kind, spec.is_unsafe,
            spec.is_const, spec.foreign_abi, spec.foreign_signature,
            parameter_names, default_arguments, spec.constraints,
            spec.interop_artifact, spec.external_symbol)});
  };
  const auto final_function_reference = [&](FunctionRefId id) {
    const auto &reference = sem_ir.functionRef(id);
    if (reference.public_entity.hasValue()) {
      const auto *entity = registry.tryGetEntity(reference.public_entity);
      if (!entity || entity->kind != PublicEntityKind::Function)
        return std::optional<PublicEntityReferenceArtifact>{};
      return std::optional(PublicEntityReferenceArtifact{
          PublicEntityKind::Function,
          std::string(sem_ir.identifier(entity->package_name)),
          std::string(sem_ir.identifier(entity->module_name)),
          std::string(sem_ir.identifier(entity->name)), entity->fingerprint});
    }
    const auto found =
        local_function_specs.find(reference.local_function.index);
    return found == local_function_specs.end()
               ? std::optional<PublicEntityReferenceArtifact>{}
               : function_reference_for_spec(functions[found->second]);
  };

  if (!internal::PublicInterfaceDeclarationConstructionService::build(
          sem_ir, registry, package_name, error, native_exports, type_context,
          local_function_specs,
          {
              .function_reference = final_function_reference,
              .interface_fingerprint =
                  [](const PublicInterfaceDeclarationArtifact &artifact) {
                    return interfaceDeclarationFingerprint(artifact);
                  },
              .alias_fingerprint = [](const PublicTypeAliasArtifact &artifact) {
                return typeAliasFingerprint(artifact);
              },
              .witness_fingerprint =
                  [](const PublicInterfaceWitnessArtifact &artifact) {
                    return interfaceWitnessFingerprint(artifact);
                  },
              .value_fingerprint = [](const PublicValueArtifact &artifact) {
                return valueFingerprint(artifact);
              },
          },
          functions, public_values, public_interfaces, public_aliases,
          public_witnesses))
    return PublicInterfaceId::invalid();
  return internal::PublicInterfaceClosureConstructionService::
      finalizeAndRegister(
          sem_ir, registry, package_name, error, native_exports, functions,
          public_nominals, public_values, public_interfaces, public_aliases,
          public_witnesses);
}

} // namespace chtholly::compiler
