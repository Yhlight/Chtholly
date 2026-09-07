#include "chtholly/Compiler/IncrementalDependencies.h"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly::compiler {
namespace {
void appendU32(std::string &out, std::uint32_t value) {
  for (std::uint32_t shift = 0; shift != 32; shift += 8)
    out.push_back(static_cast<char>((value >> shift) & 0xffU));
}
void appendField(std::string &out, std::string_view value) {
  appendU32(out, static_cast<std::uint32_t>(value.size()));
  out.append(value);
}
void appendFingerprint(std::string &out, const StableFingerprint &fingerprint) {
  out.append(reinterpret_cast<const char *>(fingerprint.bytes().data()),
             fingerprint.bytes().size());
}
PackageProvenance canonicalProvenance(std::string_view package_name,
                                      PackageProvenance provenance) {
  if (!provenance.contract_fingerprint.hasValue())
    provenance.contract_fingerprint = StableFingerprint::fromCanonicalBytes(
        std::string("chtholly.next.workspace-package-contract.v1\n") +
        std::string(package_name));
  return provenance;
}
} // namespace

StableFingerprint fingerprintSource(std::string_view package_name,
                                    std::string_view module_name,
                                    CompilationUnitKind unit_kind,
                                    std::string_view source_text) {
  std::string input;
  appendField(input, "chtholly.next.source.v3");
  appendField(input, package_name);
  appendField(input, module_name);
  input.push_back(static_cast<char>(unit_kind));
  appendField(input, source_text);
  return StableFingerprint::fromCanonicalBytes(input);
}

StableFingerprint fingerprintModuleIdentity(std::string_view package_name,
                                            std::string_view module_name) {
  std::string input;
  appendField(input, "chtholly.next.module-identity.v2");
  appendField(input, package_name);
  appendField(input, module_name);
  return StableFingerprint::fromCanonicalBytes(input);
}

StableFingerprint fingerprintCompilationConfiguration(
    std::string_view package_name, std::string_view target_triple,
    const StableFingerprint &compile_toolchain_fingerprint,
    std::span<const std::string> resolved_features,
    PackageProvenance provenance, LanguageContract language_contract,
    const std::optional<CFFIReceiptIdentity> &cffi_identity) {
  provenance = canonicalProvenance(package_name, provenance);
  std::vector<std::string> features(resolved_features.begin(),
                                    resolved_features.end());
  std::ranges::sort(features);
  features.erase(std::unique(features.begin(), features.end()), features.end());
  std::string input;
  appendField(input, "chtholly.next.compilation-configuration.v7");
  appendField(input, package_name);
  appendField(input, target_triple);
  appendField(input, compile_toolchain_fingerprint.hex());
  input.push_back(static_cast<char>(provenance.kind));
  appendField(input, provenance.contract_fingerprint.hex());
  appendU32(input, language_contract.source.major);
  appendU32(input, language_contract.source.minor);
  appendU32(input, language_contract.semantic_artifact_epoch);
  appendU32(input, language_contract.standard_library_epoch);
  input.push_back(cffi_identity ? 1 : 0);
  if (cffi_identity)
    appendField(input, cffi_identity->fingerprint().hex());
  appendU32(input, static_cast<std::uint32_t>(features.size()));
  for (const auto &feature : features)
    appendField(input, feature);
  return StableFingerprint::fromCanonicalBytes(input);
}

StableFingerprint defaultCompileToolchainFingerprint() {
  return StableFingerprint::fromCanonicalBytes(
      "chtholly.next.default-compile-toolchain.v5\n"
      "chtholly.next.language-semantics.v4");
}

StableFingerprint fingerprintObject(std::string_view target_triple,
                                    std::string_view object_bytes,
                                    StableFingerprint specific_fingerprint) {
  std::string input;
  appendField(input, "chtholly.next.object.v2");
  appendField(input, target_triple);
  appendFingerprint(input, specific_fingerprint);
  appendField(input, object_bytes);
  return StableFingerprint::fromCanonicalBytes(input);
}

std::string_view dependencyObservationKindName(DependencyObservationKind kind) {
  switch (kind) {
  case DependencyObservationKind::ModulePresence:
    return "module-presence";
  case DependencyObservationKind::EntityBinding:
    return "entity-binding";
  case DependencyObservationKind::ExportSet:
    return "export-set";
  case DependencyObservationKind::LifecycleCallable:
    return "lifecycle-callable";
  case DependencyObservationKind::NominalBinding:
    return "nominal-binding";
  case DependencyObservationKind::RelocationClosure:
    return "relocation-closure";
  case DependencyObservationKind::Count:
    return "invalid";
  }
  return "invalid";
}

std::string_view unitCompilationActionName(UnitCompilationAction action) {
  switch (action) {
  case UnitCompilationAction::Rebuild:
    return "rebuild";
  case UnitCompilationAction::Reuse:
    return "reuse";
  case UnitCompilationAction::Removed:
    return "removed";
  case UnitCompilationAction::Count:
    return "invalid";
  }
  return "invalid";
}

std::string_view unitInvalidationReasonName(UnitInvalidationReason reason) {
  switch (reason) {
  case UnitInvalidationReason::ModuleAdded:
    return "module-added";
  case UnitInvalidationReason::ModuleRemoved:
    return "module-removed";
  case UnitInvalidationReason::SourceChanged:
    return "source-changed";
  case UnitInvalidationReason::SourceKindChanged:
    return "source-kind-changed";
  case UnitInvalidationReason::ObjectEmissionRoleChanged:
    return "object-emission-role-changed";
  case UnitInvalidationReason::CompilationConfigurationChanged:
    return "compilation-configuration-changed";
  case UnitInvalidationReason::RequestedOutput:
    return "requested-output";
  case UnitInvalidationReason::ObjectArtifactMissing:
    return "object-artifact-missing";
  case UnitInvalidationReason::ObjectArtifactCorrupt:
    return "object-artifact-corrupt";
  case UnitInvalidationReason::ImportedModuleRemoved:
    return "imported-module-removed";
  case UnitInvalidationReason::EntityBindingChanged:
    return "entity-binding-changed";
  case UnitInvalidationReason::LifecycleCallableChanged:
    return "lifecycle-callable-changed";
  case UnitInvalidationReason::ExportSetChanged:
    return "export-set-changed";
  case UnitInvalidationReason::NominalBindingChanged:
    return "nominal-binding-changed";
  case UnitInvalidationReason::RelocationClosureChanged:
    return "relocation-closure-changed";
  case UnitInvalidationReason::Count:
    return "invalid";
  }
  return "invalid";
}


} // namespace chtholly::compiler
