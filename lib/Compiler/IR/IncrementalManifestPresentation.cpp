#include "chtholly/Compiler/IncrementalDependencies.h"

#include "chtholly/Core/Metrics.h"

#include <sstream>

namespace chtholly::compiler {

std::string CompilerPackageArtifactManifest::print() const {
  std::ostringstream out;
  out << "package " << package_name_ << '\n'
      << "provenance "
      << (provenance_.kind == PackageProvenanceKind::ToolchainStandardLibrary
              ? "toolchain-stdlib "
              : "workspace ")
      << provenance_.contract_fingerprint.hex() << '\n'
      << "language " << language_contract_.source.str() << '\n'
      << "semantic-artifact-epoch "
      << language_contract_.semantic_artifact_epoch << '\n'
      << "standard-library-epoch " << language_contract_.standard_library_epoch
      << '\n'
      << "compile-toolchain " << compile_toolchain_fingerprint_.hex() << '\n'
      << "configuration " << target_triple_ << ' '
      << configuration_fingerprint_.hex() << '\n';
  if (cffi_identity_)
    out << "cffi " << cffi_identity_->target << ' '
        << cffi_identity_->fingerprint().hex() << '\n';
  for (const auto &feature : resolved_features_)
    out << "feature " << feature << '\n';
  for (const auto &dependency : direct_dependencies_)
    out << "dependency " << dependency.package_name << ' '
        << dependency.manifest_fingerprint.hex() << '\n';
  for (const auto &module : modules_) {
    out << "module " << module.moduleName()
        << " source=" << module.source_fingerprint.hex()
        << " kind=" << compilationUnitKindName(module.unit_kind)
        << " emission="
        << (module.emission_role == ModuleEmissionRole::ExecutableEntry
                ? "executable-entry"
            : module.emission_role ==
                    ModuleEmissionRole::CoroutineExecutionEntry
                ? "coroutine-execution-entry"
                : "library")
        << " interface=" << module.public_interface.fingerprint().hex()
        << " object=" << module.object_fingerprint.hex() << '\n';
    for (const auto &dependency : module.module_dependencies)
      out << "  link-module " << dependency.package_name << '/'
          << dependency.module_name << '\n';
    for (const auto &symbol : module.required_foreign_symbols)
      out << "  require-symbol " << symbol.logical_name << " -> "
          << symbol.external_symbol
          << " call=" << static_cast<unsigned>(symbol.calling_convention) << ' '
          << symbol.signature_fingerprint.hex() << '\n';
    for (const auto &function : module.public_interface.functions())
      out << "  binding " << function.name << " -> "
          << function.canonical_package << '/' << function.canonical_module
          << "::" << function.canonical_name << ' '
          << function.entity_fingerprint.hex() << '\n';
    for (const auto &observation : module.observations) {
      out << "  observe " << dependencyObservationKindName(observation.kind)
          << ' ' << observation.provider.package_name << '/'
          << observation.provider.module_name;
      if (!observation.binding_name.empty())
        out << "::" << observation.binding_name;
      if (!observation.canonical_name.empty())
        out << " canonical=" << observation.canonical_provider.package_name
            << '/' << observation.canonical_provider.module_name
            << "::" << observation.canonical_name;
      out << ' ' << observation.expected_fingerprint.hex() << '\n';
    }
    for (const auto &reference : module.specializations)
      out << "  specialization " << reference.request_fingerprint.hex()
          << " component=" << reference.component_fingerprint.hex() << '\n';
  }
  return out.str();
}

void CompilerPackageArtifactManifest::collectMetrics(
    core::CompilerMetrics &metrics, std::string_view label) const {
  std::size_t binding_count = 0;
  std::size_t observation_count = 0;
  std::size_t string_size = package_name_.size() + target_triple_.size();
  for (const auto &feature : resolved_features_)
    string_size += feature.size();
  for (const auto &dependency : direct_dependencies_)
    string_size += dependency.package_name.size();
  for (const auto &module : modules_) {
    binding_count += module.public_interface.functions().size();
    observation_count += module.observations.size();
    string_size += module.moduleName().size();
    for (const auto &dependency : module.module_dependencies)
      string_size +=
          dependency.package_name.size() + dependency.module_name.size();
    for (const auto &symbol : module.required_foreign_symbols)
      string_size += symbol.logical_name.size() + symbol.external_symbol.size();
    for (const auto &function : module.public_interface.functions())
      string_size += function.name.size() + function.canonical_package.size() +
                     function.canonical_module.size() +
                     function.canonical_name.size() +
                     (function.member_owner
                          ? function.member_owner->canonical_package.size() +
                                function.member_owner->canonical_module.size() +
                                function.member_owner->canonical_name.size()
                          : 0U);
    for (const auto &observation : module.observations)
      string_size += observation.provider.package_name.size() +
                     observation.provider.module_name.size() +
                     observation.binding_name.size() +
                     observation.canonical_provider.package_name.size() +
                     observation.canonical_provider.module_name.size() +
                     observation.canonical_name.size();
  }
  metrics.addMemory(core::CompilerMetrics::childLabel(label, "modules"),
                    modules_.size() * sizeof(PackageModuleArtifact),
                    modules_.capacity() * sizeof(PackageModuleArtifact));
  metrics.addMemory(core::CompilerMetrics::childLabel(label, "bindings"),
                    binding_count * sizeof(PublicFunctionArtifact),
                    binding_count * sizeof(PublicFunctionArtifact));
  metrics.addMemory(core::CompilerMetrics::childLabel(label, "observations"),
                    observation_count * sizeof(DependencyObservation),
                    observation_count * sizeof(DependencyObservation));
  metrics.addMemory(core::CompilerMetrics::childLabel(label, "strings"),
                    string_size, string_size);
}

} // namespace chtholly::compiler
