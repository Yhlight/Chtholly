#include "chtholly/Compiler/IncrementalDependencies.h"

#include "ArtifactDecodeInternal.h"
#include "IncrementalDependenciesReaderInternal.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <limits>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace chtholly::compiler {
namespace {
using StateReader = internal::IncrementalDependenciesReader;
#include "IncrementalDependenciesCodecInternal.h"
} // namespace

StableFingerprint
fingerprintRelocationClosure(const PublicNominalTypeArtifact &nominal) {
  if (nominal.kind != NominalKind::ForeignResource)
    return {};
  return StableFingerprint::fromCanonicalBytes(
      std::string("chtholly.next.relocation-closure.v1\n") + nominal.encode());
}

CompilerPackageCheckArtifact::CompilerPackageCheckArtifact(
    std::string package_name, std::string target_triple,
    StableFingerprint compile_toolchain_fingerprint,
    std::vector<std::string> resolved_features,
    std::vector<PackageCheckDependency> direct_dependencies,
    std::vector<PackageModuleCheckArtifact> modules,
    PackageProvenance provenance, LanguageContract language_contract,
    std::optional<CFFIReceiptIdentity> cffi_identity)
    : package_name_(std::move(package_name)),
      target_triple_(std::move(target_triple)),
      compile_toolchain_fingerprint_(compile_toolchain_fingerprint),
      provenance_(canonicalProvenance(package_name_, provenance)),
      language_contract_(language_contract),
      cffi_identity_(std::move(cffi_identity)),
      resolved_features_(std::move(resolved_features)),
      direct_dependencies_(std::move(direct_dependencies)),
      modules_(std::move(modules)) {
  std::ranges::sort(resolved_features_);
  resolved_features_.erase(
      std::unique(resolved_features_.begin(), resolved_features_.end()),
      resolved_features_.end());
  std::ranges::sort(direct_dependencies_, {},
                    &PackageCheckDependency::package_name);
  for (auto &module : modules_)
    canonicalizeObservations(module.observations);
  for (auto &module : modules_) {
    std::ranges::sort(module.module_dependencies);
    std::ranges::sort(module.required_foreign_symbols);
  }
  std::ranges::sort(modules_, [](const auto &lhs, const auto &rhs) {
    return lhs.moduleName() < rhs.moduleName();
  });
}

const PackageModuleCheckArtifact *
CompilerPackageCheckArtifact::findModule(std::string_view name) const {
  const auto found = std::ranges::lower_bound(
      modules_, name, {}, &PackageModuleCheckArtifact::moduleName);
  return found != modules_.end() && found->moduleName() == name ? &*found
                                                                : nullptr;
}

