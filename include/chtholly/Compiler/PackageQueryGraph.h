#pragma once

#include "chtholly/Core/Id.h"

#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chtholly::compiler {

struct PackageQueryId : core::IndexBase<PackageQueryId> {
  using IndexBase::IndexBase;
};

enum class PackageQueryState : std::uint8_t {
  Pending,
  Ready,
  Running,
  Succeeded,
  Failed,
  Blocked,
};

struct PackageQueryNode {
  PackageQueryNode(std::string package_name,
                   std::vector<PackageQueryId> dependencies = {})
      : package_name(std::move(package_name)),
        dependencies(std::move(dependencies)) {}

  std::string package_name;
  std::vector<PackageQueryId> dependencies;

  [[nodiscard]] PackageQueryState state() const {
    return state_;
  }
  [[nodiscard]] std::span<const PackageQueryId> dependents() const {
    return dependents_;
  }
  [[nodiscard]] std::size_t remainingDependencies() const {
    return remaining_dependencies_;
  }

private:
  std::vector<PackageQueryId> dependents_;
  std::size_t remaining_dependencies_ = 0;
  PackageQueryState state_ = PackageQueryState::Pending;

  friend class CompilerPackageQueryGraph;
};

class CompilerPackageQueryGraph {
public:
  explicit CompilerPackageQueryGraph(std::vector<PackageQueryNode> nodes);

  [[nodiscard]] bool verify(std::string &error) const;
  [[nodiscard]] bool initialize(std::string &error);
  [[nodiscard]] bool hasReadyQuery() const {
    return !ready_.empty();
  }
  [[nodiscard]] std::optional<PackageQueryId> takeReadyQuery();
  [[nodiscard]] bool markSucceeded(PackageQueryId id, std::string &error);
  [[nodiscard]] bool markFailed(PackageQueryId id, std::string &error);
  void blockUnfinished();

  [[nodiscard]] std::size_t size() const {
    return nodes_.size();
  }
  [[nodiscard]] std::size_t completedCount() const {
    return completed_count_;
  }
  [[nodiscard]] std::span<const PackageQueryNode> nodes() const {
    return nodes_;
  }
  [[nodiscard]] const PackageQueryNode &node(PackageQueryId id) const;

private:
  [[nodiscard]] bool validId(PackageQueryId id) const;

  std::vector<PackageQueryNode> nodes_;
  std::set<std::pair<std::string, PackageQueryId>> ready_;
  std::size_t completed_count_ = 0;
  bool initialized_ = false;
};

[[nodiscard]] std::string_view packageQueryStateName(PackageQueryState state);

} // namespace chtholly::compiler
