#pragma once

#include "chtholly/Compiler/PublicInterface.h"
#include "chtholly/Compiler/Source.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chtholly {

class CompilerInputFileSystem;

class CompilerSourceSnapshot {
public:
  class Entry {
  public:
    [[nodiscard]] std::string_view path() const {
      return path_;
    }
    [[nodiscard]] std::string_view text() const {
      return *text_;
    }
    [[nodiscard]] const compiler::StableFingerprint &fingerprint() const {
      return fingerprint_;
    }

  private:
    Entry(std::string path, std::shared_ptr<const std::string> text,
          compiler::StableFingerprint fingerprint)
        : path_(std::move(path)), text_(std::move(text)),
          fingerprint_(fingerprint) {}

    std::string path_;
    std::shared_ptr<const std::string> text_;
    compiler::StableFingerprint fingerprint_;
    friend class CompilerSourceSnapshot;
  };

  CompilerSourceSnapshot(CompilerSourceSnapshot &&) noexcept = default;
  CompilerSourceSnapshot &operator=(CompilerSourceSnapshot &&) noexcept = default;

  [[nodiscard]] static std::optional<CompilerSourceSnapshot>
  capture(const CompilerInputFileSystem &file_system,
          std::span<const std::string> normalized_paths, std::string &error);

  [[nodiscard]] const Entry *find(std::string_view normalized_path) const;
  [[nodiscard]] compiler::SourceInput sourceInput(const Entry &entry) const;
  [[nodiscard]] std::span<const Entry> entries() const {
    return entries_;
  }
  [[nodiscard]] const compiler::StableFingerprint &fingerprint() const {
    return fingerprint_;
  }
  [[nodiscard]] bool
  verifyCurrentSources(const CompilerInputFileSystem &file_system,
                       std::span<const std::string> normalized_paths,
                       std::string &error) const;

private:
  CompilerSourceSnapshot(std::vector<Entry> entries,
                     compiler::StableFingerprint fingerprint)
      : entries_(std::move(entries)), fingerprint_(fingerprint) {}

  std::vector<Entry> entries_;
  compiler::StableFingerprint fingerprint_;
};

} // namespace chtholly
