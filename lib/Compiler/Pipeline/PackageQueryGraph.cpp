#include "chtholly/Compiler/PackageQueryGraph.h"

#include <limits>
#include <set>

namespace chtholly::compiler {

CompilerPackageQueryGraph::CompilerPackageQueryGraph(
    std::vector<PackageQueryNode> nodes)
    : nodes_(std::move(nodes)) {}

bool CompilerPackageQueryGraph::validId(PackageQueryId id) const {
  return id.hasValue() && id.index < nodes_.size();
}

bool CompilerPackageQueryGraph::verify(std::string &error) const {
  error.clear();
  if (nodes_.size() > std::numeric_limits<std::uint32_t>::max()) {
    error = "compiler package query graph has too many nodes";
    return false;
  }

  std::set<std::string> package_names;
  std::vector<std::vector<PackageQueryId>> dependents(nodes_.size());
  std::vector<std::size_t> remaining(nodes_.size());
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    const auto &node = nodes_[index];
    const auto id = PackageQueryId(static_cast<std::uint32_t>(index));
    if (node.package_name.empty() ||
        node.package_name.find_first_of("\t\r\n") != std::string::npos ||
        !package_names.insert(node.package_name).second) {
      error = "compiler package query graph has an invalid package identity";
      return false;
    }
    std::set<PackageQueryId> unique_dependencies;
    for (const auto dependency : node.dependencies) {
      if (!validId(dependency) || dependency == id ||
          !unique_dependencies.insert(dependency).second) {
        error = "compiler package query graph contains an invalid dependency";
        return false;
      }
      dependents[dependency.index].push_back(id);
    }
    remaining[index] = node.dependencies.size();
  }

  std::set<std::pair<std::string, PackageQueryId>> ready;
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    if (remaining[index] == 0)
      ready.emplace(nodes_[index].package_name,
                    PackageQueryId(static_cast<std::uint32_t>(index)));
  }
  std::size_t visited = 0;
  while (!ready.empty()) {
    const auto id = ready.begin()->second;
    ready.erase(ready.begin());
    ++visited;
    for (const auto dependent : dependents[id.index]) {
      if (--remaining[dependent.index] == 0)
        ready.emplace(nodes_[dependent.index].package_name, dependent);
    }
  }
  if (visited != nodes_.size()) {
    error = "compiler package query graph contains a dependency cycle";
    return false;
  }
  return true;
}

bool CompilerPackageQueryGraph::initialize(std::string &error) {
  if (initialized_) {
    error = "compiler package query graph is already initialized";
    return false;
  }
  if (!verify(error))
    return false;

  completed_count_ = 0;
  ready_.clear();
  for (auto &node : nodes_) {
    node.dependents_.clear();
    node.remaining_dependencies_ = node.dependencies.size();
    node.state_ = PackageQueryState::Pending;
  }
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    const auto id = PackageQueryId(static_cast<std::uint32_t>(index));
    for (const auto dependency : nodes_[index].dependencies)
      nodes_[dependency.index].dependents_.push_back(id);
  }
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    if (nodes_[index].remaining_dependencies_ == 0) {
      nodes_[index].state_ = PackageQueryState::Ready;
      ready_.emplace(nodes_[index].package_name,
                     PackageQueryId(static_cast<std::uint32_t>(index)));
    }
  }
  initialized_ = true;
  return true;
}

std::optional<PackageQueryId> CompilerPackageQueryGraph::takeReadyQuery() {
  if (!initialized_ || ready_.empty())
    return std::nullopt;
  const auto id = ready_.begin()->second;
  ready_.erase(ready_.begin());
  nodes_[id.index].state_ = PackageQueryState::Running;
  return id;
}

bool CompilerPackageQueryGraph::markSucceeded(PackageQueryId id,
                                          std::string &error) {
  if (!initialized_ || !validId(id) ||
      nodes_[id.index].state_ != PackageQueryState::Running) {
    error = "compiler package query graph rejected an invalid success transition";
    return false;
  }
  nodes_[id.index].state_ = PackageQueryState::Succeeded;
  ++completed_count_;
  for (const auto dependent : nodes_[id.index].dependents_) {
    auto &node = nodes_[dependent.index];
    if (node.state_ != PackageQueryState::Pending ||
        node.remaining_dependencies_ == 0) {
      error = "compiler package query graph has an invalid dependency state";
      return false;
    }
    if (--node.remaining_dependencies_ == 0) {
      node.state_ = PackageQueryState::Ready;
      ready_.emplace(node.package_name, dependent);
    }
  }
  return true;
}

bool CompilerPackageQueryGraph::markFailed(PackageQueryId id, std::string &error) {
  if (!initialized_ || !validId(id) ||
      nodes_[id.index].state_ != PackageQueryState::Running) {
    error = "compiler package query graph rejected an invalid failure transition";
    return false;
  }
  nodes_[id.index].state_ = PackageQueryState::Failed;
  ++completed_count_;
  return true;
}

void CompilerPackageQueryGraph::blockUnfinished() {
  ready_.clear();
  for (auto &node : nodes_) {
    if (node.state_ == PackageQueryState::Pending ||
        node.state_ == PackageQueryState::Ready)
      node.state_ = PackageQueryState::Blocked;
  }
}

const PackageQueryNode &CompilerPackageQueryGraph::node(PackageQueryId id) const {
  return nodes_.at(id.index);
}

std::string_view packageQueryStateName(PackageQueryState state) {
  switch (state) {
  case PackageQueryState::Pending:
    return "pending";
  case PackageQueryState::Ready:
    return "ready";
  case PackageQueryState::Running:
    return "running";
  case PackageQueryState::Succeeded:
    return "succeeded";
  case PackageQueryState::Failed:
    return "failed";
  case PackageQueryState::Blocked:
    return "blocked";
  }
  return "invalid";
}

} // namespace chtholly::compiler
