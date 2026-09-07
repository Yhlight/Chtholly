#include "chtholly/Driver/CompilerInputFileSystem.h"

#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace chtholly {
namespace {

void appendField(std::ostringstream &out, std::string_view value) {
  out << value.size() << ':';
  out.write(value.data(), static_cast<std::streamsize>(value.size()));
}

bool isSourcePath(std::string_view path) {
  const auto extension = pathForFileSystem(path).extension();
  return extension == ".cns" || extension == ".cfdl";
}

bool isUnderRoot(std::string_view path, std::string_view root) {
  const auto relative =
      pathForFileSystem(path).lexically_relative(pathForFileSystem(root));
  if (relative.empty() || relative.is_absolute())
    return false;
  const auto first = relative.begin();
  return first == relative.end() || *first != "..";
}

class CompilerRealInputFileSystem final : public CompilerInputFileSystem {
public:
  bool readText(std::string_view path, CompilerInputFile &file,
                std::string &error) const override {
    error.clear();
    file = {};
    const auto normalized = normalizeCompilerInputPath(path);
    if (normalized.empty()) {
      error = "invalid empty compiler input path";
      return false;
    }
    std::error_code status_error;
    const auto status =
        std::filesystem::status(pathForFileSystem(normalized), status_error);
    if (status_error) {
      if (status_error == std::errc::no_such_file_or_directory)
        return true;
      error = "failed to inspect compiler input '" + normalized +
              "': " + status_error.message();
      return false;
    }
    if (!std::filesystem::exists(status))
      return true;
    if (!std::filesystem::is_regular_file(status)) {
      error = "compiler input is not a regular file: " + normalized;
      return false;
    }
    auto text = readTextFile(normalized, error);
    if (!text)
      return false;
    file.exists = true;
    file.text = std::make_shared<const std::string>(std::move(*text));
    return true;
  }

  bool enumerateSources(std::string_view root, std::vector<std::string> &paths,
                        std::string &error) const override {
    error.clear();
    paths.clear();
    const auto normalized_root = normalizeCompilerInputPath(root);
    if (normalized_root.empty()) {
      error = "invalid empty compiler module root";
      return false;
    }
    std::error_code walk_error;
    const auto filesystem_root = pathForFileSystemTreeRoot(normalized_root);
    if (!std::filesystem::exists(filesystem_root, walk_error)) {
      if (walk_error) {
        error = "failed to inspect compiler module root '" + normalized_root +
                "': " + walk_error.message();
        return false;
      }
      return true;
    }
    for (std::filesystem::recursive_directory_iterator
             iterator(filesystem_root, walk_error),
         end;
         !walk_error && iterator != end; iterator.increment(walk_error)) {
      if (iterator->is_regular_file(walk_error) &&
          isSourcePath(iterator->path().string()))
        paths.push_back(normalizeCompilerInputPath(iterator->path().string()));
    }
    if (walk_error) {
      error = "failed to enumerate compiler module root '" + normalized_root +
              "': " + walk_error.message();
      return false;
    }
    std::ranges::sort(paths);
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return true;
  }
};

} // namespace

std::string normalizeCompilerInputPath(std::string_view path) {
  if (path.empty() || path.find('\0') != std::string_view::npos)
    return {};
  std::error_code error;
  const auto filesystem_path = pathForFileSystem(path);
  if (filesystem_path.empty())
    return {};
  auto normalized = std::filesystem::weakly_canonical(filesystem_path, error);
  if (error) {
    error.clear();
    normalized = std::filesystem::absolute(filesystem_path, error);
    if (error)
      return {};
  }
  return normalized.lexically_normal().generic_string();
}

std::shared_ptr<const CompilerInputFileSystem> makeCompilerRealInputFileSystem() {
  return std::make_shared<const CompilerRealInputFileSystem>();
}

compiler::StableFingerprint
CompilerOverlayInputFileSystem::fingerprintEntries(std::span<const Entry> entries) {
  std::ostringstream canonical;
  canonical << "chtholly.next.overlay-input.v2\n" << entries.size() << '\n';
  for (const auto &entry : entries) {
    appendField(canonical, entry.path);
    appendField(canonical, pathForFileSystem(entry.path).extension().string());
    canonical << (entry.exists ? "present\n" : "missing\n");
    if (entry.exists)
      appendField(canonical, *entry.text);
  }
  return compiler::StableFingerprint::fromCanonicalBytes(canonical.str());
}

