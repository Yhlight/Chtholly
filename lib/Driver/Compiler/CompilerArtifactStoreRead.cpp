#include "CompilerArtifactStoreInternal.h"

#include "chtholly/Support/FileSystem.h"

#include <cerrno>
#include <chrono>
#include <fstream>
#include <sstream>
#include <system_error>

namespace chtholly {

ArtifactReadResult CompilerArtifactReadService::read(
    const std::string &path, CompilerArtifactLoadMetrics *metrics) {
  const auto started_at = metrics ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
  ArtifactReadResult result;
  errno = 0;
  std::ifstream input(pathForFileSystem(path), std::ios::binary);
  const auto open_error = errno;
  if (!input) {
    if (open_error == ENOENT) {
      result.status = CompilerArtifactReadStatus::Missing;
    } else {
      result.status = CompilerArtifactReadStatus::Error;
      const std::error_code code(open_error ? open_error : EIO,
                                 std::generic_category());
      result.error = "failed to open compiler artifact file: " + path + ": " +
                     code.message();
    }
  } else {
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
      result.status = CompilerArtifactReadStatus::Error;
      result.error = "failed to read compiler artifact file: " + path;
    } else {
      result.status = CompilerArtifactReadStatus::Found;
      result.bytes = buffer.str();
    }
  }
  if (metrics) {
    result.elapsed_nanoseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started_at)
            .count());
    metrics->recordArtifactRead(result.status, result.bytes.size(),
                                result.elapsed_nanoseconds);
  }
  return result;
}

} // namespace chtholly