StableFingerprint CompilerPackageCheckArtifact::fingerprint() const {
  std::string canonical = "chtholly.next.package-check.v12\n";
  appendField(canonical, package_name_);
  appendField(canonical, target_triple_);
  appendFingerprint(canonical, compile_toolchain_fingerprint_);
  canonical.push_back(static_cast<char>(provenance_.kind));
  appendFingerprint(canonical, provenance_.contract_fingerprint);
  appendU32(canonical, language_contract_.source.major);
  appendU32(canonical, language_contract_.source.minor);
  appendU32(canonical, language_contract_.semantic_artifact_epoch);
  appendU32(canonical, language_contract_.standard_library_epoch);
  canonical.push_back(cffi_identity_ ? 1 : 0);
  if (cffi_identity_)
    appendFingerprint(canonical, cffi_identity_->fingerprint());
  appendU32(canonical, static_cast<std::uint32_t>(resolved_features_.size()));
  for (const auto &feature : resolved_features_)
    appendField(canonical, feature);
  appendU32(canonical, static_cast<std::uint32_t>(direct_dependencies_.size()));
  for (const auto &dependency : direct_dependencies_) {
    appendField(canonical, dependency.package_name);
    appendFingerprint(canonical, dependency.artifact_fingerprint);
  }
  appendU32(canonical, static_cast<std::uint32_t>(modules_.size()));
  for (const auto &module : modules_) {
    appendField(canonical, module.moduleName());
    appendFingerprint(canonical, module.source_fingerprint);
    canonical.push_back(static_cast<char>(module.unit_kind));
    appendFingerprint(canonical, module.public_interface.fingerprint());
    appendFingerprint(canonical, module.specific_fingerprint);
    appendU32(canonical,
              static_cast<std::uint32_t>(module.module_dependencies.size()));
    for (const auto &dependency : module.module_dependencies) {
      appendField(canonical, dependency.package_name);
      appendField(canonical, dependency.module_name);
    }
    appendU32(canonical, static_cast<std::uint32_t>(
                             module.required_foreign_symbols.size()));
    for (const auto &symbol : module.required_foreign_symbols)
      appendForeignRequirement(canonical, symbol);
    appendU32(canonical,
              static_cast<std::uint32_t>(module.observations.size()));
    for (const auto &observation : module.observations) {
      canonical.push_back(static_cast<char>(observation.kind));
      appendField(canonical, observation.provider.package_name);
      appendField(canonical, observation.provider.module_name);
      appendField(canonical, observation.binding_name);
      appendFingerprint(canonical, observation.expected_fingerprint);
      appendField(canonical, observation.canonical_provider.package_name);
      appendField(canonical, observation.canonical_provider.module_name);
      appendField(canonical, observation.canonical_name);
      canonical.push_back(static_cast<char>(observation.lifecycle_role));
      appendFingerprint(canonical, observation.witness_fingerprint);
      appendFingerprint(canonical, observation.specific_closure_fingerprint);
    }
  }
  return StableFingerprint::fromCanonicalBytes(canonical);
}

CompilerPackageArtifactManifest::CompilerPackageArtifactManifest(
    std::string package_name, std::string target_triple,
    StableFingerprint compile_toolchain_fingerprint,
    std::vector<std::string> resolved_features,
    std::vector<PackageManifestDependency> direct_dependencies,
    std::vector<PackageModuleArtifact> modules, PackageProvenance provenance,
    LanguageContract language_contract,
    std::optional<CFFIReceiptIdentity> cffi_identity)
    : package_name_(std::move(package_name)),
      target_triple_(std::move(target_triple)),
      compile_toolchain_fingerprint_(compile_toolchain_fingerprint),
      provenance_(canonicalProvenance(package_name_, provenance)),
      language_contract_(language_contract),
      cffi_identity_(std::move(cffi_identity)),
      configuration_fingerprint_(fingerprintCompilationConfiguration(
          package_name_, target_triple_, compile_toolchain_fingerprint_,
          resolved_features, provenance_, language_contract_, cffi_identity_)),
      resolved_features_(std::move(resolved_features)),
      direct_dependencies_(std::move(direct_dependencies)),
      modules_(std::move(modules)) {
  std::ranges::sort(resolved_features_);
  resolved_features_.erase(
      std::unique(resolved_features_.begin(), resolved_features_.end()),
      resolved_features_.end());
  std::ranges::sort(direct_dependencies_, {},
                    &PackageManifestDependency::package_name);
  for (auto &module : modules_)
    canonicalizeObservations(module.observations);
  for (auto &module : modules_) {
    std::ranges::sort(module.module_dependencies);
    std::ranges::sort(module.required_foreign_symbols);
    std::ranges::sort(module.specializations, {},
                      [](const ConcreteSpecializationReference &reference) {
                        return reference.request_fingerprint.hex();
                      });
    const auto sort_nominal = [](auto &references) {
      std::ranges::sort(references, {}, [](const auto &reference) {
        return reference.request_fingerprint.hex();
      });
    };
    sort_nominal(module.nominal_type_specifics);
    sort_nominal(module.nominal_semantic_witnesses);
    sort_nominal(module.nominal_type_layouts);
  }
  std::ranges::sort(modules_, [](const auto &lhs, const auto &rhs) {
    return lhs.moduleName() < rhs.moduleName();
  });
}

