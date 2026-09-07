#include "chtholly/Compiler/ParseTree.h"

#include <algorithm>
#include <array>
#include <limits>
#include <sstream>

namespace chtholly::compiler {
namespace {

constexpr auto Names = std::to_array<std::string_view>({
#define CHTHOLLY_COMPILER_NODE(Name) #Name,
#include "chtholly/Compiler/NodeKind.def"
});

} // namespace

NodeId ParseTree::appendLeaf(NodeKind kind, TokenId token, bool has_error) {
  return nodes_.add({token.index, 1, kind,
                     has_error ? NodeFlags::HasError : NodeFlags::None});
}

NodeId ParseTree::appendSubtree(NodeKind kind, TokenId token,
                                std::size_t subtree_start, bool has_error,
                                NodeFlags flags) {
  for (auto index = subtree_start; index < size(); ++index) {
    if (hasNodeFlag(nodes_.get(NodeId(static_cast<std::uint32_t>(index))).flags,
                    NodeFlags::HasError)) {
      has_error = true;
      break;
    }
  }
  const auto subtree_size = size() - subtree_start + 1;
  if (subtree_size > std::numeric_limits<std::uint16_t>::max())
    has_error = true;
  return nodes_.add(
      {token.index,
       static_cast<std::uint16_t>(std::min<std::size_t>(
           subtree_size, std::numeric_limits<std::uint16_t>::max())),
       kind, has_error ? flags | NodeFlags::HasError : flags});
}

std::size_t ParseTree::subtreeStart(NodeId id) const {
  const auto &node = get(id);
  return static_cast<std::size_t>(id.index + 1 - node.subtree_size);
}

std::vector<NodeId> ParseTree::children(NodeId id) const {
  std::vector<NodeId> result;
  const auto start = subtreeStart(id);
  auto cursor = static_cast<std::size_t>(id.index);
  while (cursor > start) {
    const auto child = NodeId(static_cast<std::uint32_t>(cursor - 1));
    result.push_back(child);
    cursor = subtreeStart(child);
  }
  std::reverse(result.begin(), result.end());
  return result;
}

bool ParseTree::verify(std::string &error) const {
  if (nodes_.empty()) {
    error = "parse tree is empty";
    return false;
  }
  for (std::uint32_t index = 0; index < nodes_.size(); ++index) {
    const auto &node = nodes_.get(NodeId(index));
    if (node.token >= tokens_->size()) {
      error = "parse node has an invalid token";
      return false;
    }
    if (node.subtree_size == 0 || node.subtree_size > index + 1) {
      error = "parse node has an invalid subtree size";
      return false;
    }
    if (static_cast<unsigned>(node.kind) >=
        static_cast<unsigned>(NodeKind::Count)) {
      error = "parse node has an invalid kind";
      return false;
    }
  }
  const auto root = NodeId(static_cast<std::uint32_t>(nodes_.size() - 1));
  if (kind(root) != NodeKind::File || get(root).subtree_size != nodes_.size()) {
    error = "parse tree has no complete file root";
    return false;
  }
  return true;
}

std::string ParseTree::print() const {
  std::ostringstream out;
  for (std::uint32_t index = 0; index < nodes_.size(); ++index) {
    const auto id = NodeId(index);
    const auto &node = get(id);
    out << index << ' ' << nodeKindName(node.kind) << " token=" << node.token
        << " subtree=" << node.subtree_size;
    if (hasNodeFlag(node.flags, NodeFlags::HasError))
      out << " error";
    if (hasNodeFlag(node.flags, NodeFlags::IsPublic))
      out << " public";
    if (hasNodeFlag(node.flags, NodeFlags::IsExport))
      out << " export";
    if (hasNodeFlag(node.flags, NodeFlags::IsBodyless))
      out << " bodyless";
    if (hasNodeFlag(node.flags, NodeFlags::IsForeign))
      out << " foreign";
    if (hasNodeFlag(node.flags, NodeFlags::IsUnsafe))
      out << " unsafe";
    if (hasNodeFlag(node.flags, NodeFlags::IsConst))
      out << " const";
    out << '\n';
  }
  return out.str();
}

void ParseTree::collectMetrics(core::CompilerMetrics &metrics,
                               std::string_view label) const {
  nodes_.collectMetrics(metrics,
                        core::CompilerMetrics::childLabel(label, "nodes"));
}

std::string_view nodeKindName(NodeKind kind) {
  const auto index = static_cast<std::size_t>(kind);
  return index < Names.size() ? Names[index] : "InvalidNodeKind";
}

} // namespace chtholly::compiler
