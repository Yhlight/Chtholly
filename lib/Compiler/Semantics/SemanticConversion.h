#pragma once

#include "SemanticLiteral.h"
#include "chtholly/Compiler/SemIR.h"

#include <cstdint>
#include <unordered_map>

namespace chtholly::compiler::semantics_internal {

enum class SemanticConversionKind : std::uint8_t {
  Invalid,
  Identity,
  Never,
  ContextualInteger,
  ContextualIntegerToFloat,
  ContextualFloat,
  ContextualNull,
  Numeric,
  ReferenceToValue,
  ValueToReference,
  ReferenceAuthority,
  PointerAuthority,
};

struct SemanticConversionPlan {
  SemanticConversionKind kind = SemanticConversionKind::Invalid;
  std::uint8_t rank = 0xff;
  // Number of transparent reference layers traversed by this conversion.
  // This is deliberately part of the semantic plan rather than inferred by
  // lowering so generic calls and overload ranking observe the same rules.
  std::uint8_t reference_depth = 0;

  [[nodiscard]] bool valid() const {
    return kind != SemanticConversionKind::Invalid;
  }
};

struct SemanticIntegerLiteral {
  std::uint64_t magnitude = 0;
  bool negative = false;
};

struct SemanticFloatLiteral {
  double value = 0;
};

class SemanticConversionPlanner {
public:
  SemanticConversionPlanner(SemIR &sem_ir, std::uint32_t pointer_width)
      : sem_ir_(sem_ir), pointer_width_(pointer_width) {}

  [[nodiscard]] TypeId typeForSuffix(NumericSuffix suffix);
  [[nodiscard]] bool integerMagnitudeFits(std::uint64_t magnitude,
                                          TypeId target,
                                          bool negative = false) const;
  [[nodiscard]] static bool
  integerMagnitudeExactlyRepresentableAsFloat(std::uint64_t magnitude,
                                              std::uint32_t width);
  [[nodiscard]] bool losslessNumericConversion(TypeId source,
                                               TypeId target) const;
  [[nodiscard]] SemanticConversionPlan query(InstId value, TypeId target) const;

  void recordInteger(InstId value, SemanticIntegerLiteral literal) {
    integer_literals_.insert_or_assign(value.index, literal);
  }
  void recordFloat(InstId value, SemanticFloatLiteral literal) {
    float_literals_.insert_or_assign(value.index, literal);
  }
  [[nodiscard]] const SemanticIntegerLiteral *
  integerLiteral(InstId value) const;
  [[nodiscard]] const SemanticFloatLiteral *floatLiteral(InstId value) const;
  [[nodiscard]] bool isContextualLiteral(InstId value) const;

private:
  SemIR &sem_ir_;
  std::uint32_t pointer_width_ = 64;
  std::unordered_map<std::uint32_t, SemanticIntegerLiteral> integer_literals_;
  std::unordered_map<std::uint32_t, SemanticFloatLiteral> float_literals_;
};

} // namespace chtholly::compiler::semantics_internal
