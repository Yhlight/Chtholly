#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace chtholly::compiler {

enum class DeferredDefinitionTaskKind : std::uint8_t {
  CheckSkippedDefinition,
  DefineThunk,
  EnterNestedScope,
  LeaveNestedScope,
};

template <typename Payload> struct DeferredDefinitionTask {
  DeferredDefinitionTaskKind kind =
      DeferredDefinitionTaskKind::CheckSkippedDefinition;
  std::uint32_t scope_depth = 0;
  std::optional<Payload> payload;

  [[nodiscard]] bool hasPayload() const {
    return kind == DeferredDefinitionTaskKind::CheckSkippedDefinition ||
           kind == DeferredDefinitionTaskKind::DefineThunk;
  }

  [[nodiscard]] const Payload &definition() const {
    assert(hasPayload() && payload.has_value());
    return *payload;
  }
};

// Keeps deferred semantic work ordered while preserving the nesting context in
// which each definition was suspended. Entries remain available after draining
// so later artifact construction can discover the instantiated definition set.
template <typename Payload> class DeferredDefinitionWorklist {
public:
  using Task = DeferredDefinitionTask<Payload>;

  void pushDefinition(Payload payload,
                      DeferredDefinitionTaskKind kind =
                          DeferredDefinitionTaskKind::CheckSkippedDefinition) {
    assert(kind == DeferredDefinitionTaskKind::CheckSkippedDefinition ||
           kind == DeferredDefinitionTaskKind::DefineThunk);
    entries_.push_back({kind, scope_depth_, std::move(payload)});
    updatePeakPending();
  }

  void pushEnterNestedScope() {
    ++scope_depth_;
    entries_.push_back(
        {DeferredDefinitionTaskKind::EnterNestedScope, scope_depth_, {}});
    updatePeakPending();
  }

  [[nodiscard]] bool pushLeaveNestedScope() {
    if (scope_depth_ == 0)
      return false;
    entries_.push_back(
        {DeferredDefinitionTaskKind::LeaveNestedScope, scope_depth_, {}});
    --scope_depth_;
    updatePeakPending();
    return true;
  }

  [[nodiscard]] std::optional<Task> popNext() {
    if (cursor_ == entries_.size())
      return std::nullopt;
    ++processed_count_;
    return entries_[cursor_++];
  }

  [[nodiscard]] std::span<const Task> entries() const { return entries_; }
  [[nodiscard]] std::size_t pendingCount() const {
    return entries_.size() - cursor_;
  }
  [[nodiscard]] std::uint32_t scopeDepth() const { return scope_depth_; }
  [[nodiscard]] bool scopesBalanced() const { return scope_depth_ == 0; }
  [[nodiscard]] std::size_t processedCount() const { return processed_count_; }
  [[nodiscard]] std::size_t peakPendingCount() const {
    return peak_pending_count_;
  }

private:
  void updatePeakPending() {
    peak_pending_count_ =
        std::max(peak_pending_count_, entries_.size() - cursor_);
  }

  std::vector<Task> entries_;
  std::size_t cursor_ = 0;
  std::uint32_t scope_depth_ = 0;
  std::size_t processed_count_ = 0;
  std::size_t peak_pending_count_ = 0;
};

} // namespace chtholly::compiler
