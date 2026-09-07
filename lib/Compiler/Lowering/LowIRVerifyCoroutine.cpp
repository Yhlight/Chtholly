#include "LowIRVerificationContext.h"

namespace chtholly::compiler::internal {
bool LowIRVerificationContext::verifyCoroutineInstruction(
    LowInstId id, std::span<const FunctionId> instruction_owners,
    std::string &error) const {
  const auto &value = low_ir_.inst(id);
  const auto *sem_ir_ = low_ir_.sem_ir_;
  const auto index = id.index;
  const auto origin = [&](LowInstId value_id) { return low_ir_.origin(value_id); };
  const auto inst = [&](LowInstId value_id) -> const LowInst & {
    return low_ir_.inst(value_id);
  };
  const auto valueBlock = [&](LowValueBlockId value_id) {
    return low_ir_.valueBlock(value_id);
  };
  const auto completionProviderFor = [&](TypeId type) {
    return low_ir_.completionProviderFor(type);
  };
  const auto &coroutine_task_create_plans_ =
      low_ir_.coroutine_task_create_plans_;
  const auto &coroutine_task_completion_arm_plans_ =
      low_ir_.coroutine_task_completion_arm_plans_;
  const auto &coroutine_task_completion_set_plans_ =
      low_ir_.coroutine_task_completion_set_plans_;
  const auto &coroutine_task_completion_combine_plans_ =
      low_ir_.coroutine_task_completion_combine_plans_;
  const auto &insts_ = low_ir_.insts_;
  const auto instruction_type = TypeId(value.type);
  const auto value_type = [&](std::uint32_t raw) {
    return TypeId(low_ir_.inst(LowInstId(raw)).type);
  };
  switch (value.kind) {
    case LowInstKind::CoroutineCancellationCheck:
      if (instruction_type != sem_ir_->voidType()) {
        error = "low coroutine cancellation check has invalid type";
        return false;
      }
      break;
    case LowInstKind::CoroutineCancellationRequested:
      if (instruction_type != sem_ir_->boolType()) {
        error = "low coroutine cancellation query has invalid type";
        return false;
      }
      break;
    case LowInstKind::CoroutineRuntimeFault: {
      const auto reason = sem_ir_->integer(IntegerId(value.arg0));
      if (instruction_type != sem_ir_->voidType() || reason < 1 || reason > 8) {
        error = "low coroutine runtime fault has invalid facts";
        return false;
      }
      break;
    }
    case LowInstKind::CoroutineReturnSuccess:
    case LowInstKind::CoroutineReturnError: {
      const auto expected_semantic =
          value.kind == LowInstKind::CoroutineReturnSuccess
              ? SemInstKind::CoroutineReturnSuccess
              : SemInstKind::CoroutineReturnError;
      if (instruction_type != sem_ir_->voidType() ||
          !LowInstId(value.arg0).hasValue() ||
          LowInstId(value.arg0).index >= insts_.size() ||
          !origin(id).hasValue() ||
          sem_ir_->inst(origin(id)).kind != expected_semantic) {
        error = "low coroutine payload terminal has invalid facts";
        return false;
      }
      break;
    }
    case LowInstKind::CoroutineReturnCancelled: {
      const auto semantic_kind = origin(id).hasValue()
                                     ? sem_ir_->inst(origin(id)).kind
                                     : SemInstKind::Invalid;
      if (instruction_type != sem_ir_->voidType() ||
          (semantic_kind != SemInstKind::CoroutineReturnCancelled &&
           semantic_kind != SemInstKind::CoroutineCancellationCheck &&
           semantic_kind != SemInstKind::CoroutineSuspend &&
           semantic_kind != SemInstKind::CoroutineTaskScope)) {
        error = "low coroutine cancellation terminal has invalid facts";
        return false;
      }
      break;
    }
    case LowInstKind::CoroutineExecutorSwitch:
      if (instruction_type != sem_ir_->i32Type() ||
          sem_ir_->type(value_type(value.arg0)).kind !=
              SemTypeKind::CoroutineExecutor) {
        error = "low coroutine executor switch has invalid types";
        return false;
      }
      break;
    case LowInstKind::CoroutineTaskCreate: {
      const auto &plan = coroutine_task_create_plans_.get(
          CoroutineTaskCreatePlanId(value.arg0));
      const auto &semantic = sem_ir_->inst(origin(id));
      const auto operands = valueBlock(LowValueBlockId(value.arg1));
      const auto prefix = plan.mode == CoroutineTaskCreateMode::Root ? 1U : 0U;
      const auto expected_semantic_kind =
          plan.mode == CoroutineTaskCreateMode::Root
              ? SemInstKind::CoroutineTaskCreate
              : SemInstKind::CoroutineChildTaskCreate;
      if (!plan.target.hasValue() ||
          plan.target.index >= sem_ir_->functionRefCount() ||
          semantic.kind != expected_semantic_kind ||
          semantic.arg0 != plan.target.index || semantic.type != value.type) {
        error = "low coroutine task creation has an invalid target plan";
        return false;
      }
      const auto &target = sem_ir_->functionRef(plan.target);
      const auto reconstructed_entity =
          target.local_function.hasValue()
              ? sem_ir_->coroutineConstructorEntity(target.local_function)
              : target.public_entity;
      if (target.local_function != plan.scaffold ||
          reconstructed_entity != plan.constructor_entity ||
          plan.constructor_abi_epoch != 1 ||
          (plan.scaffold.hasValue() &&
           plan.scaffold.index >= sem_ir_->functionCount())) {
        error = "low coroutine task creation has a non-canonical constructor";
        return false;
      }
      const auto *constructor =
          sem_ir_->importIRs().tryGetEntity(plan.constructor_entity);
      if (!constructor || constructor->kind != PublicEntityKind::Function ||
          constructor->execution_kind != PublicFunctionExecutionKind::Async ||
          constructor->coroutine_constructor !=
              PublicCoroutineConstructorABI{1, true, true, true, true}) {
        error = "low coroutine task creation has an invalid constructor ABI";
        return false;
      }
      const auto &target_type =
          sem_ir_->type(sem_ir_->functionRef(plan.target).local_type);
      const auto target_parameters =
          sem_ir_->typeBlock(TypeBlockId(target_type.arg0));
      if (!std::ranges::equal(plan.parameter_types, target_parameters)) {
        error =
            "low coroutine task creation disagrees with its target signature";
        return false;
      }
      if (sem_ir_->type(instruction_type).kind !=
              SemTypeKind::CoroutineChecked ||
          sem_ir_->coroutineCheckedPayloadType(instruction_type) !=
              plan.task_type) {
        error = "low coroutine task creation has an invalid task type plan";
        return false;
      }
      if (operands.size() != plan.parameter_types.size() + prefix) {
        error = "low coroutine task creation has an invalid plan arity";
        return false;
      }
      if (prefix != 0 &&
          value_type(operands.front().index) != sem_ir_->coroutineScopeType()) {
        error = "low coroutine task creation has an invalid root scope";
        return false;
      }
      for (std::size_t parameter = 0; parameter < plan.parameter_types.size();
           ++parameter)
        if (value_type(operands[parameter + prefix].index) !=
            plan.parameter_types[parameter]) {
          error = "low coroutine task creation has a mistyped argument";
          return false;
        }
      break;
    }
    case LowInstKind::CoroutineTaskGroupCreate:
      if (instruction_type != sem_ir_->coroutineScopeType() ||
          sem_ir_->inst(origin(id)).kind != SemInstKind::CoroutineTaskScope) {
        error = "low coroutine task-group create has invalid facts";
        return false;
      }
      break;
    case LowInstKind::CoroutineTaskGroupAttach: {
      const auto operands = valueBlock(LowValueBlockId(value.arg0));
      if (operands.size() != 2 ||
          value_type(operands[0].index) != sem_ir_->coroutineScopeType() ||
          value_type(operands[1].index) != instruction_type ||
          sem_ir_->type(instruction_type).kind !=
              SemTypeKind::CoroutineChecked ||
          sem_ir_->type(sem_ir_->coroutineCheckedPayloadType(instruction_type))
                  .kind != SemTypeKind::CoroutineTask ||
          sem_ir_->inst(origin(id)).kind !=
              SemInstKind::CoroutineChildTaskCreate) {
        error = "low coroutine task-group attach has invalid facts";
        return false;
      }
      break;
    }
    case LowInstKind::CoroutineTaskGroupRequestCancel:
    case LowInstKind::CoroutineTaskGroupClose:
      if (instruction_type != sem_ir_->voidType() ||
          value_type(value.arg0) != sem_ir_->coroutineScopeType()) {
        error = "low coroutine task-group terminal operation has invalid facts";
        return false;
      }
      break;
    case LowInstKind::CoroutineTaskGroupCompletionArm:
      if (instruction_type != sem_ir_->coroutineTaskCompletionType() ||
          value_type(value.arg0) != sem_ir_->coroutineScopeType()) {
        error = "low coroutine task-group completion arm has invalid facts";
        return false;
      }
      break;
    case LowInstKind::CoroutineTaskGroupDrain:
    case LowInstKind::CoroutineTaskGroupErrorDrain:
    case LowInstKind::CoroutineTaskGroupCancelDrain: {
      const auto operands = valueBlock(LowValueBlockId(value.arg0));
      if (instruction_type != sem_ir_->voidType() || operands.size() != 2 ||
          value_type(operands[0].index) !=
              sem_ir_->coroutineTaskCompletionType() ||
          value_type(operands[1].index) != sem_ir_->coroutineScopeType() ||
          sem_ir_->inst(origin(id)).kind != SemInstKind::CoroutineTaskScope) {
        error = "low coroutine task-group drain has invalid facts";
        return false;
      }
      break;
    }
    case LowInstKind::CoroutineTaskCompletionArm: {
      const auto &plan = coroutine_task_completion_arm_plans_.get(
          CoroutineTaskCompletionArmPlanId(value.arg0));
      const auto &semantic = sem_ir_->inst(origin(id));
      if (plan.abi_epoch != 1 || !plan.scaffold.hasValue() ||
          plan.scaffold.index >= sem_ir_->functionCount() ||
          plan.task_type != value_type(value.arg1) ||
          sem_ir_->type(plan.task_type).kind != SemTypeKind::CoroutineTask ||
          plan.completion_type != sem_ir_->coroutineTaskCompletionType() ||
          sem_ir_->type(instruction_type).kind !=
              SemTypeKind::CoroutineChecked ||
          sem_ir_->coroutineCheckedPayloadType(instruction_type) !=
              plan.completion_type ||
          semantic.kind != SemInstKind::CoroutineTaskCompletionArm ||
          semantic.arg0 != origin(LowInstId(value.arg1)).index ||
          semantic.type != value.type) {
        error = "low coroutine completion arm has an invalid plan";
        return false;
      }
      const auto &scaffold = sem_ir_->function(plan.scaffold);
      if ((scaffold.flags &
           (SemFunctionCoroutineScaffold | SemFunctionAsync)) !=
              (SemFunctionCoroutineScaffold | SemFunctionAsync) ||
          (scaffold.flags & (SemFunctionTemplate | SemFunctionSpecific)) != 0) {
        error = "low coroutine completion arm lacks scaffold authority";
        return false;
      }
      break;
    }
    case LowInstKind::CoroutineTaskCompletionReady:
      if (instruction_type != sem_ir_->boolType() ||
          sem_ir_->type(value_type(value.arg0)).kind !=
              SemTypeKind::CoroutineTaskCompletion) {
        error = "low coroutine completion ready test has invalid types";
        return false;
      }
      break;
    case LowInstKind::CoroutineTaskCompletionWait: {
      const auto operands = valueBlock(LowValueBlockId(value.arg0));
      if (instruction_type != sem_ir_->voidType() || operands.size() != 1 ||
          sem_ir_->type(value_type(operands.front().index)).kind !=
              SemTypeKind::CoroutineTaskCompletion) {
        error = "low coroutine completion wait has invalid types";
        return false;
      }
      break;
    }
    case LowInstKind::FinishCoroutineTaskCompletion:
      if (instruction_type != sem_ir_->voidType() ||
          sem_ir_->type(value_type(value.arg0)).kind !=
              SemTypeKind::CoroutineTaskCompletion) {
        error = "low coroutine completion cleanup has invalid types";
        return false;
      }
      break;
    case LowInstKind::CoroutineTaskCompletionSetCreate: {
      const auto &plan = coroutine_task_completion_set_plans_.get(
          CoroutineTaskCompletionSetPlanId(value.arg0));
      const auto operands = valueBlock(LowValueBlockId(value.arg1));
      const auto &semantic = sem_ir_->inst(origin(id));
      const auto expected_words =
          coroutineTaskCompletionBitmapWordCount(plan.operand_count);
      if (plan.abi_epoch != 1 || plan.scaffold != instruction_owners[index] ||
          sem_ir_->type(instruction_type).kind !=
              SemTypeKind::CoroutineChecked ||
          sem_ir_->coroutineCheckedPayloadType(instruction_type) !=
              plan.set_type ||
          sem_ir_->type(plan.set_type).kind !=
              SemTypeKind::CoroutineTaskCompletionSet ||
          plan.provider.completion_type !=
              sem_ir_->completionSetElementType(plan.set_type) ||
          plan.provider != completionProviderFor(plan.set_type) ||
          plan.provider.kind == CompletionProviderKind::Count ||
          (plan.provider.kind == CompletionProviderKind::Task) !=
              (plan.provider.completion_type ==
               sem_ir_->coroutineTaskCompletionType()) ||
          (plan.provider.kind == CompletionProviderKind::Operation &&
           (!plan.provider.resource_owner.hasValue() ||
            !plan.provider.protocol.hasValue() ||
            !plan.provider.wake_plan.hasValue())) ||
          sem_ir_->coroutineTaskCompletionCapacity(plan.set_type) !=
              plan.operand_count ||
          plan.bitmap_word_count != expected_words ||
          plan.ordered_operands.size() != plan.operand_count ||
          operands.size() != plan.operand_count ||
          semantic.kind != SemInstKind::CoroutineTaskCompletionSetCreate ||
          semantic.type != value.type) {
        error = "low coroutine completion-set create has an invalid plan";
        return false;
      }
      const auto semantic_operands =
          sem_ir_->instBlock(InstBlockId(semantic.arg0));
      if (semantic_operands.size() != plan.operand_count) {
        error = "low coroutine completion-set SemIR arity is not canonical";
        return false;
      }
      for (std::size_t operand_index = 0; operand_index < operands.size();
           ++operand_index)
        if (plan.ordered_operands[operand_index] !=
                semantic_operands[operand_index] ||
            origin(operands[operand_index]) !=
                semantic_operands[operand_index] ||
            value_type(operands[operand_index].index) !=
                plan.provider.completion_type) {
          error = "low coroutine completion-set order is not canonical";
          return false;
        }
      break;
    }
    case LowInstKind::CoroutineTaskCompletionCombine: {
      const auto &plan = coroutine_task_completion_combine_plans_.get(
          CoroutineTaskCompletionCombinePlanId(value.arg0));
      const auto &semantic = sem_ir_->inst(origin(id));
      const auto expected_kind =
          semantic.kind == SemInstKind::CoroutineTaskCompletionWaitAll
              ? CoroutineTaskCompletionCombineKind::WaitAll
          : semantic.kind == SemInstKind::CoroutineTaskCompletionSelect
              ? CoroutineTaskCompletionCombineKind::Select
          : semantic.kind == SemInstKind::CoroutineTaskCompletionRace
              ? CoroutineTaskCompletionCombineKind::Race
              : CoroutineTaskCompletionCombineKind::Count;
      const auto expected_winner =
          expected_kind == CoroutineTaskCompletionCombineKind::WaitAll
              ? CoroutineTaskCompletionWinnerPolicy::None
              : CoroutineTaskCompletionWinnerPolicy::LowestCanonicalIndex;
      const auto expected_losers =
          expected_kind == CoroutineTaskCompletionCombineKind::WaitAll
              ? CoroutineTaskCompletionLoserPolicy::ConsumeAll
          : expected_kind == CoroutineTaskCompletionCombineKind::Select
              ? CoroutineTaskCompletionLoserPolicy::TransferRemaining
              : CoroutineTaskCompletionLoserPolicy::ReleaseRemaining;
      if (plan.abi_epoch != 1 || plan.scaffold != instruction_owners[index] ||
          plan.operation != expected_kind ||
          plan.set_type != value_type(value.arg1) ||
          sem_ir_->type(plan.set_type).kind !=
              SemTypeKind::CoroutineTaskCompletionSet ||
          plan.provider.completion_type !=
              sem_ir_->completionSetElementType(plan.set_type) ||
          plan.provider != completionProviderFor(plan.set_type) ||
          plan.provider.kind == CompletionProviderKind::Count ||
          (plan.provider.kind == CompletionProviderKind::Task) !=
              (plan.provider.completion_type ==
               sem_ir_->coroutineTaskCompletionType()) ||
          (plan.provider.kind == CompletionProviderKind::Operation &&
           (!plan.provider.resource_owner.hasValue() ||
            !plan.provider.protocol.hasValue() ||
            !plan.provider.wake_plan.hasValue())) ||
          origin(LowInstId(value.arg1)) != InstId(semantic.arg0) ||
          plan.result_type != instruction_type ||
          plan.operand_count !=
              sem_ir_->coroutineTaskCompletionCapacity(plan.set_type) ||
          plan.bitmap_word_count !=
              coroutineTaskCompletionBitmapWordCount(plan.operand_count) ||
          plan.canonical_operand_order.size() != plan.operand_count ||
          plan.winner_policy != expected_winner ||
          plan.loser_policy != expected_losers ||
          plan.semantic_suspension != origin(id) ||
          (plan.operand_count != 0 && !plan.continuation.hasValue()) ||
          (plan.operand_count == 0 && plan.continuation.hasValue())) {
        error = "low coroutine completion combination has an invalid plan";
        return false;
      }
      for (std::uint32_t operand_index = 0; operand_index < plan.operand_count;
           ++operand_index)
        if (plan.canonical_operand_order[operand_index] != operand_index) {
          error = "low coroutine completion winner order is not canonical";
          return false;
        }
      break;
    }
    case LowInstKind::CoroutineTaskSelectionWinner:
      if (instruction_type != sem_ir_->i32Type() ||
          sem_ir_->type(value_type(value.arg0)).kind !=
              SemTypeKind::CoroutineTaskSelection) {
        error = "low coroutine selection winner has invalid types";
        return false;
      }
      break;
    case LowInstKind::CoroutineTaskSelectionTakeRemaining:
      if (sem_ir_->type(value_type(value.arg0)).kind !=
              SemTypeKind::CoroutineTaskSelection ||
          sem_ir_->type(instruction_type).kind !=
              SemTypeKind::CoroutineTaskCompletionSet ||
          sem_ir_->coroutineTaskCompletionCapacity(value_type(value.arg0)) !=
              sem_ir_->coroutineTaskCompletionCapacity(instruction_type) ||
          sem_ir_->completionSetElementType(value_type(value.arg0)) !=
              sem_ir_->completionSetElementType(instruction_type)) {
        error = "low coroutine selection remaining take has invalid types";
        return false;
      }
      break;
    case LowInstKind::FinishCoroutineTaskCompletionSet:
      if (instruction_type != sem_ir_->voidType() ||
          sem_ir_->type(value_type(value.arg0)).kind !=
              SemTypeKind::CoroutineTaskCompletionSet) {
        error = "low coroutine completion-set cleanup has invalid types";
        return false;
      }
      break;
    case LowInstKind::FinishCoroutineTaskSelection:
      if (instruction_type != sem_ir_->voidType() ||
          sem_ir_->type(value_type(value.arg0)).kind !=
              SemTypeKind::CoroutineTaskSelection) {
        error = "low coroutine selection cleanup has invalid types";
        return false;
      }
      break;
    case LowInstKind::CoroutineTaskJoin:
      if (instruction_type != sem_ir_->i32Type() ||
          sem_ir_->type(value_type(value.arg0)).kind !=
              SemTypeKind::CoroutineTask) {
        error = "low coroutine task join has invalid types";
        return false;
      }
      break;
    case LowInstKind::CoroutineTaskQuery:
    case LowInstKind::CoroutineTaskTakeResult:
    case LowInstKind::CoroutineTaskTakeError:
      if (sem_ir_->type(value_type(value.arg0)).kind !=
              SemTypeKind::CoroutineTask ||
          sem_ir_->type(instruction_type).kind !=
              SemTypeKind::CoroutineChecked) {
        error = "low coroutine task operation has invalid types";
        return false;
      }
      break;
    case LowInstKind::CoroutineCheckedStatus:
      if (instruction_type != sem_ir_->i32Type() ||
          sem_ir_->type(value_type(value.arg0)).kind !=
              SemTypeKind::CoroutineChecked) {
        error = "low coroutine checked status has invalid types";
        return false;
      }
      break;
    case LowInstKind::CoroutineCheckedTake:
      if (sem_ir_->type(value_type(value.arg0)).kind !=
              SemTypeKind::CoroutineChecked ||
          instruction_type !=
              sem_ir_->coroutineCheckedPayloadType(value_type(value.arg0))) {
        error = "low coroutine checked take has invalid types";
        return false;
      }
      break;
    case LowInstKind::CoroutineOutcomeCompleted:
    case LowInstKind::CoroutineOutcomeFailed:
    case LowInstKind::CoroutineOutcomeCancelled:
      if (instruction_type != sem_ir_->boolType() ||
          sem_ir_->type(value_type(value.arg0)).kind !=
              SemTypeKind::CoroutineTaskOutcome) {
        error = "low coroutine outcome predicate has invalid types";
        return false;
      }
      break;
    case LowInstKind::FinishCoroutineTask:
      if (instruction_type != sem_ir_->voidType() ||
          sem_ir_->type(value_type(value.arg0)).kind !=
              SemTypeKind::CoroutineTask) {
        error = "low coroutine task cleanup has invalid types";
        return false;
      }
      break;
    case LowInstKind::FinishCoroutineChecked:
      if (instruction_type != sem_ir_->voidType() ||
          sem_ir_->type(value_type(value.arg0)).kind !=
              SemTypeKind::CoroutineChecked) {
        error = "low coroutine checked cleanup has invalid types";
        return false;
      }
      break;
  default:
    return true;
  }
  return true;
}

} // namespace chtholly::compiler::internal
