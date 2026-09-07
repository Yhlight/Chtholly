#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly::core {

struct MemoryMetric {
  std::string label;
  std::uint64_t used_bytes = 0;
  std::uint64_t reserved_bytes = 0;
};

class CompilerMetrics {
public:
  void addMemory(std::string label, std::uint64_t used_bytes,
                 std::uint64_t reserved_bytes);

  [[nodiscard]] const std::vector<MemoryMetric> &memory() const {
    return memory_;
  }

  [[nodiscard]] std::uint64_t totalUsedBytes() const;
  [[nodiscard]] std::uint64_t totalReservedBytes() const;
  [[nodiscard]] std::string toJson(std::string_view unit_name = {}) const;

  static std::string childLabel(std::string_view parent,
                                std::string_view child);

private:
  std::vector<MemoryMetric> memory_;
};

} // namespace chtholly::core
