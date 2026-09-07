#include "SemanticConversion.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace chtholly::compiler::semantics_internal {

TypeId SemanticConversionPlanner::typeForSuffix(NumericSuffix suffix) {
  switch (suffix) {
  case NumericSuffix::I8:
    return sem_ir_.addIntegerType(8, true);
  case NumericSuffix::I16:
    return sem_ir_.addIntegerType(16, true);
  case NumericSuffix::I32:
    return sem_ir_.i32Type();
  case NumericSuffix::I64:
    return sem_ir_.addIntegerType(64, true);
  case NumericSuffix::ISize:
    return sem_ir_.addIntegerType(pointer_width_, true);
  case NumericSuffix::U8:
    return sem_ir_.addIntegerType(8, false);
  case NumericSuffix::U16:
    return sem_ir_.addIntegerType(16, false);
  case NumericSuffix::U32:
    return sem_ir_.addIntegerType(32, false);
  case NumericSuffix::U64:
    return sem_ir_.addIntegerType(64, false);
  case NumericSuffix::USize:
    return sem_ir_.addIntegerType(pointer_width_, false);
  case NumericSuffix::F32:
    return sem_ir_.addFloatType(32);
  case NumericSuffix::F64:
    return sem_ir_.f64Type();
  case NumericSuffix::None:
    return TypeId::invalid();
  }
  return TypeId::invalid();
}

bool SemanticConversionPlanner::integerMagnitudeFits(std::uint64_t magnitude,
                                                     TypeId target,
                                                     bool negative) const {
  const auto &type = sem_ir_.type(target);
  if (type.kind != SemTypeKind::Integer)
    return false;
  if (type.arg1 == 0)
    return !negative &&
           (type.arg0 == 64 || magnitude < (std::uint64_t{1} << type.arg0));
  if (negative)
    return magnitude <= (type.arg0 == 64
                             ? std::uint64_t{1} << 63U
                             : std::uint64_t{1} << (type.arg0 - 1U));
  return magnitude <= (type.arg0 == 64
                           ? static_cast<std::uint64_t>(INT64_MAX)
                           : (std::uint64_t{1} << (type.arg0 - 1U)) - 1U);
}

bool SemanticConversionPlanner::integerMagnitudeExactlyRepresentableAsFloat(
    std::uint64_t magnitude, std::uint32_t width) {
  if (magnitude == 0)
    return true;
  const auto precision = width == 32 ? 24U : 53U;
  const auto significant_bits =
      64U - static_cast<std::uint32_t>(std::countl_zero(magnitude));
  if (significant_bits <= precision)
    return true;
  return static_cast<std::uint32_t>(std::countr_zero(magnitude)) >=
         significant_bits - precision;
}

bool SemanticConversionPlanner::losslessNumericConversion(TypeId source,
                                                          TypeId target) const {
  const auto &from = sem_ir_.type(source);
  const auto &to = sem_ir_.type(target);
  if (from.kind == SemTypeKind::Integer && to.kind == SemTypeKind::Integer) {
    if (from.arg1 == to.arg1)
      return from.arg0 <= to.arg0;
    return from.arg1 == 0 && to.arg1 != 0 && from.arg0 < to.arg0;
  }
  if (from.kind == SemTypeKind::Float && to.kind == SemTypeKind::Float)
    return from.arg0 < to.arg0;
  if (from.kind == SemTypeKind::Integer && to.kind == SemTypeKind::Float) {
    const auto precision = to.arg0 == 32 ? 24U : 53U;
    return from.arg1 != 0 ? from.arg0 - 1U <= precision
                          : from.arg0 <= precision;
  }
  return false;
}

