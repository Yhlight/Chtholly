#pragma once

#include "chtholly/Compiler/SemIR.h"

#include <cstdint>
#include <unordered_map>

namespace chtholly::compiler {

enum class ConstantEvaluationFailure : std::uint8_t {
  None,
  NotConstant,
  InvalidOperation,
  Overflow,
  DivisionByZero,
  RemainderByZero,
  ShiftOutOfRange,
  FatalFailure,
  StepLimit,
  CallDepthLimit,
  Cycle,
};

struct ConstantEvaluationOutcome {
  ConstantEvalResult result;
  ConstantEvaluationFailure failure = ConstantEvaluationFailure::None;
  NodeId location;
};

struct ConstantEvaluationLimits {
  std::uint64_t steps = 1'000'000;
  std::uint32_t call_depth = 128;
};

class ConstantEvaluator {
public:
  explicit ConstantEvaluator(SemIR &sem_ir,
                             ConstantEvaluationLimits limits = {});

  [[nodiscard]] ConstantEvaluationOutcome
  evaluateEntity(ConstantEntityId entity);
  [[nodiscard]] ConstantEvaluationOutcome evaluateExpression(InstId value);

private:
  using Environment = std::unordered_map<std::uint32_t, ConstantId>;

  struct Impl;
  SemIR *sem_ir_;
  ConstantEvaluationLimits limits_;
};

} // namespace chtholly::compiler
