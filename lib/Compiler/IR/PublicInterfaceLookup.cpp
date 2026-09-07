#include "PublicInterfaceServices.h"
#include "chtholly/Compiler/PublicInterface.h"
#include "chtholly/Compiler/SharedValueStores.h"

#include <algorithm>
#include <tuple>

namespace chtholly::compiler {

const PublicNominalTypeArtifact *
PublicInterfaceArtifact::findNominalType(std::string_view name) const {
  const auto found = std::ranges::find_if(
      nominal_types_, [&](const PublicNominalTypeArtifact &nominal) {
        return nominal.is_exported && nominal.entity.canonical_name == name;
      });
  return found == nominal_types_.end() ? nullptr : &*found;
}

const PublicValueArtifact *
PublicInterfaceArtifact::findValue(std::string_view name) const {
  const auto found =
      std::ranges::find(values_, name, &PublicValueArtifact::name);
  return found == values_.end() ? nullptr : &*found;
}

const PublicInterfaceDeclarationArtifact *
PublicInterfaceArtifact::findInterface(std::string_view name) const {
  const auto found = std::ranges::find_if(interfaces_, [&](const auto &value) {
    return value.entity.canonical_name == name;
  });
  return found == interfaces_.end() ? nullptr : &*found;
}

const PublicTypeAliasArtifact *
PublicInterfaceArtifact::findTypeAlias(std::string_view name) const {
  const auto found =
      std::ranges::find_if(type_aliases_, [&](const auto &value) {
        return value.entity.canonical_name == name;
      });
  return found == type_aliases_.end() ? nullptr : &*found;
}

const PublicFunctionArtifact *
PublicInterfaceArtifact::findFunction(std::string_view name) const {
  const auto found =
      std::ranges::find_if(functions_, [&](const auto &function) {
        return !function.member_owner && function.name == name;
      });
  return found == functions_.end() ? nullptr : &*found;
}

std::vector<const PublicFunctionArtifact *>
PublicInterfaceArtifact::findFunctions(std::string_view name) const {
  std::vector<const PublicFunctionArtifact *> result;
  for (const auto &function : functions_)
    if (!function.member_owner && function.name == name)
      result.push_back(&function);
  return result;
}

const PublicFunctionArtifact *PublicInterfaceArtifact::findMemberFunction(
    const PublicEntityReferenceArtifact &owner, std::string_view name) const {
  const auto found =
      std::ranges::find_if(functions_, [&](const auto &function) {
        return function.member_owner && *function.member_owner == owner &&
               function.name == name;
      });
  return found == functions_.end() ? nullptr : &*found;
}

std::vector<const PublicFunctionArtifact *>
PublicInterfaceArtifact::findMemberFunctions(
    const PublicEntityReferenceArtifact &owner, std::string_view name) const {
  std::vector<const PublicFunctionArtifact *> result;
  for (const auto &function : functions_)
    if (function.member_owner && *function.member_owner == owner &&
        function.name == name)
      result.push_back(&function);
  return result;
}

PublicInterface::PublicInterface(core::Arena &arena, SharedValueStores &values,
                                 CheckIRId check_ir_id,
                                 PublicInterfaceId interface_id,
                                 IdentifierId package_name,
                                 IdentifierId module_name)
    : values_(&values), check_ir_id_(check_ir_id), interface_id_(interface_id),
      package_name_(package_name), module_name_(module_name) {
  (void)arena;
}

PublicBindingId
PublicInterface::addFunction(const PublicFunctionBindingSpec &function,
                             PublicEntityId canonical_entity) {
  PublicTypeBlockId parameter_block;
  for (std::uint32_t index = 0; index < parameter_blocks_.size(); ++index) {
    if (parameter_blocks_[index] == function.parameters) {
      parameter_block = PublicTypeBlockId(index);
      break;
    }
  }
  if (!parameter_block.hasValue()) {
    parameter_block =
        PublicTypeBlockId(static_cast<std::uint32_t>(parameter_blocks_.size()));
    parameter_blocks_.push_back(function.parameters);
  }
  const auto id =
      functions_.add({function.name, function.member_owner,
                      function.member_kind, function.generic_parameter_count,
                      parameter_block, function.return_type, canonical_entity});
  if (function.member_owner) {
    member_function_names_
        [internal::PublicInterfaceIdentityService::memberFunctionBindingKey(
             *function.member_owner, values_->identifier(function.name))]
            .push_back(id);
  } else {
    function_names_[function.name.index].push_back(id);
  }
  return id;
}

PublicBindingId
PublicInterface::addNominalType(const PublicNominalTypeArtifact &nominal,
                                PublicEntityId canonical_entity) {
  const auto name = values_->internIdentifier(nominal.entity.canonical_name);
  const auto id = nominal_types_.add(
      {name, nominal.generic_parameter_count, canonical_entity});
  nominal_type_names_.emplace(name.index, id);
  return id;
}

PublicBindingId PublicInterface::findFunction(IdentifierId name) const {
  const auto found = function_names_.find(name.index);
  return found == function_names_.end() || found->second.empty()
             ? PublicBindingId::invalid()
             : found->second.front();
}

std::span<const PublicBindingId>
PublicInterface::findFunctions(IdentifierId name) const {
  const auto found = function_names_.find(name.index);
  return found == function_names_.end()
             ? std::span<const PublicBindingId>{}
             : std::span<const PublicBindingId>(found->second);
}

PublicBindingId
PublicInterface::findMemberFunction(const PublicEntityReferenceArtifact &owner,
                                    IdentifierId name) const {
  const auto found = member_function_names_.find(
      internal::PublicInterfaceIdentityService::memberFunctionBindingKey(
          owner, values_->identifier(name)));
  return found == member_function_names_.end() || found->second.empty()
             ? PublicBindingId::invalid()
             : found->second.front();
}

std::span<const PublicBindingId>
PublicInterface::findMemberFunctions(const PublicEntityReferenceArtifact &owner,
                                     IdentifierId name) const {
  const auto found = member_function_names_.find(
      internal::PublicInterfaceIdentityService::memberFunctionBindingKey(
          owner, values_->identifier(name)));
  return found == member_function_names_.end()
             ? std::span<const PublicBindingId>{}
             : std::span<const PublicBindingId>(found->second);
}

PublicBindingId PublicInterface::findNominalType(IdentifierId name) const {
  const auto found = nominal_type_names_.find(name.index);
  return found == nominal_type_names_.end() ? PublicBindingId::invalid()
                                            : found->second;
}

const PublicValueArtifact *PublicInterface::findValue(IdentifierId name) const {
  if (!name.hasValue() || name.index >= values_->identifierCount())
    return nullptr;
  const auto text = values_->identifier(name);
  const auto found =
      std::ranges::find(value_artifacts_, text, &PublicValueArtifact::name);
  return found == value_artifacts_.end() ? nullptr : &*found;
}

const PublicInterfaceDeclarationArtifact *
PublicInterface::findInterface(IdentifierId name) const {
  if (!name.hasValue() || name.index >= values_->identifierCount())
    return nullptr;
  const auto text = values_->identifier(name);
  const auto found =
      std::ranges::find_if(interface_artifacts_, [&](const auto &value) {
        return value.entity.canonical_name == text;
      });
  return found == interface_artifacts_.end() ? nullptr : &*found;
}

const PublicTypeAliasArtifact *
PublicInterface::findTypeAlias(IdentifierId name) const {
  if (!name.hasValue() || name.index >= values_->identifierCount())
    return nullptr;
  const auto text = values_->identifier(name);
  const auto found =
      std::ranges::find_if(type_alias_artifacts_, [&](const auto &value) {
        return value.entity.canonical_name == text;
      });
  return found == type_alias_artifacts_.end() ? nullptr : &*found;
}

const PublicNominalTypeBinding &
PublicInterface::nominalType(PublicBindingId id) const {
  return nominal_types_.get(id);
}

const PublicFunctionBinding &
PublicInterface::function(PublicBindingId id) const {
  return functions_.get(id);
}

std::span<const PublicType>
PublicInterface::parameterTypes(PublicTypeBlockId id) const {
  return parameter_blocks_[id.index];
}

} // namespace chtholly::compiler
