#include "chtholly/Compiler/IncrementalDependencies.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <unordered_map>

namespace chtholly::compiler {

bool CompilerPackageCheckArtifact::verify(std::string &error) const {
  error.clear();
  if (package_name_.empty() || target_triple_.empty() ||
      !compile_toolchain_fingerprint_.hasValue() ||
      !language_contract_.isSupported() ||
      (cffi_identity_ && (cffi_identity_->target != target_triple_ ||
                          !cffi_identity_->fingerprint().hasValue())) ||
      provenance_.kind >= PackageProvenanceKind::Count ||
      !provenance_.contract_fingerprint.hasValue() ||
      ((provenance_.kind == PackageProvenanceKind::ToolchainStandardLibrary) !=
       (package_name_ == "std")) ||
      !std::ranges::is_sorted(resolved_features_) ||
      std::adjacent_find(resolved_features_.begin(),
                         resolved_features_.end()) !=
          resolved_features_.end() ||
      !std::ranges::is_sorted(direct_dependencies_, {},
                              &PackageCheckDependency::package_name) ||
      std::ranges::any_of(resolved_features_, [](const auto &feature) {
        return feature.empty() ||
               feature.size() > std::numeric_limits<std::uint32_t>::max();
      })) {
    error = "compiler package check artifact has an invalid configuration";
    return false;
  }
  for (std::size_t index = 0; index < direct_dependencies_.size(); ++index) {
    const auto &dependency = direct_dependencies_[index];
    if (dependency.package_name.empty() ||
        dependency.package_name == package_name_ ||
        !dependency.artifact_fingerprint.hasValue() ||
        (index != 0 && direct_dependencies_[index - 1].package_name >=
                           dependency.package_name)) {
      error = "compiler package check artifact has invalid dependencies";
      return false;
    }
  }
  std::string_view previous_module;
  for (const auto &module : modules_) {
    if ((!previous_module.empty() && previous_module >= module.moduleName()) ||
        module.public_interface.packageName() != package_name_ ||
        !module.verify(error)) {
      if (error.empty())
        error = "compiler package check artifact has invalid modules";
      return false;
    }
    previous_module = module.moduleName();
  }
  std::unordered_map<std::string_view, const ForeignSymbolRequirement *>
      foreign_requirements;
  for (const auto &module : modules_)
    for (const auto &requirement : module.required_foreign_symbols) {
      const auto [found, inserted] = foreign_requirements.emplace(
          requirement.external_symbol, &requirement);
      if (!inserted &&
          (found->second->calling_convention != requirement.calling_convention ||
           found->second->signature_fingerprint !=
               requirement.signature_fingerprint)) {
        error = "package manifest has conflicting foreign symbol ABI "
                "requirements";
        return false;
      }
    }
  for (const auto &module : modules_) {
    for (const auto &dependency : module.module_dependencies) {
      if (dependency.package_name == package_name_) {
        if (!findModule(dependency.module_name)) {
          error = "package manifest link closure names a missing local module";
          return false;
        }
      } else {
        const auto declared = std::ranges::lower_bound(
            direct_dependencies_, dependency.package_name, {},
            &PackageCheckDependency::package_name);
        if (declared == direct_dependencies_.end() ||
            declared->package_name != dependency.package_name) {
          error = "package check link closure names an undeclared package";
          return false;
        }
      }
    }
    for (const auto &observation : module.observations) {
      if (observation.provider.package_name != package_name_) {
        const auto dependency = std::ranges::lower_bound(
            direct_dependencies_, observation.provider.package_name, {},
            &PackageCheckDependency::package_name);
        if (dependency == direct_dependencies_.end() ||
            dependency->package_name != observation.provider.package_name) {
          error = "package check artifact observes an undeclared dependency";
          return false;
        }
        continue;
      }
      const auto *provider = findModule(observation.provider.module_name);
      if (!provider) {
        error = "package check artifact is not closed over local imports";
        return false;
      }
      if (observation.kind == DependencyObservationKind::ModulePresence &&
          observation.expected_fingerprint !=
              fingerprintModuleIdentity(observation.provider.package_name,
                                        observation.provider.module_name)) {
        error = "package check artifact has an invalid module observation";
        return false;
      }
      if (observation.kind == DependencyObservationKind::ExportSet &&
          observation.expected_fingerprint !=
              provider->public_interface.fingerprint()) {
        error = "package check artifact has an invalid export observation";
        return false;
      }
      if (observation.kind == DependencyObservationKind::EntityBinding ||
          observation.kind == DependencyObservationKind::LifecycleCallable) {
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
          error = observation.kind == DependencyObservationKind::LifecycleCallable
                      ? "package check artifact has an invalid lifecycle "
                        "observation"
                      : "package check artifact has an invalid entity "
                        "observation";
          return false;
        }
      }
      if (observation.kind == DependencyObservationKind::NominalBinding ||
          observation.kind == DependencyObservationKind::RelocationClosure) {
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
          error = observation.kind == DependencyObservationKind::RelocationClosure
                      ? "package check artifact has an invalid relocation "
                        "closure observation"
                      : "package check artifact has an invalid nominal "
                        "observation";
          return false;
        }
      }
    }
  }
  return fingerprint().hasValue();
}

} // namespace chtholly::compiler
