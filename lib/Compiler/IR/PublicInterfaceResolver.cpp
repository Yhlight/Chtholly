#include "PublicInterfaceServices.h"

#include <algorithm>
#include <limits>
#include <ranges>

namespace chtholly::compiler::internal {

std::optional<PublicType> PublicInterfaceResolverService::foreignNominal(
    const PublicInterfaceArtifact &artifact, const PublicType &type) {
  return PublicInterfaceArtifactVerificationService::resolveForeignNominal(
      artifact, type);
}

const PublicNominalTypeArtifact *PublicInterfaceResolverService::ownershipNominal(
    const PublicInterfaceArtifact &artifact,
    const PublicEntityReferenceArtifact &reference) {
  const auto found = std::ranges::find_if(
      artifact.nominalTypes(), [&](const PublicNominalTypeArtifact &nominal) {
        return nominal.entity == reference;
      });
  return found == artifact.nominalTypes().end() ? nullptr : &*found;
}

std::optional<PublicType>
PublicInterfaceArtifactVerificationService::resolveForeignNominal(
    const PublicInterfaceArtifact &artifact, const PublicType &type) {
  const auto found = std::ranges::find_if(
      artifact.nominalTypes(), [&](const PublicNominalTypeArtifact &nominal) {
        return nominal.entity == type.nominal_entity;
      });
  if (found == artifact.nominalTypes().end())
    return std::nullopt;
  if (found->kind == NominalKind::ForeignHandle)
    return found->foreign_representation;
  if (found->kind == NominalKind::ForeignResource &&
      found->foreign_handle_type)
    return resolveForeignNominal(artifact, *found->foreign_handle_type);
  return std::nullopt;
}


} // namespace chtholly::compiler::internal
