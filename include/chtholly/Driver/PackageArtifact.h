#pragma once

#include "chtholly/Driver/ArtifactCompatibility.h"
#include "chtholly/Compiler/CFFIIdentity.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly {

namespace compiler::interop {
class ArtifactRegistry;
}

enum class ArtifactDependencyKind : std::uint8_t {
  ExactRecord = 1,
  LookupSet = 2,
  TraitImplementationSet = 3,
  TypeLayout = 4,
  AbiSurface = 5,
  TemplateBody = 6,
  OpaqueProjection = 7,
  ExportSet = 8,
  Storage = 9,
  CFacade = 10,
  StaticLifecycle = 11,
  StateMigration = 12,
};

struct ArtifactDependencyEdge {
  std::string consumer;
  ArtifactDependencyKind kind = ArtifactDependencyKind::ExactRecord;
  std::string provider_module;
  std::string query_key;
  std::string record_id;
  std::string result_digest;

  friend bool operator==(const ArtifactDependencyEdge &,
                         const ArtifactDependencyEdge &) = default;
  friend auto operator<=>(const ArtifactDependencyEdge &,
                          const ArtifactDependencyEdge &) = default;
};

inline constexpr std::string_view PackageArtifactFormatVersion = "21";

struct PackageArtifactFile {
  std::string sha256;
  std::string relative_path;
};

struct PackageArtifactCFFIReceipt {
  compiler::CFFIReceiptIdentity identity;
  PackageArtifactFile file;
};

struct PackageArtifactInterface {
  std::string module_name;
  std::string contract_digest;
  std::string implementation_digest;
  PackageArtifactFile file;
};

struct PackageArtifactModuleDependency {
  std::string module_name;
  std::string contract_digest;
  std::vector<std::string> template_use_keys;
  std::vector<ArtifactDependencyEdge> semantic_edges;
};

struct PackageArtifactModule {
  std::string module_name;
  PackageArtifactFile object;
  PackageArtifactInterface interface;
  std::string template_ir_digest;
  std::vector<PackageArtifactModuleDependency> dependencies;
};

struct PackageArtifactDependency {
  std::string package_name;
  std::string artifact_identity;
  PackageArtifactFile manifest;
  std::vector<std::string> requested_features;
  bool default_features = true;
};

struct PackageArtifactManifest {
  std::uint32_t format_version = 21;
  std::string manifest_path;
  std::string package_name;
  std::string artifact_identity;
  std::string closure_digest;
  std::string producer_compiler;
  std::string semantic_interface_format;
  TargetInfo target;
  AbiVersion abi_version = AbiVersion::V2;
  std::string runtime_abi;
  bool default_features = true;
  std::vector<std::string> resolved_features;
  // Single-module projection used when modules is empty.
  PackageArtifactFile object;
  std::optional<PackageArtifactFile> interop_bundle;
  std::optional<PackageArtifactCFFIReceipt> cffi_receipt;
  std::optional<PackageArtifactFile> component_contract;
  std::uint32_t component_abi_epoch = 0;
  std::string component_identity;
  std::string component_contract_digest;
  std::vector<PackageArtifactInterface> interfaces;
  std::vector<PackageArtifactModule> modules;
  std::vector<PackageArtifactDependency> dependencies;
  std::vector<std::string> native_link_libraries;
};

std::string packageArtifactIdentity(const PackageArtifactManifest &manifest);

std::optional<PackageArtifactManifest>
parsePackageArtifactManifest(std::string_view text, std::string manifest_path,
                             std::string &error);

std::optional<PackageArtifactManifest>
loadPackageArtifactManifest(const std::string &manifest_path,
                            const TargetInfo &target, AbiVersion abi_version,
                            std::string &error);

bool validatePackageArtifactCompatibility(
    const PackageArtifactManifest &manifest, const TargetInfo &target,
    AbiVersion abi_version, std::string &error);

bool writePackageArtifactManifest(PackageArtifactManifest &manifest,
                                  const std::string &manifest_path,
                                  std::string &error);

std::string packageArtifactResolvedPath(const PackageArtifactManifest &manifest,
                                        std::string_view relative_path);

bool loadPackageArtifactInterop(const PackageArtifactManifest &manifest,
                                compiler::interop::ArtifactRegistry &registry,
                                std::string &error);

} // namespace chtholly
