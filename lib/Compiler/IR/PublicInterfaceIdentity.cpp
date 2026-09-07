#include "PublicInterfaceEncodingInternal.h"
#include "PublicInterfaceServices.h"

namespace chtholly::compiler::internal {

std::string PublicInterfaceIdentityService::moduleKey(std::string_view package,
                                                      std::string_view module) {
  std::string result;
  appendField(result, package);
  appendField(result, module);
  return result;
}

std::string PublicInterfaceIdentityService::entityKey(std::string_view package,
                                                      std::string_view module,
                                                      std::string_view name,
                                                      PublicEntityKind kind) {
  std::string result;
  appendField(result, "chtholly.next.public-entity-key.v3");
  appendField(result, package);
  appendField(result, module);
  appendU32(result, static_cast<std::uint32_t>(kind));
  appendField(result, name);
  return result;
}

std::string PublicInterfaceIdentityService::overloadEntityKey(
    std::string_view package, std::string_view module, std::string_view name,
    const std::optional<PublicEntityReferenceArtifact> &member_owner,
    PublicFunctionArtifact::MemberKind member_kind,
    std::uint32_t generic_parameter_count,
    std::span<const PublicType> parameters) {
  auto result = entityKey(package, module, name, PublicEntityKind::Function);
  appendField(result, "chtholly.next.overload-key.v1");
  appendU32(result, member_owner.has_value() ? 1U : 0U);
  if (member_owner)
    appendEntityReference(result, *member_owner);
  appendU32(result, static_cast<std::uint32_t>(member_kind));
  appendU32(result, generic_parameter_count);
  appendU32(result, static_cast<std::uint32_t>(parameters.size()));
  for (const auto &parameter : parameters)
    appendType(result, parameter);
  return result;
}

std::string PublicInterfaceIdentityService::memberFunctionBindingKey(
    const PublicEntityReferenceArtifact &owner, std::string_view name) {
  std::string result;
  appendField(result, "chtholly.next.public-method-binding-key.v1");
  appendEntityReference(result, owner);
  appendField(result, name);
  return result;
}

} // namespace chtholly::compiler::internal