const PackageManifestDependency *CompilerPackageArtifactManifest::findDependency(
    std::string_view package_name) const {
  const auto found =
      std::ranges::lower_bound(direct_dependencies_, package_name, {},
                               &PackageManifestDependency::package_name);
  return found != direct_dependencies_.end() &&
                 found->package_name == package_name
             ? &*found
             : nullptr;
}

const PackageModuleArtifact *
CompilerPackageArtifactManifest::findModule(std::string_view name) const {
  const auto found = std::ranges::lower_bound(
      modules_, name, {}, &PackageModuleArtifact::moduleName);
  return found != modules_.end() && found->moduleName() == name ? &*found
                                                                : nullptr;
}

std::string CompilerPackageArtifactManifest::encode(std::string &error) const {
  if (!verify(error))
    return {};
  std::string out(StateMagic);
  appendU32(out, StateFormatVersion);
  appendField(out, package_name_);
  appendField(out, target_triple_);
  appendFingerprint(out, compile_toolchain_fingerprint_);
  out.push_back(static_cast<char>(provenance_.kind));
  appendFingerprint(out, provenance_.contract_fingerprint);
  appendU32(out, language_contract_.source.major);
  appendU32(out, language_contract_.source.minor);
  appendU32(out, language_contract_.semantic_artifact_epoch);
  appendU32(out, language_contract_.standard_library_epoch);
  appendFingerprint(out, configuration_fingerprint_);
  out.push_back(cffi_identity_ ? 1 : 0);
  if (cffi_identity_)
    appendCFFIIdentity(out, *cffi_identity_);
  appendU32(out, static_cast<std::uint32_t>(resolved_features_.size()));
  for (const auto &feature : resolved_features_)
    appendField(out, feature);
  appendU32(out, static_cast<std::uint32_t>(direct_dependencies_.size()));
  for (const auto &dependency : direct_dependencies_) {
    appendField(out, dependency.package_name);
    appendFingerprint(out, dependency.manifest_fingerprint);
  }
  appendU32(out, static_cast<std::uint32_t>(modules_.size()));
  for (const auto &module : modules_) {
    appendFingerprint(out, module.source_fingerprint);
    out.push_back(static_cast<char>(module.unit_kind));
    out.push_back(static_cast<char>(module.emission_role));
    appendField(out, module.public_interface.packageName());
    appendField(out, module.public_interface.moduleName());
    appendFingerprint(out, module.public_interface.fingerprint());
    appendU32(out, static_cast<std::uint32_t>(
                       module.public_interface.functions().size()));
    for (const auto &function : module.public_interface.functions()) {
      appendField(out, function.name);
      out.push_back(function.member_owner ? 1 : 0);
      if (function.member_owner)
        appendPublicEntity(out, *function.member_owner);
      out.push_back(static_cast<char>(function.member_kind));
      appendField(out, function.canonical_package);
      appendField(out, function.canonical_module);
      appendField(out, function.canonical_name);
      appendU32(out, function.generic_parameter_count);
      appendU32(out, static_cast<std::uint32_t>(function.parameters.size()));
      for (const auto parameter : function.parameters)
        appendPublicType(out, parameter);
      appendU32(out,
                static_cast<std::uint32_t>(function.parameter_names.size()));
      for (const auto &name : function.parameter_names)
        appendField(out, name);
      appendU32(out,
                static_cast<std::uint32_t>(function.default_arguments.size()));
      for (const auto &argument : function.default_arguments) {
        out.push_back(argument ? 1 : 0);
        if (argument)
          appendConstantValue(out, *argument);
      }
      appendPublicType(out, function.return_type);
      out.push_back(static_cast<char>(function.execution_kind));
      appendU32(out, function.coroutine_constructor.epoch);
      out.push_back((function.coroutine_constructor.eager_start ? 0x01 : 0) |
                    (function.coroutine_constructor.left_to_right_exactly_once
                         ? 0x02
                         : 0) |
                    (function.coroutine_constructor.supports_root ? 0x04 : 0) |
                    (function.coroutine_constructor.supports_child ? 0x08 : 0));
      appendU32(out, function.nominal_constructor.epoch);
      out.push_back(
          static_cast<char>(function.nominal_constructor.result_kind));
      out.push_back(function.error_type ? 1 : 0);
      if (function.error_type)
        appendPublicType(out, *function.error_type);
      appendSemanticContract(out, function.semantic_contract);
      out.push_back(static_cast<char>(function.intrinsic_role));
      appendOwnershipSummary(out, function.ownership_summary);
      out.push_back(static_cast<char>(function.declaration_kind));
      out.push_back(function.is_unsafe ? 1 : 0);
      out.push_back(function.is_const ? 1 : 0);
      appendField(out, function.foreign_abi);
      appendField(out, function.external_symbol);
      appendForeignSignature(out, function.foreign_signature);
      out.push_back(function.interop_artifact ? 1 : 0);
      if (function.interop_artifact)
        appendInteropArtifactReference(out, *function.interop_artifact);
      out.push_back(function.generic_template ? 1 : 0);
      if (function.generic_template)
        appendTemplate(out, *function.generic_template);
      appendU32(out, static_cast<std::uint32_t>(function.constraints.size()));
      for (const auto &constraint : function.constraints)
        appendInterfaceConstraint(out, constraint);
      appendFingerprint(out, function.entity_fingerprint);
    }
    appendU32(out, static_cast<std::uint32_t>(
                       module.public_interface.nominalTypes().size()));
    for (const auto &nominal : module.public_interface.nominalTypes())
      appendField(out, nominal.encode());
    appendU32(out, static_cast<std::uint32_t>(
                       module.public_interface.values().size()));
    for (const auto &value : module.public_interface.values()) {
      out.push_back(static_cast<char>(value.kind));
      appendField(out, value.name);
      appendField(out, value.canonical_package);
      appendField(out, value.canonical_module);
      appendField(out, value.canonical_name);
      appendPublicType(out, value.type);
      appendConstantValue(out, value.value);
      appendFingerprint(out, value.entity_fingerprint);
    }
    appendU32(out, static_cast<std::uint32_t>(
                       module.public_interface.interfaceDeclarations().size()));
    for (const auto &declaration :
         module.public_interface.interfaceDeclarations())
      appendInterfaceDeclaration(out, declaration);
    appendU32(out, static_cast<std::uint32_t>(
                       module.public_interface.typeAliases().size()));
    for (const auto &alias : module.public_interface.typeAliases())
      appendTypeAlias(out, alias);
    appendU32(out, static_cast<std::uint32_t>(
                       module.public_interface.interfaceWitnesses().size()));
    for (const auto &witness : module.public_interface.interfaceWitnesses())
      appendInterfaceWitness(out, witness);
    appendU32(out,
              static_cast<std::uint32_t>(module.module_dependencies.size()));
    for (const auto &dependency : module.module_dependencies) {
      appendField(out, dependency.package_name);
      appendField(out, dependency.module_name);
    }
    appendU32(out, static_cast<std::uint32_t>(
                       module.required_foreign_symbols.size()));
    for (const auto &symbol : module.required_foreign_symbols)
      appendForeignRequirement(out, symbol);
    appendU32(out, static_cast<std::uint32_t>(module.observations.size()));
    for (const auto &observation : module.observations) {
      out.push_back(static_cast<char>(observation.kind));
      appendField(out, observation.provider.package_name);
      appendField(out, observation.provider.module_name);
      appendField(out, observation.binding_name);
      appendFingerprint(out, observation.expected_fingerprint);
      appendField(out, observation.canonical_provider.package_name);
      appendField(out, observation.canonical_provider.module_name);
      appendField(out, observation.canonical_name);
      out.push_back(static_cast<char>(observation.lifecycle_role));
      appendFingerprint(out, observation.witness_fingerprint);
      appendFingerprint(out, observation.specific_closure_fingerprint);
    }
    appendU32(out, static_cast<std::uint32_t>(module.specializations.size()));
    for (const auto &reference : module.specializations) {
      appendFingerprint(out, reference.request_fingerprint);
      appendFingerprint(out, reference.component_fingerprint);
    }
    const auto append_nominal_references = [&](const auto &references) {
      appendU32(out, static_cast<std::uint32_t>(references.size()));
      for (const auto &reference : references) {
        appendFingerprint(out, reference.request_fingerprint);
        appendFingerprint(out, reference.result_fingerprint);
      }
    };
    append_nominal_references(module.nominal_type_specifics);
    append_nominal_references(module.nominal_semantic_witnesses);
    append_nominal_references(module.nominal_type_layouts);
    appendFingerprint(out, module.specific_fingerprint);
    appendFingerprint(out, module.object_fingerprint);
  }
  return out;
}

