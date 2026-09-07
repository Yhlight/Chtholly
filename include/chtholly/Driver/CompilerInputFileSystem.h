#pragma once

#include "chtholly/Compiler/PublicInterface.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chtholly {

struct CompilerInputFile {
  bool exists = false;
  std::shared_ptr<const std::string> text;
};

class CompilerInputFileSystem {
public:
  virtual ~CompilerInputFileSystem() = default;

  [[nodiscard]] virtual bool readText(std::string_view path,
                                      CompilerInputFile &file,
                                      std::string &error) const = 0;
  [[nodiscard]] virtual bool enumerateSources(std::string_view root,
                                              std::vector<std::string> &paths,
                                              std::string &error) const = 0;
};

enum class CompilerOverlayChangeKind : std::uint8_t {
  Replace,
  Tombstone,
  RemoveOverride,
};

struct CompilerOverlayChange {
  std::string path;
  CompilerOverlayChangeKind kind = CompilerOverlayChangeKind::Replace;
  std::optional<std::string> text;

  [[nodiscard]] static CompilerOverlayChange replace(std::string path,
                                                 std::string text) {
    return {.path = std::move(path),
            .kind = CompilerOverlayChangeKind::Replace,
            .text = std::move(text)};
  }
  [[nodiscard]] static CompilerOverlayChange tombstone(std::string path) {
    return {.path = std::move(path), .kind = CompilerOverlayChangeKind::Tombstone};
  }
  [[nodiscard]] static CompilerOverlayChange removeOverride(std::string path) {
    return {.path = std::move(path),
            .kind = CompilerOverlayChangeKind::RemoveOverride};
  }
};

class CompilerOverlayInputFileSystem final : public CompilerInputFileSystem {
public:
  [[nodiscard]] static std::shared_ptr<const CompilerOverlayInputFileSystem>
  create(std::shared_ptr<const CompilerInputFileSystem> base,
         std::span<const CompilerOverlayChange> changes, std::string &error);

  [[nodiscard]] std::shared_ptr<const CompilerOverlayInputFileSystem>
  withChanges(std::span<const CompilerOverlayChange> changes,
              std::string &error) const;

  [[nodiscard]] bool readText(std::string_view path, CompilerInputFile &file,
                              std::string &error) const override;
  [[nodiscard]] bool enumerateSources(std::string_view root,
                                      std::vector<std::string> &paths,
                                      std::string &error) const override;
  [[nodiscard]] const compiler::StableFingerprint &fingerprint() const {
    return fingerprint_;
  }

private:
  struct Entry {
    std::string path;
    std::shared_ptr<const std::string> text;
    bool exists = false;
  };

  [[nodiscard]] static std::optional<std::vector<Entry>>
  applyChanges(std::span<const Entry> current,
               std::span<const CompilerOverlayChange> changes, std::string &error);
  [[nodiscard]] static compiler::StableFingerprint
  fingerprintEntries(std::span<const Entry> entries);

  CompilerOverlayInputFileSystem(std::shared_ptr<const CompilerInputFileSystem> base,
                             std::vector<Entry> entries,
                             compiler::StableFingerprint fingerprint)
      : base_(std::move(base)), entries_(std::move(entries)),
        fingerprint_(fingerprint) {}

  std::shared_ptr<const CompilerInputFileSystem> base_;
  std::vector<Entry> entries_;
  compiler::StableFingerprint fingerprint_;
};

[[nodiscard]] std::shared_ptr<const CompilerInputFileSystem>
makeCompilerRealInputFileSystem();
[[nodiscard]] std::string normalizeCompilerInputPath(std::string_view path);

} // namespace chtholly
