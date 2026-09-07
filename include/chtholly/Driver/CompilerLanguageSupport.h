#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly::compiler {
class CompilationSession;
}

namespace chtholly {

struct CompilerTextPosition {
  std::uint32_t line = 0;
  std::uint32_t character = 0;

  friend bool operator==(const CompilerTextPosition &,
                         const CompilerTextPosition &) = default;
};

struct CompilerTextRange {
  CompilerTextPosition start;
  CompilerTextPosition end;

  friend bool operator==(const CompilerTextRange &,
                         const CompilerTextRange &) = default;
};

struct CompilerTextDocumentContentChange {
  std::optional<CompilerTextRange> range;
  std::optional<std::uint32_t> range_length;
  std::string text;
};

[[nodiscard]] bool nextTextPositionToOffset(std::string_view text,
                                            CompilerTextPosition position,
                                            std::uint32_t &offset,
                                            std::string &error);
[[nodiscard]] bool compilerTextOffsetToPosition(std::string_view text,
                                            std::uint32_t offset,
                                            CompilerTextPosition &position,
                                            std::string &error);
[[nodiscard]] bool
applyNextTextChanges(std::string_view current,
                     std::span<const CompilerTextDocumentContentChange> changes,
                     std::string &next, std::string &error);

struct CompilerSourceLocation {
  std::string path;
  CompilerTextRange range;

  friend bool operator==(const CompilerSourceLocation &,
                         const CompilerSourceLocation &) = default;
};

struct CompilerHoverResult {
  std::string markdown;
  CompilerTextRange range;
};

enum class CompilerCompletionItemKind : std::uint8_t {
  InstanceMethod,
  AssociatedFunction,
  Function,
  Constant,
  Static,
};

struct CompilerCompletionItem {
  std::string label;
  std::string detail;
  CompilerCompletionItemKind kind = CompilerCompletionItemKind::InstanceMethod;

  friend bool operator==(const CompilerCompletionItem &,
                         const CompilerCompletionItem &) = default;
};

enum class CompilerDocumentSymbolKind : std::uint8_t {
  Function,
  Constant,
  Static,
};

struct CompilerDocumentSymbol {
  std::string name;
  CompilerDocumentSymbolKind kind = CompilerDocumentSymbolKind::Function;
  CompilerTextRange range;
  CompilerTextRange selection_range;

  friend bool operator==(const CompilerDocumentSymbol &,
                         const CompilerDocumentSymbol &) = default;
};

struct CompilerRenameResult {
  std::string placeholder;
  CompilerTextRange range;
  std::vector<CompilerSourceLocation> locations;
};

class CompilerWorkspaceSymbolIndex {
public:
  CompilerWorkspaceSymbolIndex(CompilerWorkspaceSymbolIndex &&) noexcept;
  CompilerWorkspaceSymbolIndex &operator=(CompilerWorkspaceSymbolIndex &&) noexcept;
  ~CompilerWorkspaceSymbolIndex();

  [[nodiscard]] static std::shared_ptr<const CompilerWorkspaceSymbolIndex>
  build(std::span<const std::shared_ptr<const compiler::CompilationSession>>
            sessions);

  [[nodiscard]] std::optional<CompilerHoverResult>
  hover(std::string_view path, CompilerTextPosition position) const;
  [[nodiscard]] std::vector<CompilerSourceLocation>
  definition(std::string_view path, CompilerTextPosition position) const;
  [[nodiscard]] std::vector<CompilerSourceLocation>
  references(std::string_view path, CompilerTextPosition position,
             bool include_declaration) const;
  [[nodiscard]] std::vector<CompilerCompletionItem>
  completion(std::string_view path, CompilerTextPosition position,
             std::string_view prefix = {}) const;
  [[nodiscard]] std::vector<CompilerDocumentSymbol>
  documentSymbols(std::string_view path) const;
  [[nodiscard]] std::optional<CompilerRenameResult>
  prepareRename(std::string_view path, CompilerTextPosition position) const;
  [[nodiscard]] std::optional<CompilerRenameResult>
  rename(std::string_view path, CompilerTextPosition position,
         std::string_view new_name, std::string &error) const;

private:
  struct Impl;
  explicit CompilerWorkspaceSymbolIndex(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace chtholly
