#pragma once

#include "chtholly/Core/Id.h"
#include "chtholly/Core/Metrics.h"
#include "chtholly/Core/ValueStore.h"
#include "chtholly/Compiler/Diagnostics.h"
#include "chtholly/Compiler/TokenBuffer.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly::compiler {

enum class NodeKind : std::uint8_t {
#define CHTHOLLY_COMPILER_NODE(Name) Name,
#include "chtholly/Compiler/NodeKind.def"
  Count,
};

struct NodeId : core::IndexBase<NodeId> {
  using IndexBase::IndexBase;
};

enum class NodeFlags : std::uint8_t {
  None = 0,
  HasError = 1U << 0U,
  IsPublic = 1U << 1U,
  IsExport = 1U << 2U,
  IsTraitImpl = 1U << 3U,
  IsBodyless = 1U << 4U,
  IsForeign = 1U << 5U,
  IsUnsafe = 1U << 6U,
  IsConst = 1U << 7U,
};

[[nodiscard]] constexpr NodeFlags operator|(NodeFlags lhs, NodeFlags rhs) {
  return static_cast<NodeFlags>(static_cast<std::uint8_t>(lhs) |
                                static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr bool hasNodeFlag(NodeFlags flags, NodeFlags flag) {
  return (static_cast<std::uint8_t>(flags) & static_cast<std::uint8_t>(flag)) !=
         0;
}

struct ParseNode {
  std::uint32_t token = core::AnyId::InvalidIndex;
  std::uint16_t subtree_size = 1;
  NodeKind kind = NodeKind::Error;
  NodeFlags flags = NodeFlags::None;
};

class ParseTree {
public:
  explicit ParseTree(const TokenBuffer &tokens) : tokens_(&tokens) {}

  [[nodiscard]] NodeId appendLeaf(NodeKind kind, TokenId token,
                                  bool has_error = false);
  [[nodiscard]] NodeId appendSubtree(NodeKind kind, TokenId token,
                                     std::size_t subtree_start,
                                     bool has_error = false,
                                     NodeFlags flags = NodeFlags::None);
  [[nodiscard]] const ParseNode &get(NodeId id) const {
    return nodes_.get(id);
  }
  [[nodiscard]] NodeKind kind(NodeId id) const {
    return get(id).kind;
  }
  [[nodiscard]] TokenId token(NodeId id) const {
    return TokenId(get(id).token);
  }
  [[nodiscard]] std::size_t size() const {
    return nodes_.size();
  }
  [[nodiscard]] const TokenBuffer &tokens() const {
    return *tokens_;
  }
  [[nodiscard]] std::size_t subtreeStart(NodeId id) const;
  [[nodiscard]] std::vector<NodeId> children(NodeId id) const;
  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] std::string print() const;

  void collectMetrics(core::CompilerMetrics &metrics,
                      std::string_view label) const;

private:
  const TokenBuffer *tokens_;
  core::ValueStore<NodeId, ParseNode> nodes_;
};

[[nodiscard]] std::string_view nodeKindName(NodeKind kind);

static_assert(static_cast<unsigned>(NodeKind::Count) <= 255);
static_assert(sizeof(ParseNode) == 8);

} // namespace chtholly::compiler