std::optional<CompilerPackageArtifactManifest>
CompilerPackageArtifactManifest::decode(std::string_view bytes,
                                    std::string &error) {
  error.clear();
  internal::ArtifactDecodeContext decode_context(bytes.size());
  StateReader reader(bytes, decode_context, error);
  if (decode_context.failed())
    return std::nullopt;
  std::string_view magic;
  std::uint32_t version = 0;
  std::string package_name;
  std::string target_triple;
  StableFingerprint compile_toolchain;
  PackageProvenance provenance;
  LanguageContract language_contract;
  std::uint8_t provenance_kind = 0;
  StableFingerprint configuration;
  std::uint8_t has_cffi_identity = 0;
  std::optional<CFFIReceiptIdentity> cffi_identity;
  std::uint32_t feature_count = 0;
  std::uint32_t dependency_count = 0;
  std::uint32_t module_count = 0;
  if (!reader.readBytes(StateMagic.size(), magic) || magic != StateMagic ||
      !reader.readU32(version) || version != StateFormatVersion ||
      !reader.readString(package_name) || !reader.readString(target_triple) ||
      !reader.readFingerprint(compile_toolchain) ||
      !reader.readU8(provenance_kind) ||
      provenance_kind >=
          static_cast<std::uint8_t>(PackageProvenanceKind::Count) ||
      !reader.readFingerprint(provenance.contract_fingerprint) ||
      !reader.readU32(language_contract.source.major) ||
      !reader.readU32(language_contract.source.minor) ||
      !reader.readU32(language_contract.semantic_artifact_epoch) ||
      !reader.readU32(language_contract.standard_library_epoch) ||
      !reader.readFingerprint(configuration) ||
      !reader.readU8(has_cffi_identity) || has_cffi_identity > 1 ||
      (has_cffi_identity &&
       !readCFFIIdentity(reader, cffi_identity.emplace())) ||
      !reader.readU32(feature_count) || !reader.readRecords(feature_count, 4)) {
    error = "compiler package artifact manifest has an invalid header";
    return std::nullopt;
  }
  provenance.kind = static_cast<PackageProvenanceKind>(provenance_kind);
  std::vector<std::string> features(feature_count);
  for (auto &feature : features) {
    if (!reader.readString(feature)) {
      error = "compiler package artifact manifest is truncated";
      return std::nullopt;
    }
  }
  if (!std::ranges::is_sorted(features) ||
      std::adjacent_find(features.begin(), features.end()) != features.end()) {
    error = "compiler package artifact manifest has a non-canonical feature set";
    return std::nullopt;
  }
  if (!reader.readU32(dependency_count) ||
      !reader.readRecords(dependency_count, 4 + StableFingerprint::ByteCount)) {
    error = "compiler package artifact manifest is truncated";
    return std::nullopt;
  }
  std::vector<PackageManifestDependency> dependencies(dependency_count);
  for (auto &dependency : dependencies) {
    if (!reader.readString(dependency.package_name) ||
        !reader.readFingerprint(dependency.manifest_fingerprint)) {
      error = "compiler package artifact manifest is truncated";
      return std::nullopt;
    }
  }
  if (!std::ranges::is_sorted(dependencies, {},
                              &PackageManifestDependency::package_name) ||
      std::adjacent_find(dependencies.begin(), dependencies.end(),
                         [](const auto &lhs, const auto &rhs) {
                           return lhs.package_name == rhs.package_name;
                         }) != dependencies.end()) {
    error = "compiler package artifact manifest has a non-canonical dependency set";
    return std::nullopt;
  }
  if (!reader.readU32(module_count) ||
      !reader.readRecords(module_count, MinimumModuleEncodingSize)) {
    error = "compiler package artifact manifest is truncated";
    return std::nullopt;
  }
  std::vector<PackageModuleArtifact> modules(module_count);
  for (auto &module : modules) {
    std::uint8_t unit_kind = 0;
    std::uint8_t emission_role = 0;
    std::uint32_t module_dependency_count = 0;
    std::uint32_t foreign_symbol_count = 0;
    std::uint32_t observation_count = 0;
    std::uint32_t specialization_count = 0;
    if (!reader.readFingerprint(module.source_fingerprint) ||
        !reader.readU8(unit_kind) ||
        unit_kind >= static_cast<std::uint8_t>(CompilationUnitKind::Count) ||
        !reader.readU8(emission_role) ||
        emission_role >= static_cast<std::uint8_t>(ModuleEmissionRole::Count) ||
        !readInterface(reader, module.public_interface) ||
        !reader.readU32(module_dependency_count) ||
        !reader.readRecords(module_dependency_count, 8)) {
      error = "compiler package artifact manifest is truncated";
      return std::nullopt;
    }
    module.module_dependencies.resize(module_dependency_count);
    for (auto &dependency : module.module_dependencies) {
      if (!reader.readString(dependency.package_name) ||
          !reader.readString(dependency.module_name)) {
        error = "compiler package artifact manifest is truncated";
        return std::nullopt;
      }
    }
    if (!reader.readU32(foreign_symbol_count) ||
        !reader.readRecords(foreign_symbol_count, 4)) {
      error = "compiler package artifact manifest is truncated";
      return std::nullopt;
    }
    module.required_foreign_symbols.resize(foreign_symbol_count);
    for (auto &symbol : module.required_foreign_symbols) {
      std::uint8_t convention = 0;
      if (!reader.readString(symbol.logical_name) ||
          !reader.readString(symbol.external_symbol) ||
          !reader.readU8(convention) ||
          convention >=
              static_cast<std::uint8_t>(ForeignCallingConvention::Count) ||
          !reader.readFingerprint(symbol.signature_fingerprint)) {
        error = "compiler package artifact manifest is truncated";
        return std::nullopt;
      }
      symbol.calling_convention =
          static_cast<ForeignCallingConvention>(convention);
    }
    if (!reader.readU32(observation_count) ||
        !reader.readRecords(observation_count,
                            MinimumObservationEncodingSize)) {
      error = "compiler package artifact manifest is truncated";
      return std::nullopt;
    }
    module.unit_kind = static_cast<CompilationUnitKind>(unit_kind);
    module.emission_role = static_cast<ModuleEmissionRole>(emission_role);
    module.observations.resize(observation_count);
    for (auto &observation : module.observations) {
      if (!readObservation(reader, observation)) {
        error = "compiler package artifact manifest is truncated";
        return std::nullopt;
      }
    }
    if (!reader.readU32(specialization_count) ||
        !reader.readRecords(specialization_count,
                            StableFingerprint::ByteCount * 2)) {
      error = "compiler package artifact manifest is truncated";
      return std::nullopt;
    }
    module.specializations.resize(specialization_count);
    for (auto &reference : module.specializations) {
      if (!reader.readFingerprint(reference.request_fingerprint) ||
          !reader.readFingerprint(reference.component_fingerprint)) {
        error = "compiler package artifact manifest is truncated";
        return std::nullopt;
      }
    }
    const auto read_nominal_references = [&](auto &references) {
      std::uint32_t count = 0;
      if (!reader.readU32(count) ||
          !reader.readRecords(count, StableFingerprint::ByteCount * 2))
        return false;
      references.resize(count);
      for (auto &reference : references)
        if (!reader.readFingerprint(reference.request_fingerprint) ||
            !reader.readFingerprint(reference.result_fingerprint))
          return false;
      return true;
    };
    if (!read_nominal_references(module.nominal_type_specifics) ||
        !read_nominal_references(module.nominal_semantic_witnesses) ||
        !read_nominal_references(module.nominal_type_layouts)) {
      error = "compiler package manifest has truncated nominal artifact references";
      return std::nullopt;
    }
    if (!reader.readFingerprint(module.specific_fingerprint) ||
        !reader.readFingerprint(module.object_fingerprint)) {
      error = "compiler package artifact manifest is truncated";
      return std::nullopt;
    }
  }
  if (!reader.atEnd()) {
    error = "compiler package artifact manifest has trailing bytes";
    return std::nullopt;
  }
  CompilerPackageArtifactManifest state(
      std::move(package_name), std::move(target_triple), compile_toolchain,
      std::move(features), std::move(dependencies), std::move(modules),
      provenance, language_contract, std::move(cffi_identity));
  if (state.configuration_fingerprint_ != configuration || !state.verify(error))
    return std::nullopt;
  return state;
}

