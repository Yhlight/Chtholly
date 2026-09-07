#pragma once

#include "chtholly/Driver/CompilerSourceSnapshot.h"
#include "chtholly/Compiler/PublicInterface.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chtholly {

class CompilerInputFileSystem;

struct CompilerBuildControlInputs {
  std::vector<std::string> required_files;
  std::vector<std::string> optional_files;
  compiler::StableFingerprint resolution_fingerprint;
  compiler::StableFingerprint compile_toolchain_fingerprint;
  compiler::StableFingerprint link_toolchain_fingerprint;
};

class CompilerBuildControlSnapshot {
public:
  class Entry {
  public:
    [[nodiscard]] std::string_view path() const {
      return path_;
    }
    [[nodiscard]] bool exists() const {
      return static_cast<bool>(text_);
    }
    [[nodiscard]] bool required() const {
      return required_;
    }
    [[nodiscard]] std::string_view text() const {
      return text_ ? std::string_view(*text_) : std::string_view{};
    }
    [[nodiscard]] const compiler::StableFingerprint &fingerprint() const {
      return fingerprint_;
    }

  private:
    Entry(std::string path, bool required,
          std::shared_ptr<const std::string> text,
          compiler::StableFingerprint fingerprint)
        : path_(std::move(path)), required_(required), text_(std::move(text)),
          fingerprint_(fingerprint) {}

    std::string path_;
    bool required_ = false;
    std::shared_ptr<const std::string> text_;
    compiler::StableFingerprint fingerprint_;
    friend class CompilerBuildControlSnapshot;
  };

  CompilerBuildControlSnapshot(CompilerBuildControlSnapshot &&) noexcept = default;
  CompilerBuildControlSnapshot &
  operator=(CompilerBuildControlSnapshot &&) noexcept = default;

  [[nodiscard]] static std::optional<CompilerBuildControlSnapshot>
  capture(const CompilerInputFileSystem &file_system, CompilerBuildControlInputs inputs,
          std::string &error);

  [[nodiscard]] bool verifyCurrentInputs(const CompilerInputFileSystem &file_system,
                                         CompilerBuildControlInputs inputs,
                                         std::string &error) const;
  [[nodiscard]] std::span<const Entry> entries() const {
    return entries_;
  }
  [[nodiscard]] const compiler::StableFingerprint &fingerprint() const {
    return fingerprint_;
  }
  [[nodiscard]] const compiler::StableFingerprint &
  compileToolchainFingerprint() const {
    return compile_toolchain_fingerprint_;
  }

private:
  CompilerBuildControlSnapshot(
      std::vector<Entry> entries,
      compiler::StableFingerprint resolution_fingerprint,
      compiler::StableFingerprint compile_toolchain_fingerprint,
      compiler::StableFingerprint link_toolchain_fingerprint,
      compiler::StableFingerprint fingerprint)
      : entries_(std::move(entries)),
        resolution_fingerprint_(resolution_fingerprint),
        compile_toolchain_fingerprint_(compile_toolchain_fingerprint),
        link_toolchain_fingerprint_(link_toolchain_fingerprint),
        fingerprint_(fingerprint) {}

  std::vector<Entry> entries_;
  compiler::StableFingerprint resolution_fingerprint_;
  compiler::StableFingerprint compile_toolchain_fingerprint_;
  compiler::StableFingerprint link_toolchain_fingerprint_;
  compiler::StableFingerprint fingerprint_;
};

class CompilerRequestSnapshot {
public:
  CompilerRequestSnapshot(CompilerBuildControlSnapshot controls,
                      CompilerSourceSnapshot sources);

  [[nodiscard]] const CompilerBuildControlSnapshot &controls() const {
    return controls_;
  }
  [[nodiscard]] const CompilerSourceSnapshot &sources() const {
    return sources_;
  }
  [[nodiscard]] const compiler::StableFingerprint &fingerprint() const {
    return fingerprint_;
  }

private:
  CompilerBuildControlSnapshot controls_;
  CompilerSourceSnapshot sources_;
  compiler::StableFingerprint fingerprint_;
};

} // namespace chtholly
