#include "chtholly/Compiler/SemIR.h"

#include <algorithm>
#include <ranges>

namespace chtholly::compiler {

bool SemIR::verifyCodeBlock(InstBlockId block, bool require_return,
                            TypeId return_type, std::string &error) const {
  if (!block.hasValue() || block.index >= inst_blocks_.size()) {
    error = "code block has an invalid ID";
    return false;
  }
  const auto contains_current_loop_break =
      [&](auto &&self, InstBlockId candidate,
          std::uint32_t nested_loops) -> bool {
    for (const auto candidate_id : instBlock(candidate)) {
      const auto &candidate_inst = inst(candidate_id);
      if (candidate_inst.kind == SemInstKind::Break &&
          integer(IntegerId(candidate_inst.arg0)) == nested_loops)
        return true;
      if (candidate_inst.kind == SemInstKind::If &&
          std::ranges::any_of(
              instBlock(InstBlockId(candidate_inst.arg1)), [&](InstId arm) {
                return self(self, InstBlockId(inst(arm).arg0), nested_loops);
              }))
        return true;
      if (candidate_inst.kind == SemInstKind::ScopedBlock &&
          self(self, InstBlockId(candidate_inst.arg0), nested_loops))
        return true;
      if (candidate_inst.kind == SemInstKind::Switch &&
          std::ranges::any_of(
              instBlock(InstBlockId(candidate_inst.arg1)), [&](InstId arm) {
                return self(self, InstBlockId(inst(arm).arg1), nested_loops);
              }))
        return true;
      if ((candidate_inst.kind == SemInstKind::While ||
           candidate_inst.kind == SemInstKind::For ||
           candidate_inst.kind == SemInstKind::DoWhile) &&
          self(self, InstBlockId(candidate_inst.arg1), nested_loops + 1))
        return true;
    }
    return false;
  };
  const auto condition_is_true = [&](InstBlockId candidate) {
    const auto instructions = instBlock(candidate);
    if (instructions.empty())
      return false;
    const auto &last = inst(instructions.back());
    return last.kind == SemInstKind::BoolLiteral &&
           integer(IntegerId(last.arg0)) != 0;
  };
  const auto falls_through = [&](auto &&self, InstBlockId candidate) -> bool {
    for (const auto candidate_id : instBlock(candidate)) {
      const auto &candidate_inst = inst(candidate_id);
      if (candidate_inst.kind == SemInstKind::Return ||
          candidate_inst.kind == SemInstKind::UnrecoverableFailure ||
          candidate_inst.kind == SemInstKind::Break ||
          candidate_inst.kind == SemInstKind::Continue ||
          expressionCategory(*this, candidate_id) == SemExprCategory::Diverging)
        return false;
      if (candidate_inst.kind == SemInstKind::If) {
        const auto arms = instBlock(InstBlockId(candidate_inst.arg1));
        if (arms.size() == 2 && std::ranges::none_of(arms, [&](InstId arm) {
              return self(self, InstBlockId(inst(arm).arg0));
            }))
          return false;
      }
      if (candidate_inst.kind == SemInstKind::ScopedBlock &&
          !self(self, InstBlockId(candidate_inst.arg0)))
        return false;
      if (candidate_inst.kind == SemInstKind::Switch) {
        const auto arms = instBlock(InstBlockId(candidate_inst.arg1));
        if (!arms.empty() && std::ranges::none_of(arms, [&](InstId arm) {
              return self(self, InstBlockId(inst(arm).arg1));
            }))
          return false;
      }
      if (candidate_inst.kind == SemInstKind::CoroutineTaskScope &&
          !self(self, InstBlockId(candidate_inst.arg0)))
        return false;
      if (candidate_inst.kind == SemInstKind::While &&
          condition_is_true(InstBlockId(candidate_inst.arg0)) &&
          !contains_current_loop_break(contains_current_loop_break,
                                       InstBlockId(candidate_inst.arg1), 0))
        return false;
      if (candidate_inst.kind == SemInstKind::For) {
        const auto clauses = instBlock(InstBlockId(candidate_inst.arg0));
        if (clauses.size() == 3 &&
            condition_is_true(InstBlockId(inst(clauses[1]).arg1)) &&
            !contains_current_loop_break(contains_current_loop_break,
                                         InstBlockId(candidate_inst.arg1), 0))
          return false;
      }
      if (candidate_inst.kind == SemInstKind::DoWhile &&
          condition_is_true(InstBlockId(candidate_inst.arg0)) &&
          !contains_current_loop_break(contains_current_loop_break,
                                       InstBlockId(candidate_inst.arg1), 0))
        return false;
    }
    return true;
  };
  const auto valid_defer_body = [&](auto &&self, InstBlockId candidate,
                                    std::uint32_t loop_depth) -> bool {
    for (const auto candidate_id : instBlock(candidate)) {
      const auto &candidate_inst = inst(candidate_id);
      if (candidate_inst.kind == SemInstKind::Defer ||
          candidate_inst.kind == SemInstKind::CoroutineTaskScope ||
          candidate_inst.kind == SemInstKind::Return ||
          candidate_inst.kind == SemInstKind::UnrecoverableFailure ||
          candidate_inst.kind == SemInstKind::CoroutineReturnSuccess ||
          candidate_inst.kind == SemInstKind::CoroutineReturnError ||
          candidate_inst.kind == SemInstKind::CoroutineReturnCancelled)
        return false;
      if ((candidate_inst.kind == SemInstKind::Break ||
           candidate_inst.kind == SemInstKind::Continue) &&
          static_cast<std::uint64_t>(integer(IntegerId(candidate_inst.arg0))) >=
              loop_depth)
        return false;
      if (candidate_inst.kind == SemInstKind::If) {
        for (const auto arm : instBlock(InstBlockId(candidate_inst.arg1)))
          if (!self(self, InstBlockId(inst(arm).arg0), loop_depth))
            return false;
      } else if (candidate_inst.kind == SemInstKind::Switch) {
        for (const auto arm : instBlock(InstBlockId(candidate_inst.arg1)))
          if (!self(self, InstBlockId(inst(arm).arg1), loop_depth))
            return false;
      } else if (candidate_inst.kind == SemInstKind::While) {
        if (!self(self, InstBlockId(candidate_inst.arg0), loop_depth) ||
            !self(self, InstBlockId(candidate_inst.arg1), loop_depth + 1))
          return false;
      } else if (candidate_inst.kind == SemInstKind::For) {
        for (const auto clause : instBlock(InstBlockId(candidate_inst.arg0)))
          if (!self(self, InstBlockId(inst(clause).arg1), loop_depth))
            return false;
        if (!self(self, InstBlockId(candidate_inst.arg1), loop_depth + 1))
          return false;
      } else if (candidate_inst.kind == SemInstKind::DoWhile) {
        if (!self(self, InstBlockId(candidate_inst.arg1), loop_depth + 1) ||
            !self(self, InstBlockId(candidate_inst.arg0), loop_depth))
          return false;
      }
    }
    return true;
  };
  bool terminated = false;
  for (const auto id : instBlock(block)) {
    if (id.index >= insts_.size()) {
      error = "code block contains an invalid instruction";
      return false;
    }
    if (terminated) {
      error = "instruction follows a return terminator";
      return false;
    }
    const auto &value = inst(id);
    if (value.kind == SemInstKind::If) {
      for (const auto arm_id : instBlock(InstBlockId(value.arg1))) {
        const auto &arm = inst(arm_id);
        if (arm.kind != SemInstKind::IfArm ||
            !verifyCodeBlock(InstBlockId(arm.arg0), false, return_type, error))
          return false;
      }
    }
    if (value.kind == SemInstKind::ScopedBlock &&
        !verifyCodeBlock(InstBlockId(value.arg0), false, return_type, error))
      return false;
    if (value.kind == SemInstKind::While &&
        (!verifyCodeBlock(InstBlockId(value.arg0), false, return_type, error) ||
         !verifyCodeBlock(InstBlockId(value.arg1), false, return_type, error)))
      return false;
    if (value.kind == SemInstKind::For) {
      for (const auto clause_id : instBlock(InstBlockId(value.arg0))) {
        const auto &clause = inst(clause_id);
        if (clause.kind != SemInstKind::ForClause ||
            !verifyCodeBlock(InstBlockId(clause.arg1), false, return_type,
                             error))
          return false;
      }
      if (!verifyCodeBlock(InstBlockId(value.arg1), false, return_type, error))
        return false;
    }
    if (value.kind == SemInstKind::DoWhile &&
        (!verifyCodeBlock(InstBlockId(value.arg0), false, return_type, error) ||
         !verifyCodeBlock(InstBlockId(value.arg1), false, return_type, error)))
      return false;
    if (value.kind == SemInstKind::Defer) {
      const auto body = InstBlockId(value.arg0);
      if (!verifyCodeBlock(body, false, return_type, error) ||
          !valid_defer_body(valid_defer_body, body, 0) ||
          !falls_through(falls_through, body)) {
        if (error.empty())
          error = "defer body has invalid control flow";
        return false;
      }
    }
    if (value.kind == SemInstKind::CoroutineTaskScope &&
        !verifyCodeBlock(InstBlockId(value.arg0), false, return_type, error))
      return false;
    if (value.kind == SemInstKind::Switch) {
      for (const auto arm_id : instBlock(InstBlockId(value.arg1))) {
        const auto &arm = inst(arm_id);
        if (arm.kind != SemInstKind::SwitchArm ||
            !verifyCodeBlock(InstBlockId(arm.arg1), false, return_type, error))
          return false;
      }
    }
    if (value.kind == SemInstKind::Return &&
        inst(InstId(value.arg0)).type != return_type.index) {
      error = "return operand does not match the function return type";
      return false;
    }
    terminated = value.kind == SemInstKind::Return ||
                 value.kind == SemInstKind::UnrecoverableFailure ||
                 value.kind == SemInstKind::Break ||
                 value.kind == SemInstKind::Continue ||
                 expressionCategory(*this, id) == SemExprCategory::Diverging;
    if (!terminated && value.kind == SemInstKind::If) {
      const auto arms = instBlock(InstBlockId(value.arg1));
      terminated =
          arms.size() == 2 && std::ranges::none_of(arms, [&](InstId arm) {
            return falls_through(falls_through, InstBlockId(inst(arm).arg0));
          });
    }
    if (!terminated && value.kind == SemInstKind::Switch) {
      const auto arms = instBlock(InstBlockId(value.arg1));
      terminated = !arms.empty() && std::ranges::none_of(arms, [&](InstId arm) {
        return falls_through(falls_through, InstBlockId(inst(arm).arg1));
      });
    }
    if (!terminated && value.kind == SemInstKind::CoroutineTaskScope)
      terminated = !falls_through(falls_through, InstBlockId(value.arg0));
  }
  if (require_return && !terminated && falls_through(falls_through, block)) {
    error = "function body has no return terminator";
    return false;
  }
  return true;
}


} // namespace chtholly::compiler