std::optional<CompilerPackageArtifactManifest>
CompilerPackageArtifactManifest::load(const std::string &path, std::string &error) {
  const auto bytes = readTextFile(path, error);
  return bytes ? decode(*bytes, error) : std::nullopt;
}

bool CompilerPackageArtifactManifest::save(const std::string &path,
                                       std::string &error) const {
  const auto bytes = encode(error);
  if (!error.empty())
    return false;
  std::error_code file_error;
  const auto filesystem_path = pathForFileSystem(path);
  if (const auto parent = filesystem_path.parent_path(); !parent.empty())
    std::filesystem::create_directories(parent, file_error);
  if (file_error) {
    error = "failed to create compiler package manifest directory: " +
            file_error.message();
    return false;
  }
  const auto temporary = uniqueStateTemporaryPath(path);
  if (!writeTextFile(temporary, bytes, error))
    return false;
  std::string staged_error;
  const auto staged = load(temporary, staged_error);
  if (!staged) {
    error = "failed to verify staged compiler package manifest: " + staged_error;
    removeFile(temporary, file_error);
    return false;
  }
  if (!replaceFile(temporary, path, file_error)) {
    error = "failed to publish compiler package artifact manifest: " +
            file_error.message();
    removeFile(temporary, file_error);
    return false;
  }
  return true;
}