std::optional<std::vector<CompilerOverlayInputFileSystem::Entry>>
CompilerOverlayInputFileSystem::applyChanges(
    std::span<const Entry> current, std::span<const CompilerOverlayChange> changes,
    std::string &error) {
  std::map<std::string, Entry> entries;
  for (const auto &entry : current)
    entries.emplace(entry.path, entry);
  std::set<std::string> changed_paths;
  for (const auto &change : changes) {
    const auto path = normalizeCompilerInputPath(change.path);
    if (path.empty() || !isSourcePath(path) ||
        !changed_paths.insert(path).second) {
      error = "compiler overlay contains an invalid or duplicate source path";
      return std::nullopt;
    }
    if (change.kind == CompilerOverlayChangeKind::RemoveOverride) {
      if (change.text) {
        error = "compiler overlay remove-override change cannot contain text";
        return std::nullopt;
      }
      entries.erase(path);
      continue;
    }
    if ((change.kind == CompilerOverlayChangeKind::Replace) !=
        change.text.has_value()) {
      error = "compiler overlay change payload does not match its operation";
      return std::nullopt;
    }
    Entry entry;
    entry.path = path;
    entry.exists = change.kind == CompilerOverlayChangeKind::Replace;
    if (change.text)
      entry.text = std::make_shared<const std::string>(*change.text);
    entries[path] = std::move(entry);
  }
  std::vector<Entry> result;
  result.reserve(entries.size());
  for (auto &[unused, entry] : entries) {
    (void)unused;
    result.push_back(std::move(entry));
  }
  return result;
}

std::shared_ptr<const CompilerOverlayInputFileSystem>
CompilerOverlayInputFileSystem::create(
    std::shared_ptr<const CompilerInputFileSystem> base,
    std::span<const CompilerOverlayChange> changes, std::string &error) {
  error.clear();
  if (!base) {
    error = "compiler overlay requires a base input file system";
    return {};
  }
  auto entries = applyChanges({}, changes, error);
  if (!entries)
    return {};
  const auto fingerprint = fingerprintEntries(*entries);
  return std::shared_ptr<const CompilerOverlayInputFileSystem>(
      new CompilerOverlayInputFileSystem(std::move(base), std::move(*entries),
                                     fingerprint));
}

std::shared_ptr<const CompilerOverlayInputFileSystem>
CompilerOverlayInputFileSystem::withChanges(
    std::span<const CompilerOverlayChange> changes, std::string &error) const {
  error.clear();
  auto entries = applyChanges(entries_, changes, error);
  if (!entries)
    return {};
  const auto fingerprint = fingerprintEntries(*entries);
  return std::shared_ptr<const CompilerOverlayInputFileSystem>(
      new CompilerOverlayInputFileSystem(base_, std::move(*entries), fingerprint));
}

bool CompilerOverlayInputFileSystem::readText(std::string_view path,
                                          CompilerInputFile &file,
                                          std::string &error) const {
  const auto normalized = normalizeCompilerInputPath(path);
  if (normalized.empty()) {
    error = "invalid empty compiler input path";
    file = {};
    return false;
  }
  const auto found =
      std::ranges::lower_bound(entries_, normalized, {}, &Entry::path);
  if (found == entries_.end() || found->path != normalized)
    return base_->readText(normalized, file, error);
  error.clear();
  file.exists = found->exists;
  file.text = found->text;
  return true;
}

bool CompilerOverlayInputFileSystem::enumerateSources(
    std::string_view root, std::vector<std::string> &paths,
    std::string &error) const {
  const auto normalized_root = normalizeCompilerInputPath(root);
  if (normalized_root.empty()) {
    error = "invalid empty compiler module root";
    paths.clear();
    return false;
  }
  if (!base_->enumerateSources(normalized_root, paths, error))
    return false;
  std::set<std::string> merged(paths.begin(), paths.end());
  for (const auto &entry : entries_) {
    if (!isUnderRoot(entry.path, normalized_root))
      continue;
    if (entry.exists)
      merged.insert(entry.path);
    else
      merged.erase(entry.path);
  }
  paths.assign(merged.begin(), merged.end());
  return true;
}

} // namespace chtholly
