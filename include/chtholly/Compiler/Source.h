#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chtholly {
class CompilerSourceSnapshot;
}

namespace chtholly::compiler {

enum class CompilationUnitKind : std::uint8_t {
  ChthollySource,
  ForeignBinding,
  Count,
};

[[nodiscard]] constexpr std::string_view
compilationUnitKindName(CompilationUnitKind kind) {
  switch (kind) {
  case CompilationUnitKind::ChthollySource:
    return "chtholly-source";
  case CompilationUnitKind::ForeignBinding:
    return "foreign-binding";
  case CompilationUnitKind::Count:
    return "invalid";
  }
  return "invalid";
}

struct LineColumn {
  std::uint32_t line = 1;
  std::uint32_t column = 1;
};

class SourceInput {
public:
  SourceInput(std::string filename, std::string text);

  [[nodiscard]] std::string_view filename() const {
    return filename_;
  }
  [[nodiscard]] std::string_view text() const {
    return *text_;
  }

private:
  SourceInput(std::string filename, std::shared_ptr<const std::string> text)
      : filename_(std::move(filename)), text_(std::move(text)) {}

  std::string filename_;
  std::shared_ptr<const std::string> text_;
  friend class ::chtholly::CompilerSourceSnapshot;
};

class SourceBuffer {
public:
  explicit SourceBuffer(SourceInput input);

  [[nodiscard]] std::string_view filename() const {
    return input_.filename();
  }
  [[nodiscard]] std::string_view text() const {
    return input_.text();
  }
  [[nodiscard]] std::size_t size() const {
    return input_.text().size();
  }
  [[nodiscard]] char at(std::size_t offset) const;
  [[nodiscard]] std::string_view slice(std::size_t offset,
                                       std::size_t length) const;
  [[nodiscard]] LineColumn lineColumn(std::uint32_t offset) const;
  [[nodiscard]] std::string_view lineText(std::uint32_t line) const;

private:
  SourceInput input_;
  std::vector<std::uint32_t> line_starts_;
};

} // namespace chtholly::compiler
