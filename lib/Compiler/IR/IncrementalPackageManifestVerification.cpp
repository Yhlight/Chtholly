#include "chtholly/Compiler/IncrementalDependencies.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <unordered_map>
#include <vector>

namespace chtholly::compiler {

bool CompilerPackageArtifactManifest::verify(std::string &error) const {
  error.clear();
  if (package_name_.empty() || target_triple_.empty() ||
      package_name_.size() > std::numeric_limits<std::uint32_t>::max() ||
      target_triple_.size() > std::numeric_limits<std::uint32_t>::max() ||
      resolved_features_.size() > std::numeric_limits<std::uint32_t>::max() ||
      direct_dependencies_.size() > std::numeric_limits<std::uint32_t>::max() ||
      modules_.size() > std::numeric_limits<std::uint32_t>::max() ||
      !compile_toolchain_fingerprint_.hasValue() ||
      !language_contract_.isSupported() ||
      (cffi_identity_ && (cffi_identity_->target != target_triple_ ||
                          !cffi_identity_->fingerprint().hasValue())) ||
      provenance_.kind >= PackageProvenanceKind::Count ||
      !provenance_.contract_fingerprint.hasValue() ||
      ((provenance_.kind == PackageProvenanceKind::ToolchainStandardLibrary) !=
       (package_name_ == "std")) ||
      configuration_fingerprint_ != fingerprintCompilationConfiguration(
                                        package_name_, target_triple_,
                                        compile_toolchain_fingerprint_,
                                        resolved_features_, provenance_,
                                        language_contract_, cffi_identity_) ||
      !std::ranges::is_sorted(resolved_features_) ||
      std::adjacent_find(resolved_features_.begin(),
                         resolved_features_.end()) !=
          resolved_features_.end() ||
      !std::ranges::is_sorted(direct_dependencies_, {},
                              &PackageManifestDependency::package_name) ||
      std::adjacent_find(direct_dependencies_.begin(),
                         direct_dependencies_.end(),
                         [](const auto &lhs, const auto &rhs) {
                           return lhs.package_name == rhs.package_name;
                         }) != direct_dependencies_.end() ||
      std::ranges::any_of(
          resolved_features_,
          [](const auto &value) {
            return value.empty() ||
                   value.size() > std::numeric_limits<std::uint32_t>::max();
          }) ||
      std::ranges::any_of(direct_dependencies_, [&](const auto &value) {
        return value.package_name.empty() ||
               value.package_name == package_name_ ||
               value.package_name.size() >
                   std::numeric_limits<std::uint32_t>::max() ||
               !value.manifest_fingerprint.hasValue();
      })) {
    error = "compiler package manifest has an invalid configuration";
    return false;
  }
  std::string_view previous_module;
  for (const auto &module : modules_) {
    if ((!previous_module.empty() && previous_module >= module.moduleName()) ||
        module.public_interface.packageName() != package_name_ ||
        !module.verify(error)) {
      if (error.empty())
        error = "compiler package manifest has duplicate modules";
      return false;
    }
    previous_module = module.moduleName();
  }
  for (const auto &module : modules_) {
    for (const auto &dependency : module.module_dependencies) {
      if (dependency.package_name == package_name_) {
        if (!findModule(dependency.module_name)) {
          error = "package manifest link closure names a missing local module";
          return false;
        }
      } else if (!findDependency(dependency.package_name)) {
        error = "package manifest link closure names an undeclared package";
        return false;
      }
    }
    for (const auto &observation : module.observations) {
      if (observation.provider.package_name != package_name_) {
        if (!findDependency(observation.provider.package_name)) {
          error = "package manifest observes an undeclared dependency";
          return false;
        }
        continue;
      }
      const auto *provider = findModule(observation.provider.module_name);
      if (!provider) {
        error = "compiler package manifest is not closed over local imports";
        return false;
      }
      if (observation.kind == DependencyObservationKind::ModulePresence) {
        if (observation.expected_fingerprint !=
            fingerprintModuleIdentity(observation.provider.package_name,
                                      observation.provider.module_name)) {
          error = "compiler package manifest has an invalid module "
                  "observation";
          return false;
        }
      } else if (observation.kind == DependencyObservationKind::ExportSet) {
        if (observation.expected_fingerprint !=
            provider->public_interface.fingerprint()) {
          error = "compiler package manifest has an invalid export-set "
                  "observation";
          return false;
        }
      } else if (observation.kind ==
                     DependencyObservationKind::NominalBinding ||
                 observation.kind ==
                     DependencyObservationKind::RelocationClosure) {
        const auto *binding = provider->public_interface.findNominalType(
            observation.binding_name);
        if (!binding ||
            binding->entity.canonical_package !=
                observation.canonical_provider.package_name ||
            binding->entity.canonical_module !=
                observation.canonical_provider.module_name ||
            binding->entity.canonical_name != observation.canonical_name ||
            (observation.kind == DependencyObservationKind::NominalBinding
                 ? binding->definition_fingerprint
                 : fingerprintRelocationClosure(*binding)) !=
                observation.expected_fingerprint) {
          error = "compiler package manifest has an invalid nominal observation";
          return false;
        }
      } else {
        const auto binding = std::ranges::find_if(
            provider->public_interface.functions(), [&](const auto &function) {
              return function.name == observation.binding_name &&
                     function.entity_fingerprint ==
                         observation.expected_fingerprint &&
                     function.canonical_package ==
                         observation.canonical_provider.package_name &&
                     function.canonical_module ==
                         observation.canonical_provider.module_name &&
                     function.canonical_name == observation.canonical_name;
            });
        if (binding == provider->public_interface.functions().end() ||
            binding->entity_fingerprint != observation.expected_fingerprint) {
          error = "compiler package manifest has an invalid entity "
                  "observation";
          return false;
        }
      }
    }
  }
  return true;
}

bool CompilerPackageArtifactManifest::verifyDependencies(
    std::span<const CompilerPackageArtifactManifest *const> dependencies,
    std::string &error) const {
  if (!verify(error))
    return false;
  if (dependencies.size() != direct_dependencies_.size()) {
    error = "package manifest dependency set is incomplete";
    return false;
  }
  std::vector<const CompilerPackageArtifactManifest *> ordered_dependencies(
      dependencies.begin(), dependencies.end());
  std::ranges::sort(ordered_dependencies, {},
                    [](const CompilerPackageArtifactManifest *dependency) {
                      return dependency ? dependency->packageName()
                                        : std::string_view{};
                    });
  for (std::size_t index = 0; index < ordered_dependencies.size(); ++index) {
    const auto *dependency = ordered_dependencies[index];
    if (!dependency || !dependency->verify(error) ||
        !language_contract_.isDependencyCompatibleWith(
            dependency->languageContract()) ||
        dependency->packageName() != direct_dependencies_[index].package_name ||
        dependency->fingerprint() !=
            direct_dependencies_[index].manifest_fingerprint ||
        (index != 0 && ordered_dependencies[index - 1]->packageName() ==
                           dependency->packageName())) {
      if (error.empty())
        error = "package manifest dependency set is inconsistent";
      return false;
    }
  }
  std::unordered_map<std::string_view, const ForeignSymbolRequirement *>
      closure_requirements;
  const auto merge_requirements =
      [&](const CompilerPackageArtifactManifest &value) {
        for (const auto &module : value.modules())
          for (const auto &requirement : module.required_foreign_symbols) {
            const auto [found, inserted] = closure_requirements.emplace(
                requirement.external_symbol, &requirement);
            if (!inserted && (found->second->calling_convention !=
                                  requirement.calling_convention ||
                              found->second->signature_fingerprint !=
                                  requirement.signature_fingerprint))
              return false;
          }
        return true;
      };
  if (!merge_requirements(*this) ||
      std::ranges::any_of(ordered_dependencies, [&](const auto *dependency) {
        return !merge_requirements(*dependency);
      })) {
    error = "package dependency closure has conflicting foreign symbol ABI "
            "requirements";
    return false;
  }
  for (const auto &module : modules_) {
    for (const auto &observation : module.observations) {
      if (observation.provider.package_name == package_name_)
        continue;
      const auto found = std::ranges::lower_bound(
          ordered_dependencies, observation.provider.package_name, {},
          [](const CompilerPackageArtifactManifest *value) {
            return value->packageName();
          });
      if (found == ordered_dependencies.end() ||
          (*found)->packageName() != observation.provider.package_name) {
        error = "package manifest observation provider is unavailable";
        return false;
      }
      const auto *provider =
          (*found)->findModule(observation.provider.module_name);
      if (!provider) {
        error = "package manifest observes a missing dependency module";
        return false;
      }
      if (observation.kind == DependencyObservationKind::ModulePresence) {
        if (observation.expected_fingerprint !=
            fingerprintModuleIdentity(observation.provider.package_name,
                                      observation.provider.module_name)) {
          error =
              "package manifest has an invalid dependency module observation";
          return false;
        }
      } else if (observation.kind == DependencyObservationKind::ExportSet) {
        if (observation.expected_fingerprint !=
            provider->public_interface.fingerprint()) {
          error =
              "package manifest has a stale dependency export-set observation";
          return false;
        }
      } else if (observation.kind ==
                     DependencyObservationKind::NominalBinding ||
                 observation.kind ==
                     DependencyObservationKind::RelocationClosure) {
        const auto *binding = provider->public_interface.findNominalType(
            observation.binding_name);
        if (!binding ||
            binding->entity.canonical_package !=
                observation.canonical_provider.package_name ||
            binding->entity.canonical_module !=
                observation.canonical_provider.module_name ||
            binding->entity.canonical_name != observation.canonical_name ||
            (observation.kind == DependencyObservationKind::NominalBinding
                 ? binding->definition_fingerprint
                 : fingerprintRelocationClosure(*binding)) !=
                observation.expected_fingerprint) {
          error = "package manifest has a stale dependency nominal "
                  "observation";
          return false;
        }
      } else {
        const auto binding = std::ranges::find_if(
            provider->public_interface.functions(), [&](const auto &function) {
              return function.name == observation.binding_name &&
                     function.entity_fingerprint ==
                         observation.expected_fingerprint &&
                     function.canonical_package ==
                         observation.canonical_provider.package_name &&
                     function.canonical_module ==
                         observation.canonical_provider.module_name &&
                     function.canonical_name == observation.canonical_name;
            });
        if (binding == provider->public_interface.functions().end() ||
            binding->entity_fingerprint != observation.expected_fingerprint) {
          error = "package manifest has a stale dependency entity observation";
          return false;
        }
      }
    }
  }
  return true;
}


} // namespace chtholly::compiler

