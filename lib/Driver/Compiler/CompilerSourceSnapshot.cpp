#include "chtholly/Driver/CompilerSourceSnapshot.h"

#include "chtholly/Driver/CompilerInputFileSystem.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include <functional>
#include <sstream>
#include <utility>

namespace chtholly {
namespace {

void appendField(std::ostringstream &out, std::string_view value) {
  out << value.size() << ':';
  out.write(value.data(), static_cast<std::streamsize>(value.size()));
}

std::optional<std::vector<std::string>>
canonicalPaths(std::span<const std::string> paths, std::string &error) {
  std::vector<std::string> result(paths.begin(), paths.end());
  if (std::ranges::any_of(result, [](const auto &path) {
        return path.empty() || path.find_first_of("\r\n") != std::string::npos;
      })) {
    error = "compiler source snapshot contains an invalid path";
    return std::nullopt;
  }
  std::ranges::sort(result);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  if (result.empty()) {
    error = "compiler source snapshot has no inputs";
    return std::nullopt;
  }
  return result;
}

compiler::StableFingerprint sourceInputFingerprint(std::string_view path,
                                               std::string_view text) {
  std::ostringstream canonical;
  canonical << "chtholly.next.source-input.v2\n";
  appendField(canonical, path);
  appendField(canonical, pathForFileSystem(path).extension().string());
  appendField(canonical, text);
  return compiler::StableFingerprint::fromCanonicalBytes(canonical.str());
}

compiler::StableFingerprint
snapshotFingerprint(std::span<const CompilerSourceSnapshot::Entry> entries) {
  std::ostringstream canonical;
  canonical << "chtholly.next.source-snapshot.v2\n" << entries.size() << '\n';
  for (const auto &entry : entries) {
    appendField(canonical, entry.path());
    appendField(canonical, entry.fingerprint().hex());
  }
  return compiler::StableFingerprint::fromCanonicalBytes(canonical.str());
}

} // namespace

std::optional<CompilerSourceSnapshot>
CompilerSourceSnapshot::capture(const CompilerInputFileSystem &file_system,
                            std::span<const std::string> normalized_paths,
                            std::string &error) {
  error.clear();
  auto paths = canonicalPaths(normalized_paths, error);
  if (!paths)
    return std::nullopt;

  std::vector<Entry> entries;
  entries.reserve(paths->size());
  for (auto &path : *paths) {
    CompilerInputFile file;
    if (!file_system.readText(path, file, error) || !file.exists ||
        !file.text) {
      if (error.empty())
        error = "input is missing";
      error = "failed to capture compiler source snapshot input '" + path +
              "': " + error;
      return std::nullopt;
    }
    entries.push_back(
        Entry(path, file.text, sourceInputFingerprint(path, *file.text)));
  }
  const auto fingerprint = snapshotFingerprint(entries);
  return CompilerSourceSnapshot(std::move(entries), fingerprint);
}

const CompilerSourceSnapshot::Entry *
CompilerSourceSnapshot::find(std::string_view normalized_path) const {
  const auto found =
      std::ranges::lower_bound(entries_, normalized_path, {}, &Entry::path);
  return found != entries_.end() && found->path() == normalized_path ? &*found
                                                                     : nullptr;
}

compiler::SourceInput
CompilerSourceSnapshot::sourceInput(const CompilerSourceSnapshot::Entry &entry) const {
  return compiler::SourceInput(entry.path_, entry.text_);
}

bool CompilerSourceSnapshot::verifyCurrentSources(
    const CompilerInputFileSystem &file_system,
    std::span<const std::string> normalized_paths, std::string &error) const {
  error.clear();
  auto paths = canonicalPaths(normalized_paths, error);
  if (!paths)
    return false;
  if (paths->size() != entries_.size() ||
      !std::ranges::equal(*paths, entries_, {}, std::identity{},
                          &Entry::path)) {
    error = "compiler source snapshot conflict: the source inventory changed; "
            "retry the build";
    return false;
  }
  for (const auto &entry : entries_) {
    CompilerInputFile file;
    std::string read_error;
    if (!file_system.readText(entry.path(), file, read_error) || !file.exists ||
        !file.text) {
      if (read_error.empty())
        read_error = "input is missing";
      error = "compiler source snapshot conflict: cannot reread '" +
              std::string(entry.path()) + "': " + read_error;
      return false;
    }
    if (sourceInputFingerprint(entry.path(), *file.text) !=
        entry.fingerprint()) {
      error = "compiler source snapshot conflict: source content changed for '" +
              std::string(entry.path()) + "'; retry the build";
      return false;
    }
  }
  return true;
}

} // namespace chtholly
