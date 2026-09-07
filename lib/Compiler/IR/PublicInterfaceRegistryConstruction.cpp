#include "PublicInterfaceServices.h"

#include <unordered_map>

namespace chtholly::compiler::internal {

std::optional<std::vector<PublicValueArtifact>>
PublicInterfaceRegistryConstructionService::collectUniqueValues(
    std::span<const PublicValueArtifact> candidates,
    std::span<const PublicNominalTypeArtifact> nominals, std::string &error,
    const std::function<bool(const PublicConstantValue &,
                             std::span<const PublicNominalTypeArtifact>)>
        &valid_constant_shape,
    const std::function<StableFingerprint(const PublicValueArtifact &)>
        &value_fingerprint) {
  error.clear();
  std::unordered_map<std::string, const PublicValueArtifact *>
      exported_value_names;
  std::vector<PublicValueArtifact> unique_values;
  unique_values.reserve(candidates.size());
  for (const auto &value : candidates) {
    if (value.kind >= PublicValueKind::Count || value.name.empty() ||
        value.canonical_package.empty() || value.canonical_module.empty() ||
        value.canonical_name.empty() || value.value.type != value.type ||
        !valid_constant_shape(value.value, nominals) ||
        value.entity_fingerprint != value_fingerprint(value)) {
      error = "cannot register an invalid public value";
      return std::nullopt;
    }
    if (const auto [position, inserted] =
            exported_value_names.emplace(value.name, &value);
        !inserted) {
      if (*position->second != value) {
        error = "public interface has conflicting exported value bindings";
        return std::nullopt;
      }
      continue;
    }
    unique_values.push_back(value);
  }
  return unique_values;
}

} // namespace chtholly::compiler::internal
