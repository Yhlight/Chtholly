#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace chtholly::compiler {

// A deterministic dense worklist for compiler-owned IDs.
//
// The queue suppresses duplicate IDs while they are pending, preserves push
// order, and keeps accounting in the queue rather than in each semantic pass.
// This is intentionally small: callers own the fixed point policy and decide
// what a processed item means, while the queue owns ordering and metrics.
enum class DenseWorklistOrder { FIFO, LIFO };

template <typename IdT> class DenseWorklist {
public:
  explicit DenseWorklist(std::size_t id_count,
                         DenseWorklistOrder order = DenseWorklistOrder::FIFO)
      : queued_(id_count), order_(order) {}

  void push(IdT id) {
    if (!id.hasValue() || id.index >= queued_.size() || queued_[id.index])
      return;
    queued_[id.index] = true;
    items_.push_back(id);
    peak_pending_count_ = std::max(peak_pending_count_, pendingCount());
  }

  [[nodiscard]] std::optional<IdT> pop() {
    if (head_ == items_.size())
      return std::nullopt;
    const auto index = order_ == DenseWorklistOrder::FIFO
                           ? head_++
                           : items_.size() - 1;
    const auto id = items_[index];
    if (order_ == DenseWorklistOrder::LIFO)
      items_.pop_back();
    queued_[id.index] = false;
    ++processed_count_;
    return id;
  }

  [[nodiscard]] std::size_t pendingCount() const {
    return items_.size() - head_;
  }

  [[nodiscard]] std::size_t processedCount() const { return processed_count_; }

  [[nodiscard]] std::size_t peakPendingCount() const {
    return peak_pending_count_;
  }

private:
  std::vector<IdT> items_;
  std::vector<bool> queued_;
  std::size_t head_ = 0;
  std::size_t processed_count_ = 0;
  std::size_t peak_pending_count_ = 0;
  DenseWorklistOrder order_ = DenseWorklistOrder::FIFO;
};

} // namespace chtholly::compiler
