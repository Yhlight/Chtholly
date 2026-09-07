#include "SemIRVerificationContext.h"
#include "chtholly/Compiler/BuiltinOperator.h"
#include "chtholly/Compiler/CompilerIntrinsic.h"
#include "chtholly/Compiler/CallableOwnership.h"

#include <array>
#include <cassert>
#include <limits>
#include <ranges>
#include <sstream>
#include <unordered_set>

namespace chtholly::compiler::internal {

bool SemIRVerificationContext::verifyInstructions(std::string &error) const {
  const auto &imports_ = sem_ir_.imports_;
  const auto &insts_ = sem_ir_.insts_;
  const auto &types_ = sem_ir_.types_;
  const auto &inst_blocks_ = sem_ir_.inst_blocks_;
  const auto &type_queries_ = sem_ir_.type_queries_;
  const auto &values_ = sem_ir_.values_;
  const auto bool_type_ = sem_ir_.bool_type_;
  const auto i32_type_ = sem_ir_.i32_type_;
  const auto void_type_ = sem_ir_.void_type_;
  const auto never_type_ = sem_ir_.never_type_;
  const auto string_type_ = sem_ir_.string_type_;
  const auto coroutine_executor_type_ = sem_ir_.coroutine_executor_type_;
  const auto coroutine_scope_type_ = sem_ir_.coroutine_scope_type_;
  const auto coroutine_task_completion_type_ =
      sem_ir_.coroutine_task_completion_type_;
  const auto is_moved_resource_projection = [&](InstId operand) {
    if (!operand.hasValue() || operand.index >= insts_.size())
      return false;
    const auto &projection = inst(operand);
    return projection.kind == SemInstKind::ForeignResourceUnwrap &&
           InstId(projection.arg0).hasValue() &&
           projection.arg0 < insts_.size() &&
           inst(InstId(projection.arg0)).kind == SemInstKind::Move;
  };
  const auto is_resource_place_projection = [&](InstId operand) {
    if (!operand.hasValue() || operand.index >= insts_.size())
      return false;
    const auto &projection = inst(operand);
    if (projection.kind != SemInstKind::ForeignResourceUnwrap ||
        !InstId(projection.arg0).hasValue() || projection.arg0 >= insts_.size())
      return false;
    const auto kind = inst(InstId(projection.arg0)).kind;
    return kind == SemInstKind::NameRef ||
           kind == SemInstKind::StructFieldAccess ||
           kind == SemInstKind::UnionFieldAccess ||
           kind == SemInstKind::Index || kind == SemInstKind::Dereference;
  };
  const auto contains_raw_pointer_dereference = [&](auto &&self,
                                                    InstId operand) -> bool {
    if (!operand.hasValue() || operand.index >= insts_.size())
      return false;
    const auto &projection = inst(operand);
    if (projection.kind == SemInstKind::Dereference) {
      const auto source = InstId(projection.arg0);
      if (!source.hasValue() || source.index >= insts_.size())
        return false;
      if (type(TypeId(inst(source).type)).kind == SemTypeKind::RawPointer)
        return true;
      return self(self, source);
    }
    if (projection.kind == SemInstKind::StructFieldAccess ||
        projection.kind == SemInstKind::UnionFieldAccess ||
        projection.kind == SemInstKind::Index)
      return self(self, InstId(projection.arg0));
    return false;
  };
  const auto constant_enum_discriminant =
      [&](auto &&self, InstId instruction) -> std::optional<std::uint8_t> {
    if (!instruction.hasValue() || instruction.index >= insts_.size())
      return std::nullopt;
    const auto &source = inst(instruction);
    std::int64_t discriminant = -1;
    if (source.kind == SemInstKind::ConstantRef) {
      const auto &entity = constantEntity(ConstantEntityId(source.arg0));
      if (!entity.result.isConcrete())
        return std::nullopt;
      const auto &constant = constantValue(entity.result.value);
      if (constant.kind != ConstantValueKind::Enum ||
          constant.payload >= integerCount())
        return std::nullopt;
      discriminant =
          integer(IntegerId(static_cast<std::uint32_t>(constant.payload)));
    } else if (source.kind == SemInstKind::EnumInit) {
      discriminant = integer(IntegerId(source.arg1));
    } else if (source.kind == SemInstKind::Move ||
               source.kind == SemInstKind::Copy) {
      return self(self, InstId(source.arg0));
    } else if (source.kind == SemInstKind::MaterializeTemporary) {
      return self(self, InstId(source.arg1));
    } else {
      return std::nullopt;
    }
    return discriminant >= 0 && discriminant <= 4
               ? std::optional(static_cast<std::uint8_t>(discriminant))
               : std::nullopt;
  };
  for (std::size_t index = 0; index < insts_.size(); ++index) {
    const auto &value = inst(InstId(static_cast<std::uint32_t>(index)));
    if (value.kind >= SemInstKind::Count) {
      error = "instruction has an invalid kind";
      return false;
    }
    if (value.type >= types_.size()) {
      error = "instruction has an invalid type";
      return false;
    }
    if (!containsArg(semInstArgKind(value.kind, 0), value.arg0) ||
        !containsArg(semInstArgKind(value.kind, 1), value.arg1)) {
      error = "instruction has an invalid typed argument";
      return false;
    }
    const auto instruction_type = TypeId(value.type);
    const auto first_inst_type = [&]() {
      return TypeId(inst(InstId(value.arg0)).type);
    };
    switch (value.kind) {
#include "SemIRVerifyInstructionsLiterals.inc"
#include "SemIRVerifyInstructionsInterop.inc"
#include "SemIRVerifyInstructionsCoroutine.inc"
#include "SemIRVerifyInstructionsControl.inc"
#include "SemIRVerifyInstructionsTypedChannel.inc"
    case SemInstKind::Count:
      error = "instruction has the sentinel kind";
      return false;
    }
  }

  return true;
}

} // namespace chtholly::compiler::internal
