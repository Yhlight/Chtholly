#include "CompilerArtifactStoreInternal.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <string_view>

namespace chtholly {
namespace {

std::string makeLeaseId(std::string_view session_key, std::uint64_t attempt) {
  static std::atomic<std::uint64_t> sequence = 0;
  std::ostringstream identity;
  identity << "chtholly.next.artifact-lease.v1\n"
           << session_key << '\n'
           << std::chrono::system_clock::now().time_since_epoch().count()
           << '\n'
           << std::chrono::steady_clock::now().time_since_epoch().count()
           << '\n'
           << sequence.fetch_add(1, std::memory_order_relaxed) << '\n'
           << attempt << '\n';
  return sha256Hex(identity.str());
}

} // namespace

std::optional<CompilerArtifactLeasePlan>
CompilerArtifactAcquireService::prepare(std::string_view session_key,
                                        std::string_view expected_target,
                                        std::string_view expected_root,
                                        std::string &error,
                                        CompilerArtifactAcquireState &state) {
  std::optional<CompilerSessionArtifactReference> reference;
  if (!state.load_current_reference(session_key, reference, error))
    return std::nullopt;
  std::map<std::string, compiler::CompilerPackageArtifactManifest> manifests;
  std::string lease_path;
  if (reference) {
    if (reference->target_triple != expected_target ||
        reference->root_package != expected_root) {
      error = "compiler session reference has an invalid build identity";
      return std::nullopt;
    }
    if (!CompilerArtifactGCService::verifyReferenceClosure(
            state.root, *reference, &manifests, error))
      return std::nullopt;

    CompilerArtifactLeaseSnapshot record{
        .session_key = std::string(session_key),
        .target_triple = std::string(expected_target),
        .root_package = std::string(expected_root),
        .root_manifest = reference->root_manifest};
    const auto bytes = CompilerArtifactCodecService::encodeLease(record, error);
    if (!error.empty())
      return std::nullopt;
    for (std::uint64_t attempt = 0; attempt < 1024; ++attempt) {
      const auto candidate = CompilerArtifactPathService::lease(
          state.root, makeLeaseId(session_key, attempt));
      std::error_code file_error;
      const auto exists =
          std::filesystem::exists(pathForFileSystem(candidate), file_error);
      if (file_error) {
        error = "failed to inspect compiler artifact lease path: " +
                file_error.message();
        return std::nullopt;
      }
      if (!exists) {
        lease_path = candidate;
        break;
      }
    }
    if (lease_path.empty()) {
      error = "failed to allocate a unique compiler artifact lease";
      return std::nullopt;
    }
    if (!CompilerArtifactWriteService::atomic(lease_path, bytes, error)) {
      std::error_code file_error;
      removeFile(lease_path, file_error);
      return std::nullopt;
    }
  }

  return CompilerArtifactLeasePlan{.reference = std::move(reference),
                                   .manifests = std::move(manifests),
                                   .lease_path = std::move(lease_path)};
}
} // namespace chtholly
