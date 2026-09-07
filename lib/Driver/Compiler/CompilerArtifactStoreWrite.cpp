#include "CompilerArtifactStoreInternal.h"

#include "chtholly/Support/FileSystem.h"

#include <atomic>
#include <chrono>
#include <system_error>

namespace chtholly {
namespace {

std::string uniqueTemporaryPath(const std::string &path) {
  static std::atomic<std::uint64_t> sequence = 0;
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return path + ".tmp." + std::to_string(stamp) + "." +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

} // namespace

bool CompilerArtifactWriteService::atomic(const std::string &path,
                                          const std::string &bytes,
                                          std::string &error) {
  std::error_code file_error;
  const auto parent = pathForFileSystem(path).parent_path();
  if (!parent.empty())
    std::filesystem::create_directories(parent, file_error);
  if (file_error) {
    error = "failed to create compiler artifact directory: " +
            file_error.message();
    return false;
  }
  const auto temporary = uniqueTemporaryPath(path);
  if (!writeTextFile(temporary, bytes, error))
    return false;
  if (!replaceFile(temporary, path, file_error)) {
    error = "failed to publish compiler artifact: " + file_error.message();
    removeFile(temporary, file_error);
    return false;
  }
  return true;
}

} // namespace chtholly
