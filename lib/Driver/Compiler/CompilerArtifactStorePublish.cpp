#include "CompilerArtifactStoreInternal.h"
#include "chtholly/Support/FileSystem.h"

#include <filesystem>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chtholly {
namespace {

std::string manifestPath(const std::filesystem::path &root,
                         const compiler::StableFingerprint &fingerprint) {
  return CompilerArtifactPathService::manifest(root, fingerprint);
}

std::string artifactObjectPath(const std::filesystem::path &root,
                               const compiler::StableFingerprint &fingerprint,
                               std::string_view extension) {
  return CompilerArtifactPathService::object(root, fingerprint, extension);
}

std::string
specializationComponentPath(const std::filesystem::path &root,
                            const compiler::StableFingerprint &fingerprint) {
  return CompilerArtifactPathService::specialization(root, fingerprint);
}

std::string
specializationIndexPath(const std::filesystem::path &root,
                        const compiler::StableFingerprint &fingerprint) {
  return CompilerArtifactPathService::specializationIndex(root, fingerprint);
}

std::string typeSpecificPath(const std::filesystem::path &root,
                             const compiler::StableFingerprint &fingerprint) {
  return CompilerArtifactPathService::typeSpecific(root, fingerprint);
}

std::string
typeSpecificIndexPath(const std::filesystem::path &root,
                      const compiler::StableFingerprint &fingerprint) {
  return CompilerArtifactPathService::typeSpecificIndex(root, fingerprint);
}

std::string
nominalSemanticWitnessPath(const std::filesystem::path &root,
                           const compiler::StableFingerprint &fingerprint) {
  return CompilerArtifactPathService::nominalWitness(root, fingerprint);
}

std::string nominalSemanticWitnessIndexPath(
    const std::filesystem::path &root,
    const compiler::StableFingerprint &fingerprint) {
  return CompilerArtifactPathService::nominalWitnessIndex(root, fingerprint);
}

std::string typeLayoutPath(const std::filesystem::path &root,
                           const compiler::StableFingerprint &fingerprint) {
  return CompilerArtifactPathService::typeLayout(root, fingerprint);
}

std::string
typeLayoutIndexPath(const std::filesystem::path &root,
                    const compiler::StableFingerprint &fingerprint) {
  return CompilerArtifactPathService::typeLayoutIndex(root, fingerprint);
}

std::string referencePath(const std::filesystem::path &root,
                          std::string_view session_key) {
  return CompilerArtifactPathService::reference(root, session_key);
}

} // namespace

