#include "chtholly/Driver/CompilerBuildControlSnapshot.h"

#include "chtholly/Driver/CompilerInputFileSystem.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <utility>

namespace chtholly {
namespace {

void appendField(std::ostringstream &out, std::string_view value) {
  out << value.size() << ':';
  out.write(value.data(), static_cast<std::streamsize>(value.size()));
}

struct ControlPath {
  std::string path;
  bool required = false;
};

std::optional<std::vector<ControlPath>>
canonicalPaths(CompilerBuildControlInputs &inputs, std::string &error) {
  std::map<std::string, bool> paths;
  const auto add = [&](std::span<const std::string> values, bool required) {
    for (const auto &value : values) {
      const auto path = normalizeCompilerInputPath(value);
      if (path.empty() || path.find_first_of("\r\n") != std::string::npos) {
        error = "compiler build-control snapshot contains an invalid path";
        return false;
      }
      const auto [found, inserted] = paths.emplace(path, required);
      if (!inserted && found->second != required) {
        error = "compiler build-control input cannot be both required and optional";
        return false;
      }
    }
    return true;
  };
  if (!add(inputs.required_files, true) || !add(inputs.optional_files, false))
    return std::nullopt;
  std::vector<ControlPath> result;
  result.reserve(paths.size());
  for (auto &[path, required] : paths)
    result.push_back({std::move(path), required});
  return result;
}

compiler::StableFingerprint entryFingerprint(std::string_view path, bool required,
                                         const CompilerInputFile &file) {
  std::ostringstream canonical;
  canonical << "chtholly.next.build-control-entry.v1\n";
  appendField(canonical, path);
  canonical << (required ? "required\n" : "optional\n")
            << (file.exists ? "present\n" : "missing\n");
  if (file.exists)
    appendField(canonical, *file.text);
  return compiler::StableFingerprint::fromCanonicalBytes(canonical.str());
}

compiler::StableFingerprint
snapshotFingerprint(std::span<const CompilerBuildControlSnapshot::Entry> entries,
                    const CompilerBuildControlInputs &inputs) {
  std::ostringstream canonical;
  canonical << "chtholly.next.build-control-snapshot.v1\n"
            << entries.size() << '\n';
  for (const auto &entry : entries) {
    appendField(canonical, entry.path());
    appendField(canonical, entry.fingerprint().hex());
  }
  appendField(canonical, inputs.resolution_fingerprint.hex());
  appendField(canonical, inputs.compile_toolchain_fingerprint.hex());
  appendField(canonical, inputs.link_toolchain_fingerprint.hex());
  return compiler::StableFingerprint::fromCanonicalBytes(canonical.str());
}

} // namespace

std::optional<CompilerBuildControlSnapshot>
CompilerBuildControlSnapshot::capture(const CompilerInputFileSystem &file_system,
                                  CompilerBuildControlInputs inputs,
                                  std::string &error) {
  error.clear();
  if (!inputs.resolution_fingerprint.hasValue() ||
      !inputs.compile_toolchain_fingerprint.hasValue() ||
      !inputs.link_toolchain_fingerprint.hasValue()) {
    error = "compiler build-control snapshot has an invalid configuration";
    return std::nullopt;
  }
  auto paths = canonicalPaths(inputs, error);
  if (!paths)
    return std::nullopt;
  std::vector<Entry> entries;
  entries.reserve(paths->size());
  for (const auto &path : *paths) {
    CompilerInputFile file;
    if (!file_system.readText(path.path, file, error))
      return std::nullopt;
    if (file.exists && !file.text) {
      error = "compiler build-control input has no readable contents: " + path.path;
      return std::nullopt;
    }
    if (path.required && (!file.exists || !file.text)) {
      error = "required compiler build-control input is missing: " + path.path;
      return std::nullopt;
    }
    const auto fingerprint = entryFingerprint(path.path, path.required, file);
    entries.push_back(Entry(path.path, path.required, file.text, fingerprint));
  }
  const auto fingerprint = snapshotFingerprint(entries, inputs);
  return CompilerBuildControlSnapshot(
      std::move(entries), inputs.resolution_fingerprint,
      inputs.compile_toolchain_fingerprint, inputs.link_toolchain_fingerprint,
      fingerprint);
}

bool CompilerBuildControlSnapshot::verifyCurrentInputs(
    const CompilerInputFileSystem &file_system, CompilerBuildControlInputs inputs,
    std::string &error) const {
  error.clear();
  auto paths = canonicalPaths(inputs, error);
  if (!paths)
    return false;
  if (inputs.resolution_fingerprint != resolution_fingerprint_ ||
      inputs.compile_toolchain_fingerprint != compile_toolchain_fingerprint_ ||
      inputs.link_toolchain_fingerprint != link_toolchain_fingerprint_ ||
      paths->size() != entries_.size()) {
    error = "compiler build-control snapshot conflict: resolved configuration "
            "changed; retry the build";
    return false;
  }
  for (std::size_t index = 0; index < entries_.size(); ++index) {
    const auto &path = (*paths)[index];
    const auto &entry = entries_[index];
    if (path.path != entry.path() || path.required != entry.required()) {
      error = "compiler build-control snapshot conflict: input inventory changed; "
              "retry the build";
      return false;
    }
    CompilerInputFile file;
    if (!file_system.readText(path.path, file, error)) {
      error = "compiler build-control snapshot conflict: " + error;
      return false;
    }
    if (file.exists && !file.text) {
      error = "compiler build-control snapshot conflict: input has no readable "
              "contents for '" +
              path.path + "'";
      return false;
    }
    if (entryFingerprint(path.path, path.required, file) !=
        entry.fingerprint()) {
      error = "compiler build-control snapshot conflict: input changed for '" +
              path.path + "'; retry the build";
      return false;
    }
  }
  return true;
}

CompilerRequestSnapshot::CompilerRequestSnapshot(CompilerBuildControlSnapshot controls,
                                         CompilerSourceSnapshot sources)
    : controls_(std::move(controls)), sources_(std::move(sources)) {
  std::ostringstream canonical;
  canonical << "chtholly.next.request-snapshot.v1\n";
  appendField(canonical, controls_.fingerprint().hex());
  appendField(canonical, sources_.fingerprint().hex());
  fingerprint_ = compiler::StableFingerprint::fromCanonicalBytes(canonical.str());
}

} // namespace chtholly
