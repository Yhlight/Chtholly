#include "LowIRVerificationContext.h"

#include "chtholly/Compiler/CallableOwnership.h"

#include <array>
#include <ranges>
#include <vector>

namespace chtholly::compiler::internal {

const ForeignAbiSignature *LowIRForeignVerificationService::foreignSignature(
    const SemIR &sem_ir, FunctionRefId target) {
  const auto &reference = sem_ir.functionRef(target);
  if (reference.local_function.hasValue()) {
    const auto &declaration = sem_ir.functionDeclaration(reference.local_function);
    return declaration.foreign_signature ? &*declaration.foreign_signature
                                         : nullptr;
  }
  const auto *entity = sem_ir.importIRs().tryGetEntity(reference.public_entity);
  return entity && entity->foreign_signature ? &*entity->foreign_signature
                                             : nullptr;
}

const interop::ForeignOperationArtifact *
LowIRForeignVerificationService::foreignOperation(const SemIR &sem_ir,
                                                   FunctionRefId target) {
  const auto &reference = sem_ir.functionRef(target);
  if (reference.local_function.hasValue()) {
    const auto &declaration = sem_ir.functionDeclaration(reference.local_function);
    return declaration.interop_artifact
               ? sem_ir.importIRs().interopRegistry().resolve(
                     *declaration.interop_artifact)
               : nullptr;
  }
  const auto *entity = sem_ir.importIRs().tryGetEntity(reference.public_entity);
  return entity && entity->interop_artifact
             ? sem_ir.importIRs().interopRegistry().resolve(
                   *entity->interop_artifact)
             : nullptr;
}

bool LowIRForeignVerificationService::isForeign(const SemIR &sem_ir,
                                                FunctionRefId target) {
  const auto &reference = sem_ir.functionRef(target);
  if (reference.local_function.hasValue())
    return sem_ir.functionDeclaration(reference.local_function).kind ==
           SemCallableDeclarationKind::Foreign;
  const auto *entity = sem_ir.importIRs().tryGetEntity(reference.public_entity);
  return entity &&
         entity->declaration_kind == PublicCallableDeclarationKind::Foreign;
}

bool LowIRVerificationContext::verifyForeignInstruction(
    LowInstId id, std::string &error) const {
  const auto &value = low_ir_.inst(id);
  switch (value.kind) {
  case LowInstKind::ForeignDefaultPromote:
  case LowInstKind::ForeignFunctionRef:
  case LowInstKind::ForeignCall:
  case LowInstKind::CallbackAdapter:
  case LowInstKind::IndirectForeignCall:
  case LowInstKind::CallbackAdapterCall:
  case LowInstKind::MakeCallbackAdapter:
  case LowInstKind::MakeCallbackRegistration:
  case LowInstKind::CallbackRegistrationActive:
  case LowInstKind::FinishCallbackRegistration:
  case LowInstKind::CallbackRegistrationUnregister:
  case LowInstKind::CallbackRegistrationCancel:
  case LowInstKind::CallbackRegistrationCancelAsync:
  case LowInstKind::CallbackCompletionPending:
  case LowInstKind::CallbackCompletionPoll:
  case LowInstKind::FinishCallbackCompletion:
  case LowInstKind::CallbackCompletionWait:
  case LowInstKind::MakeCallbackWake:
  case LowInstKind::ForeignOperationProjectCompletion:
  case LowInstKind::ForeignOperationProjectWake:
  case LowInstKind::ForeignOperationPortProject:
  case LowInstKind::CallbackWakeReady:
  case LowInstKind::FinishCallbackWake:
  case LowInstKind::CallbackWakeWait:
  case LowInstKind::CallbackDetach:
    break;
  default:
    return true;
  }
  const auto *sem_ir = low_ir_.sem_ir_;
  const auto instruction_type = TypeId(value.type);
  const auto value_type = [&](std::uint32_t raw) {
    return TypeId(low_ir_.inst(LowInstId(raw)).type);
  };
  switch (value.kind) {
  case LowInstKind::ForeignDefaultPromote:
    if (instruction_type != low_ir_.cDefaultPromotedType(value_type(value.arg0)) ||
        instruction_type == value_type(value.arg0)) {
      error = "C default promotion has invalid source or result types";
      return false;
    }
    return true;
  case LowInstKind::ForeignFunctionRef: {
    const auto target = FunctionRefId(value.arg0);
    const auto &source = sem_ir->functionRef(target);
    const auto &source_type = sem_ir->type(source.local_type);
    const auto &callback = sem_ir->type(instruction_type);
    const auto *signature =
        LowIRForeignVerificationService::foreignSignature(*sem_ir, target);
    const auto *ownership = callableOwnershipSummary(*sem_ir, target);
    const auto physical_type = [&](TypeId type) {
      const auto representation = sem_ir->foreignRepresentationType(type);
      return representation.hasValue() ? representation : type;
    };
    std::vector<TypeId> physical_parameters;
    if (source_type.kind == SemTypeKind::Function) {
      for (const auto parameter :
           sem_ir->typeBlock(TypeBlockId(source_type.arg0)))
        physical_parameters.push_back(physical_type(parameter));
    }
    auto physical_ownership = ownership ? *ownership : CallableOwnershipSummary{};
    if (source_type.kind == SemTypeKind::Function &&
        physical_type(TypeId(source_type.arg1)) != TypeId(source_type.arg1))
      physical_ownership.returns_owned = true;
    bool valid_source =
        !source.generic.hasValue() &&
        LowIRForeignVerificationService::isForeign(*sem_ir, target);
    if (source.local_function.hasValue()) {
      const auto &declaration =
          sem_ir->functionDeclaration(source.local_function);
      valid_source = valid_source && declaration.is_unsafe &&
                     declaration.foreign_abi.hasValue() &&
                     sem_ir->identifier(declaration.foreign_abi) == "C";
    } else if (const auto *entity =
                   sem_ir->importIRs().tryGetEntity(source.public_entity)) {
      valid_source = valid_source &&
                     entity->kind == PublicEntityKind::Function &&
                     entity->generic_parameter_count == 0 && entity->is_unsafe &&
                     entity->foreign_abi.hasValue() &&
                     sem_ir->identifier(entity->foreign_abi) == "C";
    } else {
      valid_source = false;
    }
    if (!valid_source || !signature || !ownership ||
        source_type.kind != SemTypeKind::Function ||
        (callback.kind != SemTypeKind::CFunctionPointer &&
         callback.kind != SemTypeKind::CVariadicFunctionPointer) ||
        TypeId(callback.arg1) != physical_type(TypeId(source_type.arg1)) ||
        !std::ranges::equal(
            sem_ir->typeBlock(TypeBlockId(callback.arg0)), physical_parameters) ||
        (callback.kind == SemTypeKind::CVariadicFunctionPointer) !=
            (signature && signature->is_variadic) ||
        sem_ir->callbackContextParameter(instruction_type) !=
            core::AnyId::InvalidIndex ||
        sem_ir->callbackContract(instruction_type) != physical_ownership) {
      error = "low foreign function reference disagrees with its declaration";
      return false;
    }
    return true;
  }
  case LowInstKind::ForeignCall: {
    const auto &call_layout = low_ir_.foreignAbiCallLayout(
        ForeignAbiCallLayoutId(value.arg0));
    const auto &foreign_layout =
        low_ir_.foreignAbiLayout(call_layout.function_layout);
    const auto target = foreign_layout.target;
    const auto &function = sem_ir->functionRef(target);
    const auto &function_type = sem_ir->type(function.local_type);
    if (function_type.kind != SemTypeKind::Function) {
      error = "low call target does not have a function type";
      return false;
    }
    if (sem_ir->functionIntrinsicRole(target) != CompilerIntrinsicRole::None) {
      error = "low ordinary call targets a compiler intrinsic";
      return false;
    }
    const auto *imported =
        function.local_function.hasValue()
            ? nullptr
            : sem_ir->importIRs().tryGetEntity(function.public_entity);
    if (!function.local_function.hasValue() && !imported) {
      error = "low call target has no callable declaration";
      return false;
    }
    const auto ordinary =
        function.local_function.hasValue()
            ? (sem_ir->functionSemanticContract(function.local_function).domain ==
                   CallableSemanticDomain::Ordinary ||
               sem_ir->functionSemanticContract(function.local_function).domain ==
                   CallableSemanticDomain::NominalConstruction)
            : imported->semantic_contract.domain ==
                      CallableSemanticDomain::Ordinary ||
                  imported->semantic_contract.domain ==
                      CallableSemanticDomain::NominalConstruction;
    if (!ordinary) {
      error = "low ordinary call targets a representation helper";
      return false;
    }
    const auto foreign =
        function.local_function.hasValue()
            ? sem_ir->functionDeclaration(function.local_function).kind ==
                  SemCallableDeclarationKind::Foreign
            : imported->declaration_kind == PublicCallableDeclarationKind::Foreign;
    if (!foreign) {
      error = "low foreign call targets an ordinary callable";
      return false;
    }
    if (low_ir_.foreignAbiLayoutFor(target) != call_layout.function_layout) {
      error = "low foreign call does not reference its callable ABI layout";
      return false;
    }
    const auto arguments = low_ir_.valueBlock(LowValueBlockId(value.arg1));
    const auto parameters =
        sem_ir->typeBlock(TypeBlockId(function_type.arg0));
    const auto suffix_count = call_layout.suffix.size();
    if (instruction_type.index != function_type.arg1 ||
        arguments.size() != parameters.size() + suffix_count) {
      error = "low call does not match its function signature";
      return false;
    }
    for (std::size_t argument = 0; argument < parameters.size(); ++argument) {
      if (arguments[argument].index >= low_ir_.insts_.size() ||
          low_ir_.inst(arguments[argument]).type != parameters[argument].index) {
        error = "low call argument does not match its parameter";
        return false;
      }
    }
    for (std::size_t suffix = 0; suffix < suffix_count; ++suffix) {
      const auto argument = parameters.size() + suffix;
      if (arguments[argument].index >= low_ir_.insts_.size() ||
          TypeId(low_ir_.inst(arguments[argument]).type) !=
              call_layout.suffix[suffix].semantic_type) {
        error = "low foreign call suffix does not match its promoted layout";
        return false;
      }
    }
    return true;
  }
  case LowInstKind::CallbackAdapter: {
    const auto &plan = low_ir_.foreignAbiThunkPlan(
        ForeignAbiThunkPlanId(value.arg0));
    if (plan.callback_type != instruction_type ||
        low_ir_.foreignAbiLayout(plan.callback_layout).callback_type !=
            instruction_type) {
      error = "low callback adapter does not match its verified thunk plan";
      return false;
    }
    return true;
  }
  case LowInstKind::IndirectForeignCall: {
    const auto &call_layout = low_ir_.foreignAbiCallLayout(
        ForeignAbiCallLayoutId(value.arg0));
    const auto &layout =
        low_ir_.foreignAbiLayout(call_layout.function_layout);
    if (!layout.callback_type.hasValue() || layout.target.hasValue() ||
        instruction_type != layout.result.semantic_type) {
      error = "low indirect foreign call has an invalid callback layout";
      return false;
    }
    const auto operands = low_ir_.valueBlock(LowValueBlockId(value.arg1));
    if (operands.empty() ||
        value_type(operands.front().index) != layout.callback_type ||
        operands.size() != 1 + layout.parameters.size() + call_layout.suffix.size()) {
      error = "low indirect foreign call has invalid operands";
      return false;
    }
    for (std::size_t argument_index = 0;
         argument_index < layout.parameters.size(); ++argument_index) {
      if (value_type(operands[argument_index + 1].index) !=
          layout.parameters[argument_index].semantic_type) {
        error = "low indirect foreign call fixed argument is mistyped";
        return false;
      }
    }
    for (std::size_t suffix_index = 0;
         suffix_index < call_layout.suffix.size(); ++suffix_index) {
      if (value_type(
              operands[1 + layout.parameters.size() + suffix_index].index) !=
          call_layout.suffix[suffix_index].semantic_type) {
        error = "low indirect foreign call suffix is not promoted";
        return false;
      }
    }
    return true;
  }
  case LowInstKind::CallbackAdapterCall: {
    const auto &plan = low_ir_.callbackAdapterPlan(
        CallbackAdapterPlanId(value.arg0));
    const auto &entry_call =
        low_ir_.foreignAbiCallLayout(plan.entry_call_layout);
    const auto &layout =
        low_ir_.foreignAbiLayout(entry_call.function_layout);
    const auto operands = low_ir_.valueBlock(LowValueBlockId(value.arg1));
    if (operands.empty() ||
        value_type(operands.front().index) != plan.adapter_type ||
        instruction_type != layout.result.semantic_type ||
        operands.size() != layout.parameters.size() ||
        plan.context_parameter >= layout.parameters.size()) {
      error = "low callback adapter call disagrees with its verified plan";
      return false;
    }
    for (std::size_t source = 1; source < operands.size(); ++source) {
      const auto parameter =
          source - 1 < plan.context_parameter ? source - 1 : source;
      if (value_type(operands[source].index) !=
          layout.parameters[parameter].semantic_type) {
        error = "low callback adapter call has a mistyped source argument";
        return false;
      }
    }
    return true;
  }
  case LowInstKind::MakeCallbackAdapter: {
    const auto &adapter = sem_ir->type(instruction_type);
    const auto operands = low_ir_.valueBlock(LowValueBlockId(value.arg0));
    if (adapter.kind != SemTypeKind::CallbackAdapter || operands.size() != 3) {
      error = "low callback adapter construction has invalid storage";
      return false;
    }
    const auto fields = sem_ir->typeBlock(TypeBlockId(adapter.arg0));
    for (std::size_t field_index = 0; field_index < operands.size();
         ++field_index) {
      if (value_type(operands[field_index].index) != fields[field_index]) {
        error = "low callback adapter field does not match its ABI type";
        return false;
      }
    }
    return true;
  }
  case LowInstKind::MakeCallbackRegistration: {
    const auto &plan = low_ir_.callbackRegistrationPlan(
        CallbackRegistrationPlanId(value.arg0));
    const auto &registration = sem_ir->type(instruction_type);
    const auto operands = low_ir_.valueBlock(LowValueBlockId(value.arg1));
    if (plan.registration_type != instruction_type ||
        registration.kind != SemTypeKind::CallbackRegistration) {
      error = "low callback registration construction has invalid storage";
      return false;
    }
    const auto bindings = sem_ir->callbackRegistrationBindings(instruction_type);
    const auto fields = sem_ir->typeBlock(TypeBlockId(registration.arg0));
    const auto fixed_count = fields.size() == 5 ? 4U : fields.size() - 1U;
    if (operands.size() != fixed_count + bindings.size() ||
        plan.binding_parameters.size() != bindings.size()) {
      error = "low callback registration construction has invalid storage";
      return false;
    }
    constexpr std::array<std::size_t, 9> field_indices{0, 2, 3, 4, 5,
                                                       6, 7, 8, 9};
    for (std::size_t operand = 0; operand < fixed_count; ++operand) {
      if (value_type(operands[operand].index) != fields[field_indices[operand]]) {
        error = "low callback registration field has the wrong ABI type";
        return false;
      }
    }
    const auto register_parameters = sem_ir->typeBlock(
        TypeBlockId(sem_ir->type(fields[2]).arg0));
    for (std::size_t binding = 0; binding < bindings.size(); ++binding) {
      const auto parameter = plan.binding_parameters[binding];
      if (parameter != bindings[binding].parameter_index ||
          parameter >= register_parameters.size() ||
          value_type(operands[fixed_count + binding].index) !=
              register_parameters[parameter]) {
        error = "low callback registration binding has the wrong ABI type";
        return false;
      }
    }
    return true;
  }
  case LowInstKind::CallbackRegistrationActive:
    if (instruction_type != sem_ir->boolType() ||
        sem_ir->type(value_type(value.arg0)).kind !=
            SemTypeKind::CallbackRegistration) {
      error = "low callback registration active test has invalid types";
      return false;
    }
    return true;
  case LowInstKind::FinishCallbackRegistration:
  case LowInstKind::CallbackRegistrationUnregister:
  case LowInstKind::CallbackRegistrationCancel: {
    const auto &plan = low_ir_.callbackRegistrationPlan(
        CallbackRegistrationPlanId(value.arg0));
    const auto operands = low_ir_.valueBlock(LowValueBlockId(value.arg1));
    if (instruction_type != sem_ir->voidType() || operands.size() != 1 ||
        value_type(operands.front().index) != plan.registration_type) {
      error = "low callback registration terminal operation has invalid types";
      return false;
    }
    return true;
  }
  case LowInstKind::CallbackRegistrationCancelAsync: {
    const auto &plan = low_ir_.callbackRegistrationPlan(
        CallbackRegistrationPlanId(value.arg0));
    const auto operands = low_ir_.valueBlock(LowValueBlockId(value.arg1));
    if (!plan.cancel_async_call_layout.hasValue() ||
        !plan.completion_plan.hasValue() || operands.size() != 1 ||
        value_type(operands.front().index) != plan.registration_type ||
        low_ir_.callbackCompletionPlan(plan.completion_plan).completion_type !=
            instruction_type) {
      error = "low asynchronous callback cancellation has invalid types";
      return false;
    }
    return true;
  }
  case LowInstKind::CallbackCompletionPending:
    if (instruction_type != sem_ir->boolType() ||
        sem_ir->type(value_type(value.arg0)).kind !=
            SemTypeKind::CallbackCompletion) {
      error = "low callback completion pending test has invalid types";
      return false;
    }
    return true;
  case LowInstKind::CallbackCompletionPoll: {
    const auto &plan = low_ir_.callbackReadinessPlan(
        CallbackReadinessPlanId(value.arg0));
    const auto operands = low_ir_.valueBlock(LowValueBlockId(value.arg1));
    if (instruction_type != sem_ir->boolType() || operands.size() != 1 ||
        value_type(operands.front().index) != plan.completion_type ||
        low_ir_.callbackCompletionPlan(plan.completion_plan).completion_type !=
            plan.completion_type) {
      error = "low callback completion poll has invalid types";
      return false;
    }
    return true;
  }
  case LowInstKind::FinishCallbackCompletion:
  case LowInstKind::CallbackCompletionWait: {
    const auto &plan = low_ir_.callbackCompletionPlan(
        CallbackCompletionPlanId(value.arg0));
    const auto operands = low_ir_.valueBlock(LowValueBlockId(value.arg1));
    if (instruction_type != sem_ir->voidType() || operands.size() != 1 ||
        value_type(operands.front().index) != plan.completion_type) {
      error = "low callback completion terminal operation has invalid types";
      return false;
    }
    return true;
  }
  case LowInstKind::MakeCallbackWake: {
    const auto &plan = low_ir_.callbackWakePlan(CallbackWakePlanId(value.arg0));
    const auto operands = low_ir_.valueBlock(LowValueBlockId(value.arg1));
    const auto wake_fields = sem_ir->typeBlock(
        TypeBlockId(sem_ir->type(instruction_type).arg0));
    if (sem_ir->type(instruction_type).kind != SemTypeKind::CallbackWake ||
        wake_fields.size() != 1 || wake_fields[0] != plan.completion_type ||
        operands.size() != 2 ||
        value_type(operands[0].index) != plan.completion_type ||
        sem_ir->type(value_type(operands[1].index)).kind !=
            SemTypeKind::CallbackAdapter) {
      error = "low callback wake construction has invalid types";
      return false;
    }
    return true;
  }
  case LowInstKind::ForeignOperationProjectCompletion: {
    const auto plan_id = ForeignOperationCompletionPlanId(value.arg0);
    const auto operands = low_ir_.valueBlock(LowValueBlockId(value.arg1));
    if (!plan_id.hasValue() ||
        plan_id.index >= low_ir_.foreignOperationCompletionPlanCount() ||
        sem_ir->type(instruction_type).kind !=
            SemTypeKind::CallbackCompletion ||
        operands.size() != 2 ||
        !sem_ir->foreignOperationStateOwner(value_type(operands[0].index)) ||
        sem_ir->foreignOperationStateOwner(value_type(operands[0].index))
                ->state != ForeignOperationStateKind::Subscription ||
        low_ir_.foreignOperationCompletionPlan(plan_id).projection !=
            interop::ForeignCompletionProjectionKind::ScalarToCompletion) {
      error = "low foreign completion projection has invalid facts";
      return false;
    }
    return true;
  }
  case LowInstKind::ForeignOperationProjectWake: {
    const auto plan_id = ForeignOperationCompletionPlanId(value.arg0);
    const auto operands = low_ir_.valueBlock(LowValueBlockId(value.arg1));
    if (!plan_id.hasValue() ||
        plan_id.index >= low_ir_.foreignOperationCompletionPlanCount() ||
        sem_ir->type(instruction_type).kind != SemTypeKind::CallbackWake ||
        operands.size() != 3 ||
        sem_ir->type(value_type(operands[0].index)).kind !=
            SemTypeKind::CallbackCompletion ||
        sem_ir->type(value_type(operands[1].index)).kind !=
            SemTypeKind::CallbackAdapter ||
        (sem_ir->type(value_type(operands[2].index)).kind !=
             SemTypeKind::Bool &&
         sem_ir->type(value_type(operands[2].index)).kind !=
             SemTypeKind::Integer) ||
        low_ir_.foreignOperationCompletionPlan(plan_id).projection !=
            interop::ForeignCompletionProjectionKind::ScalarToWake) {
      error = "low foreign wake projection has invalid facts";
      return false;
    }
    return true;
  }
  case LowInstKind::ForeignOperationPortProject: {
    const auto plan_id = ForeignOperationCompletionPlanId(value.arg0);
    const auto operands = low_ir_.valueBlock(LowValueBlockId(value.arg1));
    if (!plan_id.hasValue() ||
        plan_id.index >= low_ir_.foreignOperationCompletionPlanCount() ||
        (operands.size() != 2 && operands.size() != 5) ||
        low_ir_.inst(operands.back()).kind != LowInstKind::IntegerConstant) {
      error = "low foreign operation port projection has invalid facts";
      return false;
    }
    const auto lane_value =
        sem_ir->integer(IntegerId(low_ir_.inst(operands.back()).arg0));
    if (lane_value < 0) {
      error = "low foreign operation port projection has invalid lane";
      return false;
    }
    const auto lane = static_cast<std::uint32_t>(lane_value);
    const auto &plan = low_ir_.foreignOperationCompletionPlan(plan_id);
    const auto source_kind = sem_ir->type(value_type(operands[0].index)).kind;
    const auto source_owner =
        sem_ir->foreignOperationStateOwner(value_type(operands[0].index));
    const bool callback_lane =
        source_kind == SemTypeKind::CallbackAdapter &&
        std::ranges::find(plan.wake_callback_lanes, lane) !=
            plan.wake_callback_lanes.end();
    const bool carrier_lane =
        (source_kind == SemTypeKind::CallbackCompletion ||
         source_kind == SemTypeKind::CallbackWake ||
         (source_owner &&
          source_owner->state == ForeignOperationStateKind::Subscription)) &&
        plan.carrier_lane == lane;
    const bool subscription_result =
        plan.projection ==
            interop::ForeignCompletionProjectionKind::ScalarToSubscription &&
        plan.result_lane == lane && operands.size() == 5;
    if (!callback_lane && !carrier_lane && !subscription_result) {
      error = "low foreign operation port projection is not in its plan";
      return false;
    }
    return true;
  }
  case LowInstKind::CallbackWakeReady:
    if (instruction_type != sem_ir->boolType() ||
        sem_ir->type(value_type(value.arg0)).kind != SemTypeKind::CallbackWake) {
      error = "low callback wake ready test has invalid types";
      return false;
    }
    return true;
  case LowInstKind::FinishCallbackWake:
  case LowInstKind::CallbackWakeWait:
  case LowInstKind::CallbackDetach: {
    const auto &plan = low_ir_.callbackWakePlan(CallbackWakePlanId(value.arg0));
    const auto operands = low_ir_.valueBlock(LowValueBlockId(value.arg1));
    if (instruction_type != sem_ir->voidType() || operands.size() != 1) {
      error = "low callback wake terminal operation has invalid storage";
      return false;
    }
    const auto operand_type = value_type(operands.front().index);
    if (sem_ir->type(operand_type).kind == SemTypeKind::CallbackWake) {
      const auto fields = sem_ir->typeBlock(
          TypeBlockId(sem_ir->type(operand_type).arg0));
      if (fields.size() != 1 || fields[0] != plan.completion_type) {
        error = "low callback wake terminal operation has invalid type";
        return false;
      }
    } else if (value.kind != LowInstKind::CallbackDetach ||
               operand_type != plan.completion_type) {
      error = "low callback wake terminal operation has invalid type";
      return false;
    }
    return true;
  }
  default:
    return true;
  }
}

} // namespace chtholly::compiler::internal
