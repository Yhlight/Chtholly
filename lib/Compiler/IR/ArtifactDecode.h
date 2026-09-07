#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace chtholly::compiler::internal {

struct ArtifactDecodeLimits {
  std::size_t max_input_bytes = 64U * 1024U * 1024U;
  std::size_t max_nodes = 1'000'000U;
  std::size_t max_string_bytes = 32U * 1024U * 1024U;
  std::size_t max_single_string_bytes = 8U * 1024U * 1024U;
  std::size_t max_recursion_depth = 128U;
};

class ArtifactDecodeContext {
public:
  explicit ArtifactDecodeContext(
      std::size_t input_size,
      ArtifactDecodeLimits limits = ArtifactDecodeLimits{})
      : limits_(limits) {
    if (input_size > limits_.max_input_bytes)
      fail("input-limit: compiler artifact exceeds the decoder input limit");
  }

  [[nodiscard]] bool consumeNodes(std::size_t count) {
    if (failed_)
      return false;
    if (count > limits_.max_nodes - nodes_)
      return fail("node-budget: compiler artifact exceeds the decoder node budget");
    nodes_ += count;
    return true;
  }

  [[nodiscard]] bool consumeString(std::size_t size) {
    if (failed_)
      return false;
    if (size > limits_.max_single_string_bytes)
      return fail(
          "string-budget: compiler artifact string exceeds the per-string limit");
    if (size > limits_.max_string_bytes - string_bytes_)
      return fail(
          "string-budget: compiler artifact exceeds the decoder string budget");
    string_bytes_ += size;
    return consumeNodes(1);
  }

  [[nodiscard]] bool enterRecursion() {
    if (!consumeNodes(1))
      return false;
    if (depth_ >= limits_.max_recursion_depth)
      return fail(
          "recursion-budget: compiler artifact exceeds the decoder recursion budget");
    ++depth_;
    return true;
  }

  void leaveRecursion() {
    if (depth_ != 0)
      --depth_;
  }

  [[nodiscard]] bool failed() const { return failed_; }
  [[nodiscard]] std::string_view error() const { return error_; }

  void preferBudgetError(std::string &error) const {
    if (failed_)
      error.assign(error_);
  }

private:
  bool fail(std::string_view error) {
    if (!failed_) {
      failed_ = true;
      error_.assign(error);
    }
    return false;
  }

  ArtifactDecodeLimits limits_;
  std::size_t nodes_ = 0;
  std::size_t string_bytes_ = 0;
  std::size_t depth_ = 0;
  bool failed_ = false;
  std::string error_;
};

class ArtifactDecodeRecursionScope {
public:
  explicit ArtifactDecodeRecursionScope(ArtifactDecodeContext &context)
      : context_(context), entered_(context_.enterRecursion()) {}
  ~ArtifactDecodeRecursionScope() {
    if (entered_)
      context_.leaveRecursion();
  }

  ArtifactDecodeRecursionScope(const ArtifactDecodeRecursionScope &) = delete;
  ArtifactDecodeRecursionScope &
  operator=(const ArtifactDecodeRecursionScope &) = delete;

  [[nodiscard]] bool entered() const { return entered_; }

private:
  ArtifactDecodeContext &context_;
  bool entered_;
};

class ArtifactDecodeErrorScope {
public:
  ArtifactDecodeErrorScope(ArtifactDecodeContext &context, std::string &error)
      : context_(context), error_(error) {}
  ~ArtifactDecodeErrorScope() { context_.preferBudgetError(error_); }

  ArtifactDecodeErrorScope(const ArtifactDecodeErrorScope &) = delete;
  ArtifactDecodeErrorScope &operator=(const ArtifactDecodeErrorScope &) =
      delete;

private:
  ArtifactDecodeContext &context_;
  std::string &error_;
};

class ArtifactDecodeReader {
public:
  ArtifactDecodeReader(std::string_view input, ArtifactDecodeContext &context)
      : input_(input), context_(context) {}

  [[nodiscard]] bool bytes(std::size_t count, std::string_view &value) {
    if (context_.failed() || count > input_.size() - offset_)
      return false;
    value = input_.substr(offset_, count);
    offset_ += count;
    return true;
  }

  [[nodiscard]] bool magic(std::string_view expected) {
    std::string_view found;
    return bytes(expected.size(), found) && found == expected;
  }

  [[nodiscard]] bool u8(std::uint8_t &value) {
    std::string_view data;
    if (!bytes(1, data))
      return false;
    value = static_cast<std::uint8_t>(data.front());
    return true;
  }

  [[nodiscard]] bool u32(std::uint32_t &value) {
    std::string_view data;
    if (!bytes(4, data))
      return false;
    value = 0;
    for (std::uint32_t index = 0; index < 4; ++index)
      value |=
          static_cast<std::uint32_t>(static_cast<unsigned char>(data[index]))
          << (index * 8U);
    return true;
  }

  [[nodiscard]] bool u64(std::uint64_t &value) {
    std::string_view data;
    if (!bytes(8, data))
      return false;
    value = 0;
    for (std::uint32_t index = 0; index < 8; ++index)
      value |=
          static_cast<std::uint64_t>(static_cast<unsigned char>(data[index]))
          << (index * 8U);
    return true;
  }

  [[nodiscard]] bool string(std::string &value) {
    std::uint32_t size = 0;
    std::string_view data;
    if (!u32(size) || size > remaining() || !context_.consumeString(size) ||
        !bytes(size, data))
      return false;
    value.assign(data);
    return true;
  }

  [[nodiscard]] bool records(std::uint32_t count,
                             std::size_t minimum_record_size) {
    return minimum_record_size != 0 &&
           count <= remaining() / minimum_record_size &&
           context_.consumeNodes(count);
  }

  [[nodiscard]] bool done() const { return offset_ == input_.size(); }
  [[nodiscard]] std::size_t remaining() const {
    return input_.size() - offset_;
  }
  [[nodiscard]] ArtifactDecodeContext &context() { return context_; }

private:
  std::string_view input_;
  ArtifactDecodeContext &context_;
  std::size_t offset_ = 0;
};

} // namespace chtholly::compiler::internal