SemanticConversionPlan SemanticConversionPlanner::query(InstId value,
                                                        TypeId target) const {
  const auto actual = TypeId(sem_ir_.inst(value).type);
  if (actual == target)
    return {SemanticConversionKind::Identity, 0};
  if (actual == sem_ir_.neverType())
    return {SemanticConversionKind::Never, 0};
  const auto literal = integer_literals_.find(value.index);
  const auto &target_type = sem_ir_.type(target);
  if (sem_ir_.inst(value).kind == SemInstKind::NullPointer &&
      target_type.kind == SemTypeKind::RawPointer)
    return {SemanticConversionKind::ContextualNull, 1};
  if (literal != integer_literals_.end()) {
    if (target_type.kind == SemTypeKind::Integer &&
        integerMagnitudeFits(literal->second.magnitude, target,
                             literal->second.negative))
      return {SemanticConversionKind::ContextualInteger, 1};
    if (target_type.kind == SemTypeKind::Float &&
        integerMagnitudeExactlyRepresentableAsFloat(literal->second.magnitude,
                                                    target_type.arg0))
      return {SemanticConversionKind::ContextualIntegerToFloat, 1};
  }
  if (const auto found = float_literals_.find(value.index);
      found != float_literals_.end() &&
      target_type.kind == SemTypeKind::Float &&
      (target_type.arg0 == 64 ||
       std::isfinite(static_cast<float>(found->second.value))))
    return {SemanticConversionKind::ContextualFloat, 1};
  const auto &source_type = sem_ir_.type(actual);
  if (losslessNumericConversion(actual, target))
    return {SemanticConversionKind::Numeric, 2};
  if (source_type.kind == SemTypeKind::Reference) {
    // References are transparent aliases in value contexts. Walk the
    // canonical pointee chain here so overload resolution, generic
    // specialization, and lowering all agree on the same conversion depth.
    TypeId pointee = actual;
    std::uint8_t depth = 0;
    while (sem_ir_.type(pointee).kind == SemTypeKind::Reference &&
           depth != std::numeric_limits<std::uint8_t>::max()) {
      pointee = sem_ir_.referencePointee(pointee);
      ++depth;
    }
    if (target_type.kind == SemTypeKind::Reference) {
      TypeId target_pointee = target;
      while (sem_ir_.type(target_pointee).kind == SemTypeKind::Reference)
        target_pointee = sem_ir_.referencePointee(target_pointee);
      if (pointee == target_pointee &&
          sem_ir_.referenceMutability(target) ==
              SemReferenceMutability::ReadOnly)
        return {SemanticConversionKind::ReferenceAuthority, 2, depth};
      if (pointee == target_pointee && depth != 0)
        return {SemanticConversionKind::ReferenceAuthority, 2, depth};
    } else if (pointee == target && depth != 0) {
      return {SemanticConversionKind::ReferenceToValue, 2, depth};
    }
  }
  if (target_type.kind == SemTypeKind::Reference) {
    TypeId target_pointee = target;
    while (sem_ir_.type(target_pointee).kind == SemTypeKind::Reference)
      target_pointee = sem_ir_.referencePointee(target_pointee);
    if (actual == target_pointee)
      return {SemanticConversionKind::ValueToReference, 2, 0};
  }
  if (source_type.kind == SemTypeKind::RawPointer &&
      target_type.kind == SemTypeKind::RawPointer) {
    const auto source_pointee = sem_ir_.rawPointerPointee(actual);
    const auto target_pointee = sem_ir_.rawPointerPointee(target);
    const auto source_const = sem_ir_.rawPointerPointeeConst(actual);
    const auto target_const = sem_ir_.rawPointerPointeeConst(target);
    const auto preserves_authority = !source_const || target_const;
    if (preserves_authority &&
        (source_pointee == target_pointee ||
         sem_ir_.type(target_pointee).kind == SemTypeKind::Void))
      return {SemanticConversionKind::PointerAuthority, 2};
  }
  return {};
}

const SemanticIntegerLiteral *
SemanticConversionPlanner::integerLiteral(InstId value) const {
  const auto found = integer_literals_.find(value.index);
  return found == integer_literals_.end() ? nullptr : &found->second;
}

const SemanticFloatLiteral *
SemanticConversionPlanner::floatLiteral(InstId value) const {
  const auto found = float_literals_.find(value.index);
  return found == float_literals_.end() ? nullptr : &found->second;
}

bool SemanticConversionPlanner::isContextualLiteral(InstId value) const {
  return integer_literals_.contains(value.index) ||
         float_literals_.contains(value.index);
}

} // namespace chtholly::compiler::semantics_internal
