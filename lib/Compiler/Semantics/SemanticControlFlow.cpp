#include "SemanticControlFlow.h"

#include <algorithm>
#include <cstdint>
#include <ranges>

namespace chtholly::compiler::semantics_internal {
namespace {

bool conditionIsAlwaysTrue(const SemIR &sem_ir, InstBlockId block) {
  const auto instructions = sem_ir.instBlock(block);
  if (instructions.empty())
    return false;
  const auto &last = sem_ir.inst(instructions.back());
  return last.kind == SemInstKind::BoolLiteral &&
         sem_ir.integer(IntegerId(last.arg0)) != 0;
}

bool blockContainsCurrentLoopBreak(const SemIR &sem_ir,
                                   std::span<const InstId> block,
                                   std::uint32_t nested_loops = 0) {
  for (const auto id : block) {
    const auto &inst = sem_ir.inst(id);
    if (inst.kind == SemInstKind::Break &&
        sem_ir.integer(IntegerId(inst.arg0)) == nested_loops)
      return true;
    if (inst.kind == SemInstKind::If) {
      if (std::ranges::any_of(
              sem_ir.instBlock(InstBlockId(inst.arg1)), [&](InstId arm) {
                return blockContainsCurrentLoopBreak(
                    sem_ir,
                    sem_ir.instBlock(InstBlockId(sem_ir.inst(arm).arg0)),
                    nested_loops);
              }))
        return true;
    } else if (inst.kind == SemInstKind::Switch) {
      if (std::ranges::any_of(
              sem_ir.instBlock(InstBlockId(inst.arg1)), [&](InstId arm) {
                return blockContainsCurrentLoopBreak(
                    sem_ir,
                    sem_ir.instBlock(InstBlockId(sem_ir.inst(arm).arg1)),
                    nested_loops);
              }))
        return true;
    } else if (inst.kind == SemInstKind::While ||
               inst.kind == SemInstKind::For ||
               inst.kind == SemInstKind::DoWhile) {
      if (blockContainsCurrentLoopBreak(
              sem_ir, sem_ir.instBlock(InstBlockId(inst.arg1)),
              nested_loops + 1))
        return true;
    } else if (inst.kind == SemInstKind::CoroutineTaskScope &&
               blockContainsCurrentLoopBreak(
                   sem_ir, sem_ir.instBlock(InstBlockId(inst.arg0)),
                   nested_loops)) {
      return true;
    }
  }
  return false;
}

} // namespace

bool blockFallsThrough(const SemIR &sem_ir, std::span<const InstId> block) {
  for (const auto id : block) {
    const auto &inst = sem_ir.inst(id);
    if (inst.kind == SemInstKind::Return ||
        inst.kind == SemInstKind::UnrecoverableFailure ||
        inst.kind == SemInstKind::Break || inst.kind == SemInstKind::Continue ||
        expressionCategory(sem_ir, id) == SemExprCategory::Diverging)
      return false;
    if (inst.kind == SemInstKind::If) {
      const auto arms = sem_ir.instBlock(InstBlockId(inst.arg1));
      if (arms.size() == 2 && std::ranges::none_of(arms, [&](InstId arm) {
            return blockFallsThrough(
                sem_ir, sem_ir.instBlock(InstBlockId(sem_ir.inst(arm).arg0)));
          }))
        return false;
    }
    if (inst.kind == SemInstKind::Switch) {
      const auto arms = sem_ir.instBlock(InstBlockId(inst.arg1));
      if (!arms.empty() && std::ranges::none_of(arms, [&](InstId arm) {
            return blockFallsThrough(
                sem_ir, sem_ir.instBlock(InstBlockId(sem_ir.inst(arm).arg1)));
          }))
        return false;
    }
    if (inst.kind == SemInstKind::CoroutineTaskScope &&
        !blockFallsThrough(sem_ir, sem_ir.instBlock(InstBlockId(inst.arg0))))
      return false;
    if (inst.kind == SemInstKind::While &&
        conditionIsAlwaysTrue(sem_ir, InstBlockId(inst.arg0)) &&
        !blockContainsCurrentLoopBreak(
            sem_ir, sem_ir.instBlock(InstBlockId(inst.arg1))))
      return false;
    if (inst.kind == SemInstKind::For) {
      const auto clauses = sem_ir.instBlock(InstBlockId(inst.arg0));
      if (clauses.size() == 3 &&
          conditionIsAlwaysTrue(sem_ir,
                                InstBlockId(sem_ir.inst(clauses[1]).arg1)) &&
          !blockContainsCurrentLoopBreak(
              sem_ir, sem_ir.instBlock(InstBlockId(inst.arg1))))
        return false;
    }
    if (inst.kind == SemInstKind::DoWhile &&
        conditionIsAlwaysTrue(sem_ir, InstBlockId(inst.arg0)) &&
        !blockContainsCurrentLoopBreak(
            sem_ir, sem_ir.instBlock(InstBlockId(inst.arg1))))
      return false;
  }
  return true;
}

} // namespace chtholly::compiler::semantics_internal