StableFingerprint CompilerPackageArtifactManifest::fingerprint() const {
  std::string error;
  const auto bytes = encode(error);
  return error.empty()
             ? StableFingerprint::fromCanonicalBytes(
                   std::string("chtholly.next.package-manifest.v14") + bytes)
             : StableFingerprint{};
}

IncrementalCompilationPlan::IncrementalCompilationPlan(
    std::vector<UnitCompilationDecision> decisions)
    : decisions_(std::move(decisions)) {
  for (auto &decision : decisions_)
    canonicalizeInvalidations(decision.invalidations);
  std::ranges::sort(decisions_, {}, &UnitCompilationDecision::module_name);
}

const UnitCompilationDecision *
IncrementalCompilationPlan::find(std::string_view module_name) const {
  const auto found = std::ranges::lower_bound(
      decisions_, module_name, {}, &UnitCompilationDecision::module_name);
  return found != decisions_.end() && found->module_name == module_name
             ? &*found
             : nullptr;
}

bool IncrementalCompilationPlan::rebuilds(std::string_view module_name) const {
  const auto *decision = find(module_name);
  return decision && decision->action == UnitCompilationAction::Rebuild;
}

bool IncrementalCompilationPlan::reuses(std::string_view module_name) const {
  const auto *decision = find(module_name);
  return decision && decision->action == UnitCompilationAction::Reuse;
}

} // namespace chtholly::compiler
