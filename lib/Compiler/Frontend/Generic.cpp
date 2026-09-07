#include "chtholly/Compiler/Generic.h"

#include <array>
#include <cassert>
#include <limits>
#include <string>

namespace chtholly::compiler {
namespace {

std::size_t combineHash(std::size_t hash, std::uint32_t value) {
  hash ^= static_cast<std::size_t>(value) + 0x9e3779b9U + (hash << 6U) +
          (hash >> 2U);
  return hash;
}

void appendU32(std::string &out, std::uint32_t value) {
  for (std::uint32_t shift = 0; shift != 32; shift += 8)
    out.push_back(static_cast<char>((value >> shift) & 0xffU));
}

} // namespace

std::size_t
CanonicalTypeHash::operator()(const CanonicalType &type) const noexcept {
  auto hash = static_cast<std::size_t>(type.kind);
  hash = combineHash(hash, type.arg0);
  hash = combineHash(hash, type.arg1);
  hash = combineHash(hash, static_cast<std::uint32_t>(type.projection_kind));
  for (const auto element : type.elements)
    hash = combineHash(hash, element.index);
  hash = combineHash(hash, type.abi_union ? 1U : 0U);
  hash = combineHash(
      hash, static_cast<std::uint32_t>(type.foreign_calling_convention));
  for (const auto byte : type.nominal_key)
    hash = combineHash(hash, static_cast<unsigned char>(byte));
  hash = combineHash(hash, type.callable_context_parameter);
  hash = combineHash(hash, type.registration_authority);
  hash = combineHash(hash, type.registration_entry_parameter);
  hash = combineHash(hash, type.registration_userdata_parameter);
  hash = combineHash(hash, type.registration_release_parameter);
  for (const auto &binding : type.registration_bindings) {
    for (const auto byte : binding.name)
      hash = combineHash(hash, static_cast<unsigned char>(byte));
    hash = combineHash(hash, binding.parameter_index);
  }
  for (const auto parameter : type.registration_arm_parameters)
    hash = combineHash(hash, parameter);
  for (const auto parameter : type.registration_detach_parameters)
    hash = combineHash(hash, parameter);
  hash = combineHash(hash, type.foreign_resource_protocol.index);
  const auto hash_region = [&](const OwnershipRegion &region) {
    hash = combineHash(hash, region.parameter_index);
    for (const auto &step : region.path) {
      hash = combineHash(hash, static_cast<std::uint32_t>(step.kind));
      hash = combineHash(hash, step.index);
    }
    hash = combineHash(hash, region.has_bit_range ? 1U : 0U);
    hash = combineHash(hash, region.bit_begin);
    hash = combineHash(hash, region.bit_end);
  };
  for (const auto &effect : type.callable_contract.effects) {
    hash = combineHash(hash, static_cast<std::uint32_t>(effect.kind));
    hash_region(effect.region);
  }
  for (const auto &postcondition : type.callable_contract.postconditions) {
    hash_region(postcondition.region);
    hash = combineHash(hash, postcondition.outcomes);
    hash = combineHash(
        hash, static_cast<std::uint32_t>(postcondition.condition.clauses.size()));
    for (const auto &clause : postcondition.condition.clauses) {
      hash = combineHash(hash,
                         static_cast<std::uint32_t>(clause.atoms.size()));
      for (const auto &atom : clause.atoms) {
        hash = combineHash(hash, atom.parameter_index);
        hash = combineHash(hash, atom.expected ? 1U : 0U);
      }
    }
  }
  hash = combineHash(hash, type.callable_contract.returns_owned ? 1U : 0U);
  for (const auto &source : type.callable_contract.return_provenance) {
    hash_region(source.region);
    hash = combineHash(hash,
                       static_cast<std::uint32_t>(source.carrier_path.size()));
    for (const auto &step : source.carrier_path) {
      hash = combineHash(hash, static_cast<std::uint32_t>(step.kind));
      hash = combineHash(hash, step.index);
    }
    hash = combineHash(
        hash, static_cast<std::uint32_t>(source.condition.clauses.size()));
    for (const auto &clause : source.condition.clauses) {
      hash = combineHash(hash, static_cast<std::uint32_t>(clause.atoms.size()));
      for (const auto &atom : clause.atoms) {
        hash = combineHash(hash, atom.parameter_index);
        hash = combineHash(hash, atom.expected ? 1U : 0U);
      }
    }
  }
  return hash;
}

std::size_t CanonicalForeignResourceProtocolHash::operator()(
    const CanonicalForeignResourceProtocol &value) const noexcept {
  auto hash = combineHash(0, value.facts.semantic_epoch);
  hash = combineHash(hash, value.facts.completion_projection ? 1U : 0U);
  hash = combineHash(hash, value.facts.callback_type_index);
  hash = combineHash(hash, value.facts.resource_type_index);
  hash = combineHash(hash, value.facts.completion_type_index);
  hash = combineHash(hash, static_cast<std::uint32_t>(value.facts.invalid_state));
  const auto invalid_bits = static_cast<std::uint64_t>(value.facts.invalid_integer);
  hash = combineHash(hash, static_cast<std::uint32_t>(invalid_bits));
  hash = combineHash(hash, static_cast<std::uint32_t>(invalid_bits >> 32U));
  hash = combineHash(hash, value.facts.release_authority);
  for (const auto role : value.facts.cleanup_path)
    hash = combineHash(hash, static_cast<std::uint32_t>(role));
  for (const auto type : value.types)
    hash = combineHash(hash, type.index);
  for (const auto &role : value.facts.roles) {
    hash = combineHash(hash, static_cast<std::uint32_t>(role.kind));
    hash = combineHash(hash, role.callable_type_index);
    hash = combineHash(hash, static_cast<std::uint32_t>(role.quiescence));
    for (const auto &parameter : role.parameters) {
      hash = combineHash(hash, static_cast<std::uint32_t>(parameter.kind));
      hash = combineHash(hash, parameter.parameter_index);
      for (const auto byte : parameter.name)
        hash = combineHash(hash, static_cast<unsigned char>(byte));
    }
  }
  return hash;
}

std::size_t CanonicalConstantHash::operator()(
    const CanonicalConstant &value) const noexcept {
  return combineHash(value.type.index,
                     static_cast<std::uint32_t>(value.dependence));
}

GenericValueStores::GenericValueStores() {
  void_type_ = internType({.kind = CanonicalTypeKind::Void});
  bool_type_ = internType({.kind = CanonicalTypeKind::Bool});
  char_type_ = internType({.kind = CanonicalTypeKind::Char});
  i32_type_ = integerType(32, true);
  string_type_ = internType({.kind = CanonicalTypeKind::String});
  never_type_ = internType({.kind = CanonicalTypeKind::Never});
}

CanonicalTypeId GenericValueStores::internType(CanonicalType type) {
  return types_.add(std::move(type));
}

const CanonicalType &GenericValueStores::type(CanonicalTypeId id) const {
  return types_.get(id);
}

ForeignResourceProtocolId GenericValueStores::internForeignResourceProtocol(
    CanonicalForeignResourceProtocol protocol) {
  protocol.facts.canonicalize();
  return foreign_resource_protocols_.add(std::move(protocol));
}

const CanonicalForeignResourceProtocol &
GenericValueStores::foreignResourceProtocol(ForeignResourceProtocolId id) const {
  return foreign_resource_protocols_.get(id);
}

CanonicalConstantId
GenericValueStores::internTypeConstant(CanonicalTypeId type_id,
                                       ConstantDependence dependence) {
  return constants_.add({type_id, dependence});
}

const CanonicalConstant &
GenericValueStores::constant(CanonicalConstantId id) const {
  return constants_.get(id);
}

GenericId GenericValueStores::addGeneric(CheckIRId owner,
                                         IdentifierId module_name,
                                         IdentifierId name,
                                         std::uint32_t binding_count) {
  const auto id = generics_.add({.owner = owner,
                                 .module_name = module_name,
                                 .name = name,
                                 .binding_count = binding_count,
                                 .self_specific = SpecificId::invalid()});
  std::vector<CanonicalConstantId> arguments;
  arguments.reserve(binding_count);
  for (std::uint32_t index = 0; index < binding_count; ++index) {
    const auto type_id = internType({.kind = CanonicalTypeKind::TypeParameter,
                                     .arg0 = id.index,
                                     .arg1 = index});
    arguments.push_back(
        internTypeConstant(type_id, ConstantDependence::TemplateSymbolic));
  }
  generics_.get(id).self_specific = getOrAddSpecific(id, arguments);
  auto &self = specifics_.get(generics_.get(id).self_specific);
  self.declaration_state = SpecificRegionState::Ready;
  self.definition_state = SpecificRegionState::Ready;
  return id;
}

const Generic &GenericValueStores::generic(GenericId id) const {
  return generics_.get(id);
}

SpecificId GenericValueStores::getOrAddSpecific(
    GenericId generic_id, std::span<const CanonicalConstantId> arguments,
    std::span<const StableFingerprint> constraint_witnesses) {
  std::string key;
  appendU32(key, generic_id.index);
  appendU32(key, static_cast<std::uint32_t>(arguments.size()));
  for (const auto argument : arguments)
    appendU32(key, argument.index);
  appendU32(key, static_cast<std::uint32_t>(constraint_witnesses.size()));
  for (const auto &witness : constraint_witnesses)
    for (const auto byte : witness.bytes())
      key.push_back(static_cast<char>(byte));
  if (const auto found = specific_lookup_.find(key);
      found != specific_lookup_.end())
    return found->second;
  const auto id = specifics_.add(
      {.generic = generic_id,
       .arguments = std::vector(arguments.begin(), arguments.end()),
       .constraint_witnesses = std::vector(constraint_witnesses.begin(),
                                           constraint_witnesses.end())});
  specific_lookup_.emplace(std::move(key), id);
  return id;
}

const Specific &GenericValueStores::specific(SpecificId id) const {
  return specifics_.get(id);
}

Specific &GenericValueStores::specific(SpecificId id) {
  return specifics_.get(id);
}

void GenericValueStores::collectMetrics(core::CompilerMetrics &metrics,
                                        std::string_view label) const {
  types_.collectMetrics(metrics,
                        core::CompilerMetrics::childLabel(label, "types"));
  constants_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "constants"));
  foreign_resource_protocols_.collectMetrics(
      metrics,
      core::CompilerMetrics::childLabel(label, "foreign_resource_protocols"));
  generics_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "generics"));
  specifics_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "specifics"));
  metrics.addMemory(core::CompilerMetrics::childLabel(label, "specific_lookup"),
                    specific_lookup_.size() *
                        sizeof(decltype(specific_lookup_)::value_type),
                    specific_lookup_.bucket_count() * sizeof(void *) +
                        specific_lookup_.size() *
                            sizeof(decltype(specific_lookup_)::value_type));
}

std::string_view canonicalTypeKindName(CanonicalTypeKind kind) {
  constexpr auto names = std::to_array<std::string_view>(
      {"void", "bool", "integer", "float", "string", "array", "function",
       "async-function", "type-parameter", "nominal", "reference",
       "raw-pointer", "c-function-pointer", "c-variadic-function-pointer",
       "callback-adapter", "callback-registration", "callback-completion",
       "callback-wake", "foreign-completion", "foreign-wake",
       "coroutine-executor", "coroutine-scope", "coroutine-task",
       "coroutine-task-outcome", "coroutine-task-completion",
       "coroutine-task-completion-set", "coroutine-task-selection",
       "coroutine-checked", "never", "tuple", "slice", "char"});
  const auto index = static_cast<std::size_t>(kind);
  return index < names.size() ? names[index] : "invalid";
}

} // namespace chtholly::compiler