bool CompilerArtifactPublishService::publish(
    const compiler::CompilerPackageArtifactManifest &root_manifest,
    std::span<const compiler::CompilerPackageArtifactManifest *const> manifests,
    std::span<const CompilerPublishedObject> objects,
    std::span<const CompilerPublishedSpecialization> specializations,
    std::span<const CompilerPublishedNominalTypeSpecific> nominal_specifics,
    std::span<const CompilerPublishedNominalSemanticWitness>
        nominal_semantic_witnesses,
    std::span<const CompilerPublishedNominalTypeLayout> nominal_layouts,
    std::string &error, CompilerArtifactPublishState &state) {
  const auto &root = state.root;
  struct PreparedManifest {
    compiler::StableFingerprint fingerprint;
    std::string bytes;
  };
  struct PreparedSpecialization {
    compiler::StableFingerprint fingerprint;
    std::string bytes;
    const compiler::ConcreteSpecializationComponentArtifact *component =
        nullptr;
  };
  std::map<std::string, const compiler::CompilerPackageArtifactManifest *>
      by_package;
  std::vector<PreparedManifest> prepared_manifests;
  prepared_manifests.reserve(manifests.size());
  for (const auto *manifest : manifests) {
    if (!manifest || !manifest->verify(error) ||
        !by_package.emplace(std::string(manifest->packageName()), manifest)
             .second) {
      if (error.empty())
        error = "compiler artifact publication contains invalid manifests";
      return false;
    }
  }
  for (const auto &[package, manifest] : by_package) {
    std::vector<const compiler::CompilerPackageArtifactManifest *> dependencies;
    for (const auto &dependency : manifest->directDependencies()) {
      const auto found = by_package.find(dependency.package_name);
      if (found == by_package.end()) {
        error =
            "compiler artifact publication has an incomplete manifest closure";
        return false;
      }
      dependencies.push_back(found->second);
    }
    if (!manifest->verifyDependencies(dependencies, error))
      return false;
    auto bytes = manifest->encode(error);
    if (!error.empty())
      return false;
    prepared_manifests.push_back(
        {.fingerprint = manifest->fingerprint(), .bytes = std::move(bytes)});
  }
  std::map<std::string, PreparedSpecialization> prepared_specializations;
  for (const auto &published : specializations) {
    if (!published.component.verify(error))
      return false;
    const auto fingerprint = published.component.fingerprint();
    auto bytes = published.component.encode(error);
    if (!fingerprint.hasValue() || !error.empty())
      return false;
    if (const auto found = prepared_specializations.find(fingerprint.hex());
        found != prepared_specializations.end()) {
      if (found->second.bytes != bytes) {
        error = "specialization publication contains a fingerprint collision";
        return false;
      }
      continue;
    }
    prepared_specializations.emplace(
        fingerprint.hex(), PreparedSpecialization{fingerprint, std::move(bytes),
                                                  &published.component});
  }
  std::map<std::string, compiler::ConcreteSpecializationComponentArtifact>
      stored_specializations;
  std::map<std::string, const compiler::NominalTypeSpecificArtifact *>
      prepared_nominal_specifics;
  for (const auto &published : nominal_specifics) {
    if (!published.artifact.verify(error))
      return false;
    const auto [position, inserted] = prepared_nominal_specifics.emplace(
        published.artifact.result_fingerprint.hex(), &published.artifact);
    if (!inserted && *position->second != published.artifact) {
      error = "nominal type specific publication contains a fingerprint "
              "collision";
      return false;
    }
  }
  std::map<std::string, const compiler::NominalSemanticWitnessArtifact *>
      prepared_nominal_semantic_witnesses;
  for (const auto &published : nominal_semantic_witnesses) {
    if (!published.artifact.verify(error))
      return false;
    const auto [position, inserted] =
        prepared_nominal_semantic_witnesses.emplace(
            published.artifact.result_fingerprint.hex(), &published.artifact);
    if (!inserted && *position->second != published.artifact) {
      error = "nominal semantic witness publication contains a fingerprint "
              "collision";
      return false;
    }
  }
  std::map<std::string, const compiler::NominalTypeLayoutArtifact *>
      prepared_nominal_layouts;
  for (const auto &published : nominal_layouts) {
    if (!published.artifact.verify(error))
      return false;
    const auto [position, inserted] = prepared_nominal_layouts.emplace(
        published.artifact.result_fingerprint.hex(), &published.artifact);
    if (!inserted && *position->second != published.artifact) {
      error = "nominal type layout publication contains a fingerprint "
              "collision";
      return false;
    }
  }
  const auto find_component =
      [&](const compiler::StableFingerprint &fingerprint)
      -> const compiler::ConcreteSpecializationComponentArtifact * {
    const auto key = fingerprint.hex();
    if (const auto prepared = prepared_specializations.find(key);
        prepared != prepared_specializations.end())
      return prepared->second.component;
    if (const auto stored = stored_specializations.find(key);
        stored != stored_specializations.end())
      return &stored->second;
    std::string load_error;
    const auto bytes = readTextFile(
        specializationComponentPath(root, fingerprint), load_error);
    auto component =
        bytes ? compiler::ConcreteSpecializationComponentArtifact::decode(
                    *bytes, load_error)
              : std::nullopt;
    if (!component || component->fingerprint() != fingerprint)
      return nullptr;
    const auto [position, unused] =
        stored_specializations.emplace(key, std::move(*component));
    (void)unused;
    return &position->second;
  };
  for (const auto &[unused, manifest] : by_package) {
    (void)unused;
    for (const auto &module : manifest->modules()) {
      for (const auto &reference : module.specializations) {
        const auto *component = find_component(reference.component_fingerprint);
        if (!component || !component->findNode(reference.request_fingerprint)) {
          error = "manifest references an unavailable specialization component";
          return false;
        }
      }
      for (const auto &reference : module.nominal_type_specifics) {
        const compiler::NominalTypeSpecificArtifact *specific = nullptr;
        std::optional<compiler::NominalTypeSpecificArtifact> stored_specific;
        if (const auto found = prepared_nominal_specifics.find(
                reference.result_fingerprint.hex());
            found != prepared_nominal_specifics.end()) {
          specific = found->second;
        } else {
          std::string load_error;
          const auto bytes = readTextFile(
              typeSpecificPath(root, reference.result_fingerprint), load_error);
          stored_specific =
              bytes ? compiler::NominalTypeSpecificArtifact::decode(*bytes,
                                                                    load_error)
                    : std::nullopt;
          specific = stored_specific ? &*stored_specific : nullptr;
        }
        if (!specific ||
            specific->request_fingerprint != reference.request_fingerprint ||
            specific->result_fingerprint != reference.result_fingerprint) {
          error = "manifest references an unavailable nominal type specific";
          return false;
        }
        const auto &witness = specific->nominal_semantic_witness;
        if (std::ranges::none_of(module.nominal_semantic_witnesses,
                                 [&](const auto &candidate) {
                                   return candidate.request_fingerprint ==
                                              witness.request_fingerprint &&
                                          candidate.result_fingerprint ==
                                              witness.result_fingerprint;
                                 })) {
          error =
              "manifest nominal specific omits its nominal semantic witness";
          return false;
        }
      }
      for (const auto &reference : module.nominal_semantic_witnesses) {
        if (const auto found = prepared_nominal_semantic_witnesses.find(
                reference.result_fingerprint.hex());
            found != prepared_nominal_semantic_witnesses.end()) {
          if (found->second->request_fingerprint !=
              reference.request_fingerprint) {
            error =
                "manifest nominal semantic witness reference disagrees with "
                "publication";
            return false;
          }
        } else {
          std::string load_error;
          const auto bytes = readTextFile(
              nominalSemanticWitnessPath(root, reference.result_fingerprint),
              load_error);
          auto artifact =
              bytes ? compiler::NominalSemanticWitnessArtifact::decode(
                          *bytes, load_error)
                    : std::nullopt;
          if (!artifact ||
              artifact->request_fingerprint != reference.request_fingerprint ||
              artifact->result_fingerprint != reference.result_fingerprint) {
            error =
                "manifest references an unavailable nominal semantic witness";
            return false;
          }
        }
      }
      for (const auto &reference : module.nominal_type_layouts) {
        if (const auto found = prepared_nominal_layouts.find(
                reference.result_fingerprint.hex());
            found != prepared_nominal_layouts.end()) {
          if (found->second->request_fingerprint !=
              reference.request_fingerprint) {
            error =
                "manifest nominal layout reference disagrees with publication";
            return false;
          }
        } else {
          std::string load_error;
          const auto bytes = readTextFile(
              typeLayoutPath(root, reference.result_fingerprint), load_error);
          auto artifact = bytes ? compiler::NominalTypeLayoutArtifact::decode(
                                      *bytes, load_error)
                                : std::nullopt;
          if (!artifact ||
              artifact->request_fingerprint != reference.request_fingerprint ||
              artifact->result_fingerprint != reference.result_fingerprint) {
            error = "manifest references an unavailable nominal type layout";
            return false;
          }
        }
      }
    }
  }
  const auto root_entry =
      by_package.find(std::string(root_manifest.packageName()));
  if (root_entry == by_package.end() || root_entry->second != &root_manifest) {
    error = "compiler artifact publication omitted its root manifest";
    return false;
  }
  for (const auto &object : objects) {
    if (!object.fingerprint.hasValue() ||
        !object.specific_fingerprint.hasValue() || object.extension.empty() ||
        object.target_triple != state.expected_target || object.bytes.empty() ||
        compiler::fingerprintObject(object.target_triple, object.bytes,
                                    object.specific_fingerprint) !=
            object.fingerprint) {
      error = "compiler artifact publication contains an invalid object";
      return false;
    }
  }
  CompilerSessionArtifactReference published_reference{
      .target_triple = std::string(root_manifest.targetTriple()),
      .root_package = std::string(root_manifest.packageName()),
      .root_manifest = root_manifest.fingerprint()};
  const auto reference_bytes = published_reference.encode(error);
  if (!error.empty())
    return false;

  return state.with_store_lock(
      [&]() -> bool {
        std::optional<CompilerSessionArtifactReference> current_reference;
        if (!state.load_current_reference(state.session_key, current_reference,
                                          error))
          return false;
        if (current_reference != state.observed_reference) {
          error = "compiler artifact publication conflict: the session root "
                  "changed "
                  "during compilation; retry the build";
          return false;
        }

        std::set<std::string> verified_components;
        std::set<std::string> visiting_components;
        const auto verify_component =
            [&](const auto &self,
                const compiler::StableFingerprint &fingerprint) -> bool {
          const auto key = fingerprint.hex();
          if (verified_components.contains(key))
            return true;
          if (!visiting_components.insert(key).second) {
            error =
                "specialization publication closure contains a component cycle";
            return false;
          }
          const compiler::ConcreteSpecializationComponentArtifact *component =
              nullptr;
          std::optional<compiler::ConcreteSpecializationComponentArtifact>
              stored;
          if (const auto prepared = prepared_specializations.find(key);
              prepared != prepared_specializations.end()) {
            component = prepared->second.component;
          } else {
            const auto path = specializationComponentPath(root, fingerprint);
            std::string load_error;
            const auto bytes = readTextFile(path, load_error);
            stored =
                bytes
                    ? compiler::ConcreteSpecializationComponentArtifact::decode(
                          *bytes, load_error)
                    : std::nullopt;
            if (!stored || stored->fingerprint() != fingerprint) {
              error = load_error.empty()
                          ? "specialization publication closure is unavailable"
                          : std::move(load_error);
              return false;
            }
            component = &*stored;
          }
          for (const auto &dependency : component->dependencies())
            if (!self(self, dependency))
              return false;
          visiting_components.erase(key);
          verified_components.insert(key);
          return true;
        };
        for (const auto &[unused, manifest] : by_package) {
          (void)unused;
          for (const auto &module : manifest->modules()) {
            for (const auto &reference : module.specializations)
              if (!verify_component(verify_component,
                                    reference.component_fingerprint))
                return false;
          }
        }

        for (const auto &object : objects) {
          const auto path =
              artifactObjectPath(root, object.fingerprint, object.extension);
          const auto loaded =
              state.load_object(object.fingerprint, object.specific_fingerprint,
                                object.target_triple, object.extension);
          if (loaded.status == compiler::ObjectArtifactLoadStatus::Error) {
            error = loaded.error;
            return false;
          }
          if (loaded.status != compiler::ObjectArtifactLoadStatus::Found &&
              !CompilerArtifactWriteService::atomic(path, object.bytes, error))
            return false;
        }
        for (const auto &[unused, specialization] : prepared_specializations) {
          (void)unused;
          const auto path =
              specializationComponentPath(root, specialization.fingerprint);
          std::error_code file_error;
          bool write = true;
          if (std::filesystem::exists(pathForFileSystem(path), file_error)) {
            std::string existing_error;
            const auto bytes = readTextFile(path, existing_error);
            auto existing =
                bytes
                    ? compiler::ConcreteSpecializationComponentArtifact::decode(
                          *bytes, existing_error)
                    : std::nullopt;
            write = !existing ||
                    existing->fingerprint() != specialization.fingerprint;
          } else if (file_error) {
            error = "failed to inspect specialization component: " +
                    file_error.message();
            return false;
          }
          if (write && !CompilerArtifactWriteService::atomic(
                           path, specialization.bytes, error))
            return false;
        }
        for (const auto &[unused, artifact] : prepared_nominal_specifics) {
          (void)unused;
          if (!CompilerArtifactWriteService::atomic(
                  typeSpecificPath(root, artifact->result_fingerprint),
                  artifact->encode(), error) ||
              !CompilerArtifactWriteService::atomic(
                  typeSpecificIndexPath(root, artifact->request_fingerprint),
                  CompilerArtifactCodecService::encodeNominalReference(
                      CompilerArtifactCodecService::NominalReferenceKind::
                          TypeSpecific,
                      artifact->result_fingerprint),
                  error))
            return false;
        }
        for (const auto &[unused, artifact] :
             prepared_nominal_semantic_witnesses) {
          (void)unused;
          if (!CompilerArtifactWriteService::atomic(
                  nominalSemanticWitnessPath(root,
                                             artifact->result_fingerprint),
                  artifact->encode(), error) ||
              !CompilerArtifactWriteService::atomic(
                  nominalSemanticWitnessIndexPath(
                      root, artifact->request_fingerprint),
                  CompilerArtifactCodecService::encodeNominalReference(
                      CompilerArtifactCodecService::NominalReferenceKind::
                          SemanticWitness,
                      artifact->result_fingerprint),
                  error))
            return false;
        }
        for (const auto &[unused, artifact] : prepared_nominal_layouts) {
          (void)unused;
          if (!CompilerArtifactWriteService::atomic(
                  typeLayoutPath(root, artifact->result_fingerprint),
                  artifact->encode(), error) ||
              !CompilerArtifactWriteService::atomic(
                  typeLayoutIndexPath(root, artifact->request_fingerprint),
                  CompilerArtifactCodecService::encodeNominalReference(
                      CompilerArtifactCodecService::NominalReferenceKind::
                          TypeLayout,
                      artifact->result_fingerprint),
                  error))
            return false;
        }
        for (const auto &[unused, manifest] : by_package) {
          (void)unused;
          for (const auto &module : manifest->modules()) {
            for (const auto &reference : module.specializations) {
              const auto bytes =
                  CompilerArtifactCodecService::encodeSpecializationReference(
                      reference.component_fingerprint);
              if (!CompilerArtifactWriteService::atomic(
                      specializationIndexPath(root,
                                              reference.request_fingerprint),
                      bytes, error))
                return false;
            }
          }
        }
        for (const auto &manifest : prepared_manifests) {
          const auto path = manifestPath(root, manifest.fingerprint);
          std::error_code file_error;
          if (std::filesystem::exists(pathForFileSystem(path), file_error)) {
            auto existing =
                compiler::CompilerPackageArtifactManifest::load(path, error);
            if (!existing || existing->fingerprint() != manifest.fingerprint) {
              if (error.empty())
                error = "compiler manifest CAS contains corrupt content";
              return false;
            }
          } else if (file_error) {
            error = "failed to inspect compiler manifest artifact: " +
                    file_error.message();
            return false;
          } else if (!CompilerArtifactWriteService::atomic(path, manifest.bytes,
                                                           error)) {
            return false;
          }
        }
        if (!CompilerArtifactWriteService::atomic(
                referencePath(root, state.session_key), reference_bytes, error))
          return false;
        state.retire_lease();
        return true;
      },
      error);
}

} // namespace chtholly
