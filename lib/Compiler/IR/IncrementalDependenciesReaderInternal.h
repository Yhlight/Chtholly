#pragma once

#include "ArtifactDecodeInternal.h"
#include "chtholly/Compiler/IncrementalDependencies.h"

#include <array>

namespace chtholly::compiler::internal {

class IncrementalDependenciesReader {
public:
  IncrementalDependenciesReader(std::string_view input, internal::ArtifactDecodeContext &context,
              std::string &error)
      : reader_(input, context), context_(context), error_(error) {}
  ~IncrementalDependenciesReader() {
    context_.preferBudgetError(error_);
  }

  [[nodiscard]] bool readBytes(std::size_t size, std::string_view &value) {
    return reader_.bytes(size, value);
  }

  [[nodiscard]] bool readU8(std::uint8_t &value) {
    return reader_.u8(value);
  }

  [[nodiscard]] bool readU32(std::uint32_t &value) {
    return reader_.u32(value);
  }

  [[nodiscard]] bool readString(std::string &value) {
    return reader_.string(value);
  }

  [[nodiscard]] bool readFingerprint(StableFingerprint &value) {
    std::string_view bytes;
    if (!readBytes(StableFingerprint::ByteCount, bytes))
      return false;
    std::array<std::uint8_t, StableFingerprint::ByteCount> result{};
    for (std::size_t index = 0; index < result.size(); ++index)
      result[index] = static_cast<std::uint8_t>(bytes[index]);
    value = StableFingerprint(result);
    return true;
  }

  [[nodiscard]] bool atEnd() const {
    return reader_.done();
  }
  [[nodiscard]] std::size_t remaining() const {
    return reader_.remaining();
  }
  [[nodiscard]] bool readRecords(std::uint32_t count,
                                 std::size_t minimum_record_size) {
    return reader_.records(count, minimum_record_size);
  }
  [[nodiscard]] internal::ArtifactDecodeContext &context() {
    return reader_.context();
  }

private:
  internal::ArtifactDecodeReader reader_;
  internal::ArtifactDecodeContext &context_;
  std::string &error_;

};


} // namespace chtholly::compiler::internal
