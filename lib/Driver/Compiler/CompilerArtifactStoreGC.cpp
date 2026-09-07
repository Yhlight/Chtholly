#include "CompilerArtifactStoreInternal.h"

#include "chtholly/Support/FileSystem.h"

#include <charconv>
#include <chrono>
#include <filesystem>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly {
namespace {

constexpr std::string_view GCStateMagic = "CHNXTGC1";
constexpr std::string_view RecoveryInstruction =
    "retain this cache namespace; retry with a fresh --cache-dir after compiler processes exit";

bool validHexKey(std::string_view value) {
  return value.size() == 64 &&
         std::ranges::all_of(value, [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

std::string manifestPath(
    const std::filesystem::path &root,
    const compiler::StableFingerprint &fingerprint) {
  return CompilerArtifactPathService::manifest(root, fingerprint);
}

std::string specializationComponentPath(
    const std::filesystem::path &root,
    const compiler::StableFingerprint &fingerprint) {
  return CompilerArtifactPathService::specialization(root, fingerprint);
}

struct ReachableArtifacts {
  std::set<std::string> manifests;
  std::set<std::string> objects;
  std::set<std::string> specialization_components;
  std::set<std::string> specialization_indices;
  std::set<std::string> nominal_type_specifics;
  std::set<std::string> nominal_type_specific_indices;
  std::set<std::string> nominal_semantic_witnesses;
  std::set<std::string> nominal_semantic_witness_indices;
  std::set<std::string> nominal_type_layouts;
  std::set<std::string> nominal_type_layout_indices;
};

bool markSpecializationClosure(
    const std::filesystem::path &root,
    const compiler::StableFingerprint &component_fingerprint,
    ReachableArtifacts &reachable, std::string &error) {
  const auto hex = component_fingerprint.hex();
  if (!reachable.specialization_components.insert(hex).second)
    return true;
  const auto bytes =
      readTextFile(specializationComponentPath(root, component_fingerprint),
                   error);
  auto component =
      bytes
          ? compiler::ConcreteSpecializationComponentArtifact::decode(*bytes,
                                                                      error)
          : std::nullopt;
  if (!component || component->fingerprint() != component_fingerprint) {
    if (error.empty())
      error = "specialization manifest closure contains a corrupt component";
    return false;
  }
  for (const auto &dependency : component->dependencies())
    if (!markSpecializationClosure(root, dependency, reachable, error))
      return false;
  return true;
}

bool loadManifestClosure(
    const std::filesystem::path &root,
    const compiler::StableFingerprint &root_fingerprint,
    std::map<std::string, compiler::CompilerPackageArtifactManifest> *manifests,
    ReachableArtifacts *reachable, std::string &error) {
  std::map<std::string, compiler::CompilerPackageArtifactManifest>
      local_manifests;
  auto *output_manifests = manifests ? manifests : &local_manifests;
  std::set<std::string> visiting;
  std::set<std::string> visited;
  const auto visit = [&](const auto &self,
                         const compiler::StableFingerprint &fingerprint) -> bool {
    const auto hex = fingerprint.hex();
    if (visited.contains(hex))
      return true;
    if (!visiting.insert(hex).second) {
      error = "compiler package manifest dependency closure contains a cycle";
      return false;
    }
    auto manifest = compiler::CompilerPackageArtifactManifest::load(
        manifestPath(root, fingerprint), error);
    if (!manifest || manifest->fingerprint() != fingerprint) {
      if (error.empty())
        error = "compiler artifact reference points to an invalid manifest";
      return false;
    }
    const auto package_name = std::string(manifest->packageName());
    std::vector<std::string> object_fingerprints;
    object_fingerprints.reserve(manifest->modules().size());
    for (const auto &module : manifest->modules()) {
      object_fingerprints.push_back(module.object_fingerprint.hex());
      if (!reachable)
        continue;
      for (const auto &reference : module.specializations) {
        reachable->specialization_indices.insert(
            reference.request_fingerprint.hex());
        if (!markSpecializationClosure(root, reference.component_fingerprint,
                                       *reachable, error))
          return false;
      }
      for (const auto &reference : module.nominal_type_specifics) {
        reachable->nominal_type_specific_indices.insert(
            reference.request_fingerprint.hex());
        reachable->nominal_type_specifics.insert(
            reference.result_fingerprint.hex());
      }
      for (const auto &reference : module.nominal_semantic_witnesses) {
        reachable->nominal_semantic_witness_indices.insert(
            reference.request_fingerprint.hex());
        reachable->nominal_semantic_witnesses.insert(
            reference.result_fingerprint.hex());
      }
      for (const auto &reference : module.nominal_type_layouts) {
        reachable->nominal_type_layout_indices.insert(
            reference.request_fingerprint.hex());
        reachable->nominal_type_layouts.insert(
            reference.result_fingerprint.hex());
      }
    }
    for (const auto &dependency : manifest->directDependencies())
      if (!self(self, dependency.manifest_fingerprint))
        return false;
    const auto [position, inserted] =
        output_manifests->emplace(package_name, std::move(*manifest));
    if (!inserted && position->second.fingerprint() != fingerprint) {
      error = "compiler manifest closure contains duplicate package identities";
      return false;
    }
    const auto &stored = position->second;
    std::vector<const compiler::CompilerPackageArtifactManifest *> dependencies;
    dependencies.reserve(stored.directDependencies().size());
    for (const auto &dependency : stored.directDependencies()) {
      const auto found = output_manifests->find(dependency.package_name);
      if (found == output_manifests->end()) {
        error = "compiler manifest closure omitted a direct dependency";
        return false;
      }
      dependencies.push_back(&found->second);
    }
    if (!stored.verifyDependencies(dependencies, error))
      return false;
    if (reachable) {
      reachable->manifests.insert(hex);
      reachable->objects.insert(object_fingerprints.begin(),
                                object_fingerprints.end());
    }
    visiting.erase(hex);
    visited.insert(hex);
    return true;
  };
  return visit(visit, root_fingerprint);
}

bool verifyReferenceClosureImpl(
    const std::filesystem::path &root,
    const CompilerSessionArtifactReference &reference,
    std::map<std::string, compiler::CompilerPackageArtifactManifest> *manifests,
    ReachableArtifacts *reachable, std::string &error) {
  std::map<std::string, compiler::CompilerPackageArtifactManifest> local;
  auto *loaded = manifests ? manifests : &local;
  if (!loadManifestClosure(root, reference.root_manifest, loaded, reachable,
                           error))
    return false;
  if (std::ranges::any_of(*loaded, [&](const auto &entry) {
        return entry.second.targetTriple() != reference.target_triple;
      })) {
    error = "compiler session manifest closure has an inconsistent target";
    return false;
  }
  const auto root_manifest = loaded->find(reference.root_package);
  if (root_manifest == loaded->end() ||
      root_manifest->second.fingerprint() != reference.root_manifest) {
    error = "compiler session reference does not resolve to its root package";
    return false;
  }
  return true;
}

} // namespace

bool CompilerArtifactGCService::verifyReferenceClosure(
    const std::filesystem::path &root,
    const CompilerSessionArtifactReference &reference,
    std::map<std::string, compiler::CompilerPackageArtifactManifest> *manifests,
    std::string &error) {
  return verifyReferenceClosureImpl(root, reference, manifests, nullptr, error);
}

bool CompilerArtifactGCService::collect(
    bool force, std::chrono::seconds minimum_interval,
    CompilerGarbageCollectionReport &report, std::string &error,
    CompilerArtifactGCState &state) {
  report.valid = true;
  report.recovery_instruction.clear();
  const auto &root = state.root;
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const auto state_path = (root / "gc" / "state").string();
  std::error_code file_error;
  if (!force &&
      std::filesystem::exists(pathForFileSystem(state_path), file_error)) {
    std::string state_error;
    const auto state_bytes = readTextFile(state_path, state_error);
    std::istringstream input(state_bytes ? *state_bytes : std::string{});
    std::string magic;
    std::string record;
    std::int64_t previous = 0;
    if (!state_bytes || !std::getline(input, magic) ||
        magic != GCStateMagic || !std::getline(input, record) ||
        !record.starts_with("last\t")) {
      error = "compiler GC state has an invalid encoding";
      return false;
    }
    const auto parsed = std::from_chars(
        record.data() + 5, record.data() + record.size(), previous);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != record.data() + record.size()) {
      error = "compiler GC state has an invalid timestamp";
      return false;
    }
    if (now >= previous && now - previous < minimum_interval.count())
      return true;
  } else if (file_error) {
    error = "failed to inspect compiler GC state: " + file_error.message();
    return false;
  }

  ReachableArtifacts reachable;
  const auto refs = root / "refs";
  if (std::filesystem::exists(refs, file_error)) {
    for (std::filesystem::directory_iterator iterator(refs, file_error), end;
         !file_error && iterator != end; iterator.increment(file_error)) {
      if (!iterator->is_regular_file(file_error) ||
          iterator->path().extension() != ".ref")
        continue;
      std::string read_error;
      const auto bytes = readTextFile(iterator->path().string(), read_error);
      const auto reference =
          bytes ? CompilerSessionArtifactReference::decode(*bytes, read_error)
                : std::nullopt;
      if (!reference ||
          !verifyReferenceClosureImpl(root, *reference, nullptr, &reachable,
                                      read_error)) {
        report.valid = false;
        report.recovery_instruction = std::string(RecoveryInstruction);
        error = "compiler GC aborted on invalid reference '" +
                iterator->path().filename().string() + "': " + read_error +
                "; the namespace was retained without deleting artifacts; "
                "retry with a fresh --cache-dir or remove this cache "
                "namespace only after all compiler processes exit";
        return false;
      }
    }
  }
  if (file_error) {
    error =
        "failed to enumerate compiler session references: " +
        file_error.message();
    return false;
  }

  std::vector<std::filesystem::path> stale_leases;
  const auto leases = root / "leases";
  file_error.clear();
  if (std::filesystem::exists(leases, file_error)) {
    for (std::filesystem::directory_iterator iterator(leases, file_error), end;
         !file_error && iterator != end; iterator.increment(file_error)) {
      if (!iterator->is_regular_file(file_error) ||
          iterator->path().extension() != ".lease")
        continue;
      std::string probe_error;
      const auto probe = state.probe_lease(iterator->path(), probe_error);
      if (probe == CompilerArtifactLeaseProbe::Error) {
        error = probe_error;
        return false;
      }
      if (probe == CompilerArtifactLeaseProbe::Stale) {
        stale_leases.push_back(iterator->path());
        continue;
      }
      const auto lease_id = iterator->path().stem().string();
      std::string read_error;
      const auto bytes = readTextFile(iterator->path().string(), read_error);
      const auto lease_record =
          bytes ? CompilerArtifactCodecService::decodeLease(*bytes, read_error)
                : std::nullopt;
      std::map<std::string, compiler::CompilerPackageArtifactManifest>
          manifests;
      if (!validHexKey(lease_id) || !lease_record ||
          !loadManifestClosure(root, lease_record->root_manifest, &manifests,
                               &reachable, read_error)) {
        report.valid = false;
        report.recovery_instruction = std::string(RecoveryInstruction);
        error = "compiler GC aborted on invalid active lease '" +
                iterator->path().filename().string() + "': " + read_error;
        return false;
      }
      const auto root_manifest = manifests.find(lease_record->root_package);
      if (root_manifest == manifests.end() ||
          root_manifest->second.fingerprint() != lease_record->root_manifest ||
          std::ranges::any_of(manifests, [&](const auto &entry) {
            return entry.second.targetTriple() != lease_record->target_triple;
          })) {
        report.valid = false;
        report.recovery_instruction = std::string(RecoveryInstruction);
        error = "compiler GC aborted on inconsistent active lease closure";
        return false;
      }
      ++report.active_lease_count;
    }
  }
  if (file_error) {
    error =
        "failed to enumerate compiler artifact leases: " +
        file_error.message();
    return false;
  }
  for (const auto &stale : stale_leases) {
    file_error.clear();
    if (!std::filesystem::remove(stale, file_error) && file_error) {
      error =
          "failed to remove stale compiler artifact lease: " +
          file_error.message();
      return false;
    }
    ++report.stale_lease_count;
  }

  const auto trash_root = root / "trash";
  file_error.clear();
  if (std::filesystem::exists(trash_root, file_error)) {
    std::filesystem::remove_all(trash_root, file_error);
    if (file_error) {
      error = "failed to remove abandoned compiler GC quarantine: " +
              file_error.message();
      return false;
    }
  } else if (file_error) {
    error =
        "failed to inspect compiler GC quarantine: " + file_error.message();
    return false;
  }

  // Capture byte accounting before the sweep mutates the namespace.  The
  // existing count fields retain their historical meaning (files reclaimed
  // by this invocation); the additive byte fields describe the complete input
  // set and its mark result.
  const auto measure_group = [&](std::string_view group,
                                 const std::set<std::string> &marked,
                                 std::uintmax_t &total,
                                 std::uintmax_t &reachable_bytes,
                                 std::uintmax_t &unreachable_bytes) -> bool {
    const auto group_root = root / group;
    file_error.clear();
    if (!std::filesystem::exists(group_root, file_error))
      return !file_error;
    for (std::filesystem::recursive_directory_iterator
             iterator(group_root, file_error),
         end;
         !file_error && iterator != end; iterator.increment(file_error)) {
      if (!iterator->is_regular_file(file_error))
        continue;
      std::error_code size_error;
      const auto size = iterator->file_size(size_error);
      if (size_error) {
        error = "failed to inspect compiler CAS artifact size: " +
                size_error.message();
        return false;
      }
      total += size;
      const auto filename = iterator->path().filename().string();
      const auto dot = filename.find('.');
      const auto digest = filename.substr(0, dot);
      if (validHexKey(digest) && marked.contains(digest))
        reachable_bytes += size;
      else
        unreachable_bytes += size;
    }
    if (file_error) {
      error = "failed to enumerate compiler CAS artifacts: " +
              file_error.message();
      return false;
    }
    return true;
  };
  if (!measure_group("manifests", reachable.manifests,
                    report.manifest_total_bytes,
                    report.manifest_reachable_bytes,
                    report.manifest_unreachable_bytes) ||
      !measure_group("objects", reachable.objects,
                    report.object_total_bytes,
                    report.object_reachable_bytes,
                    report.object_unreachable_bytes) ||
      !measure_group("specializations", reachable.specialization_components,
                    report.specialization_component_total_bytes,
                    report.specialization_component_reachable_bytes,
                    report.specialization_component_unreachable_bytes) ||
      !measure_group("specialization-index", reachable.specialization_indices,
                    report.specialization_index_total_bytes,
                    report.specialization_index_reachable_bytes,
                    report.specialization_index_unreachable_bytes) ||
      !measure_group("type-specifics", reachable.nominal_type_specifics,
                    report.nominal_type_specific_total_bytes,
                    report.nominal_type_specific_reachable_bytes,
                    report.nominal_type_specific_unreachable_bytes) ||
      !measure_group("type-specific-index",
                    reachable.nominal_type_specific_indices,
                    report.nominal_type_specific_index_total_bytes,
                    report.nominal_type_specific_index_reachable_bytes,
                    report.nominal_type_specific_index_unreachable_bytes) ||
      !measure_group("nominal-semantic-witnesses",
                    reachable.nominal_semantic_witnesses,
                    report.nominal_semantic_witness_total_bytes,
                    report.nominal_semantic_witness_reachable_bytes,
                    report.nominal_semantic_witness_unreachable_bytes) ||
      !measure_group("nominal-semantic-witness-index",
                    reachable.nominal_semantic_witness_indices,
                    report.nominal_semantic_witness_index_total_bytes,
                    report.nominal_semantic_witness_index_reachable_bytes,
                    report.nominal_semantic_witness_index_unreachable_bytes) ||
      !measure_group("type-layouts", reachable.nominal_type_layouts,
                    report.nominal_type_layout_total_bytes,
                    report.nominal_type_layout_reachable_bytes,
                    report.nominal_type_layout_unreachable_bytes) ||
      !measure_group("type-layout-index", reachable.nominal_type_layout_indices,
                    report.nominal_type_layout_index_total_bytes,
                    report.nominal_type_layout_index_reachable_bytes,
                    report.nominal_type_layout_index_unreachable_bytes))
    return false;

  const auto sweep =
      std::to_string(now) + "." +
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());
  const auto trash = root / "trash" / sweep;
  const auto quarantine = [&](const std::filesystem::path &path,
                              std::string_view group) -> bool {
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    if (!size_error) {
      report.quarantine_bytes += size;
      report.reclaimed_bytes += size;
    }
    const auto destination = trash / group / path.filename();
    std::filesystem::create_directories(destination.parent_path(), file_error);
    if (!file_error)
      std::filesystem::rename(path, destination, file_error);
    if (file_error) {
      error = "failed to quarantine unreachable compiler artifact: " +
              file_error.message();
      return false;
    }
    return true;
  };
  const auto sweep_group =
      [&](std::string_view group, const std::set<std::string> &marked,
          std::string_view extension, std::size_t &count) -> bool {
    const auto group_root = root / group;
    std::vector<std::filesystem::path> candidates;
    file_error.clear();
    if (!std::filesystem::exists(group_root, file_error))
      return !file_error;
    for (std::filesystem::recursive_directory_iterator
             iterator(group_root, file_error),
         end;
         !file_error && iterator != end; iterator.increment(file_error)) {
      if (!iterator->is_regular_file(file_error))
        continue;
      const auto filename = iterator->path().filename().string();
      const auto dot = filename.find('.');
      const auto digest = filename.substr(0, dot);
      if (!validHexKey(digest) ||
          (extension.empty() ? dot == std::string::npos
                             : iterator->path().extension() != extension) ||
          marked.contains(digest))
        continue;
      candidates.push_back(iterator->path());
    }
    if (file_error) {
      error =
          "failed to enumerate compiler CAS artifacts: " +
          file_error.message();
      return false;
    }
    for (const auto &candidate : candidates) {
      if (!quarantine(candidate, group))
        return false;
      ++count;
    }
    return true;
  };
  if (!sweep_group("manifests", reachable.manifests, ".manifest",
                   report.manifest_count) ||
      !sweep_group("objects", reachable.objects, {}, report.object_count) ||
      !sweep_group("specializations", reachable.specialization_components,
                   ".specific", report.specialization_component_count) ||
      !sweep_group("specialization-index", reachable.specialization_indices,
                   ".ref", report.specialization_index_count) ||
      !sweep_group("type-specifics", reachable.nominal_type_specifics, ".type",
                   report.nominal_type_specific_count) ||
      !sweep_group("type-specific-index",
                   reachable.nominal_type_specific_indices, ".ref",
                   report.nominal_type_specific_index_count) ||
      !sweep_group("nominal-semantic-witnesses",
                   reachable.nominal_semantic_witnesses, ".witness",
                   report.nominal_semantic_witness_count) ||
      !sweep_group("nominal-semantic-witness-index",
                   reachable.nominal_semantic_witness_indices, ".ref",
                   report.nominal_semantic_witness_index_count) ||
      !sweep_group("type-layouts", reachable.nominal_type_layouts, ".layout",
                   report.nominal_type_layout_count) ||
      !sweep_group("type-layout-index", reachable.nominal_type_layout_indices,
                   ".ref", report.nominal_type_layout_index_count))
    return false;
  if (std::filesystem::exists(trash, file_error)) {
    file_error.clear();
    std::filesystem::remove_all(trash, file_error);
    if (file_error) {
      error = "failed to delete quarantined compiler artifacts: " +
              file_error.message();
      return false;
    }
  }
  const auto state_bytes =
      std::string(GCStateMagic) + "\nlast\t" + std::to_string(now) + "\n";
  if (!CompilerArtifactWriteService::atomic(state_path, state_bytes, error))
    return false;
  report.ran = true;
  return true;
}

} // namespace chtholly
