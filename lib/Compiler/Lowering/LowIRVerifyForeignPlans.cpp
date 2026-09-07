#include "LowIRVerificationContext.h"

#include <algorithm>
#include <ranges>
#include <unordered_set>

namespace chtholly::compiler::internal {

bool LowIRVerificationContext::verifyForeignPlans(std::string &error) const {
  const auto sem_ir_ = low_ir_.sem_ir_;
  const auto &callback_adapter_plan_by_type_ = low_ir_.callback_adapter_plan_by_type_;
  const auto &callback_adapter_plans_ = low_ir_.callback_adapter_plans_;
  const auto &callback_completion_plan_by_type_ = low_ir_.callback_completion_plan_by_type_;
  const auto &callback_completion_plans_ = low_ir_.callback_completion_plans_;
  const auto &callback_readiness_plan_by_type_ = low_ir_.callback_readiness_plan_by_type_;
  const auto &callback_readiness_plans_ = low_ir_.callback_readiness_plans_;
  const auto &callback_registration_plan_by_type_ = low_ir_.callback_registration_plan_by_type_;
  const auto &callback_registration_plans_ = low_ir_.callback_registration_plans_;
  const auto &callback_wake_plan_by_type_ = low_ir_.callback_wake_plan_by_type_;
  const auto &callback_wake_plans_ = low_ir_.callback_wake_plans_;
  const auto &foreign_abi_call_layouts_ = low_ir_.foreign_abi_call_layouts_;
  const auto &foreign_abi_layout_by_callback_type_ = low_ir_.foreign_abi_layout_by_callback_type_;
  const auto &foreign_abi_layout_by_target_ = low_ir_.foreign_abi_layout_by_target_;
  const auto &foreign_abi_layouts_ = low_ir_.foreign_abi_layouts_;
  const auto &foreign_abi_thunk_plans_ = low_ir_.foreign_abi_thunk_plans_;
  const auto &foreign_operation_callback_plans_ = low_ir_.foreign_operation_callback_plans_;
  const auto &foreign_operation_completion_plans_ = low_ir_.foreign_operation_completion_plans_;
  const auto &foreign_operation_plans_ = low_ir_.foreign_operation_plans_;
  const auto buildForeignAbiLayout = [&](FunctionRefId target, std::string &err) {
    return low_ir_.buildForeignAbiLayout(target, err);
  };
  const auto buildCallbackAbiLayout = [&](TypeId type, std::string &err) {
    return low_ir_.buildCallbackAbiLayout(type, err);
  };
  const auto buildForeignAbiThunkPlan = [&](FunctionRefId source, TypeId type,
                                            std::string &err) {
    return low_ir_.buildForeignAbiThunkPlan(source, type, err);
  };
  const auto buildCallbackAdapterPlan = [&](TypeId type, std::string &err) {
    return low_ir_.buildCallbackAdapterPlan(type, err);
  };
  const auto buildCallbackCompletionPlan = [&](TypeId type, std::string &err) {
    return low_ir_.buildCallbackCompletionPlan(type, err);
  };
  const auto buildCallbackReadinessPlan = [&](TypeId type, std::string &err) {
    return low_ir_.buildCallbackReadinessPlan(type, err);
  };
  const auto buildCallbackWakePlan = [&](TypeId type, std::string &err) {
    return low_ir_.buildCallbackWakePlan(type, err);
  };
  const auto buildCallbackRegistrationPlan = [&](TypeId type, std::string &err) {
    return low_ir_.buildCallbackRegistrationPlan(type, err);
  };
  const auto cDefaultPromotedType = [&](TypeId type) {
    return low_ir_.cDefaultPromotedType(type);
  };

  const auto is_foreign = [&](FunctionRefId target) {
    const auto &reference = sem_ir_->functionRef(target);
    if (reference.local_function.hasValue())
      return sem_ir_->functionDeclaration(reference.local_function).kind ==
             SemCallableDeclarationKind::Foreign;
    const auto *entity =
        sem_ir_->importIRs().tryGetEntity(reference.public_entity);
    return entity &&
           entity->declaration_kind == PublicCallableDeclarationKind::Foreign;
  };
  std::size_t expected_foreign_count = 0;
  for (std::uint32_t index = 0; index < sem_ir_->functionRefCount(); ++index) {
    const auto target = FunctionRefId(index);
    const auto layout_id = foreign_abi_layout_by_target_[index];
    if (!is_foreign(target)) {
      if (layout_id.hasValue()) {
        error = "ordinary callable owns a foreign ABI layout";
        return false;
      }
      continue;
    }
    ++expected_foreign_count;
    if (!layout_id.hasValue() ||
        layout_id.index >= foreign_abi_layouts_.size()) {
      error = "foreign callable has no LowIR ABI layout";
      return false;
    }
    std::string layout_error;
    const auto expected = buildForeignAbiLayout(target, layout_error);
    if (!expected || expected->callback_type.hasValue() ||
        expected->abi_epoch != 11 ||
        *expected != foreign_abi_layouts_[layout_id.index]) {
      error = layout_error.empty()
                  ? "LowIR foreign ABI layout does not match its query"
                  : std::move(layout_error);
      return false;
    }
  }
  std::size_t expected_callback_count = 0;
  for (std::uint32_t index = 0; index < sem_ir_->typeCount(); ++index) {
    const auto type = TypeId(index);
    const auto kind = sem_ir_->type(type).kind;
    const auto layout_id = foreign_abi_layout_by_callback_type_[index];
    if (kind != SemTypeKind::CFunctionPointer &&
        kind != SemTypeKind::CVariadicFunctionPointer) {
      if (layout_id.hasValue()) {
        error = "non-callback type owns a callback ABI layout";
        return false;
      }
      continue;
    }
    ++expected_callback_count;
    if (!layout_id.hasValue() ||
        layout_id.index >= foreign_abi_layouts_.size()) {
      error = "C function-pointer type has no LowIR ABI layout";
      return false;
    }
    std::string layout_error;
    const auto expected = buildCallbackAbiLayout(type, layout_error);
    if (!expected || expected->target.hasValue() || expected->abi_epoch != 11 ||
        *expected != foreign_abi_layouts_[layout_id.index]) {
      error = layout_error.empty()
                  ? "LowIR callback ABI layout does not match its query"
                  : std::move(layout_error);
      return false;
    }
  }
  if (expected_foreign_count + expected_callback_count !=
      foreign_abi_layouts_.size()) {
    error = "LowIR foreign ABI layout table has duplicate entries";
    return false;
  }
  for (const auto &call : foreign_abi_call_layouts_) {
    if (!call.function_layout.hasValue() ||
        call.function_layout.index >= foreign_abi_layouts_.size() ||
        call.source_suffix_types.size() != call.suffix.size() ||
        call.abi_epoch != 11) {
      error = "LowIR has an invalid foreign call-site ABI descriptor";
      return false;
    }
    const auto &fixed = foreign_abi_layouts_[call.function_layout.index];
    if (fixed.abi_epoch != 11 ||
        (fixed.target.hasValue() == fixed.callback_type.hasValue()) ||
        (!fixed.is_variadic && !call.suffix.empty()) ||
        std::ranges::any_of(call.source_suffix_types, [&](TypeId type) {
          return !type.hasValue() ||
                 cDefaultPromotedType(type) == TypeId::invalid();
        })) {
      error = "LowIR foreign call-site suffix violates C promotion rules";
      return false;
    }
    for (std::size_t index = 0; index < call.suffix.size(); ++index)
      if (call.suffix[index].semantic_type !=
          cDefaultPromotedType(call.source_suffix_types[index])) {
        error = "LowIR foreign call-site suffix is not canonically promoted";
        return false;
      }
  }
  for (const auto &plan : foreign_abi_thunk_plans_) {
    if (plan.abi_epoch != 10 || !plan.callback_layout.hasValue() ||
        plan.callback_layout.index >= foreign_abi_layouts_.size()) {
      error = "LowIR has an invalid callback thunk plan header";
      return false;
    }
    std::string plan_error;
    const auto expected =
        buildForeignAbiThunkPlan(plan.source, plan.callback_type, plan_error);
    if (!expected || *expected != plan ||
        plan.target_kind == ReverseThunkTargetKind::Count ||
        plan.role == ReverseThunkRole::Count ||
        (plan.context_parameter == core::AnyId::InvalidIndex) !=
            !plan.context_carrier.hasValue()) {
      error = plan_error.empty()
                  ? "LowIR callback thunk plan does not match its query"
                  : std::move(plan_error);
      return false;
    }
  }
  for (const auto &plan : foreign_operation_plans_.values()) {
    static constexpr std::array expected_phases{
        ForeignOperationPhase::Prepare, ForeignOperationPhase::Invoke,
        ForeignOperationPhase::Classify, ForeignOperationPhase::Resolve,
        ForeignOperationPhase::Publish};
    if (!plan.operation_fingerprint.hasValue() ||
        plan.phases != std::vector<ForeignOperationPhase>(
                           expected_phases.begin(), expected_phases.end()) ||
        !std::ranges::is_sorted(plan.input_lanes) ||
        !std::ranges::is_sorted(plan.callback_lanes) ||
        !std::ranges::is_sorted(plan.output_lanes) ||
        !std::ranges::is_sorted(plan.success_lanes) ||
        !std::ranges::is_sorted(plan.failure_lanes) || plan.abi_epoch != 1) {
      error = "LowIR foreign operation plan is stale or not canonical";
      return false;
    }
    if (plan.completion_projection >=
            interop::ForeignCompletionProjectionKind::Count ||
        plan.completion_input_effect >=
            interop::ForeignCompletionInputEffect::Count ||
        !std::ranges::is_sorted(plan.wake_callback_lanes) ||
        std::ranges::adjacent_find(plan.wake_callback_lanes) !=
            plan.wake_callback_lanes.end()) {
      error = "LowIR foreign operation projection metadata is not canonical";
      return false;
    }
  }
  for (const auto &plan : foreign_operation_callback_plans_.values()) {
    if (!plan.operation.hasValue() ||
        plan.operation.index >= foreign_operation_plans_.size() ||
        !plan.adapter.hasValue() ||
        plan.adapter.index >= callback_adapter_plans_.size() ||
        plan.lanes[0] == core::AnyId::InvalidIndex ||
        plan.lanes[1] == core::AnyId::InvalidIndex ||
        plan.lanes[2] == core::AnyId::InvalidIndex ||
        !std::ranges::is_sorted(plan.lanes) ||
        std::ranges::adjacent_find(plan.lanes) != plan.lanes.end() ||
        plan.authority >= CallbackReleaseAuthority::Count ||
        plan.abi_epoch != 1 || !plan.entry_thunk.hasValue() ||
        plan.entry_thunk.index >= foreign_abi_thunk_plans_.size() ||
        !plan.context_thunk.hasValue() ||
        plan.context_thunk.index >= foreign_abi_thunk_plans_.size() ||
        !plan.release_thunk.hasValue() ||
        plan.release_thunk.index >= foreign_abi_thunk_plans_.size()) {
      error = "LowIR foreign operation callback plan has invalid lanes or IDs";
      return false;
    }
    const auto &operation = foreign_operation_plans_.get(plan.operation);
    if (operation.callback_lanes !=
        std::vector<std::uint32_t>(plan.lanes.begin(), plan.lanes.end())) {
      error = "LowIR foreign operation callback lanes disagree with operation";
      return false;
    }
    const auto &adapter = callback_adapter_plans_[plan.adapter.index];
    if (!adapter.adapter_type.hasValue() ||
        adapter.adapter_type.index >= sem_ir_->typeCount() ||
        sem_ir_->type(adapter.adapter_type).kind !=
            SemTypeKind::CallbackAdapter) {
      error = "LowIR foreign operation callback adapter plan is invalid";
      return false;
    }
    for (const auto thunk_id :
         {plan.entry_thunk, plan.context_thunk, plan.release_thunk}) {
      const auto &thunk = foreign_abi_thunk_plans_[thunk_id.index];
      std::string thunk_error;
      const auto expected = buildForeignAbiThunkPlan(
          thunk.source, thunk.callback_type, thunk_error);
      if (!expected || *expected != thunk) {
        error = thunk_error.empty()
                    ? "LowIR foreign operation callback thunk is stale"
                    : std::move(thunk_error);
        return false;
      }
    }
  }
  for (const auto &plan : foreign_operation_completion_plans_.values()) {
    if (!plan.operation.hasValue() ||
        plan.operation.index >= foreign_operation_plans_.size() ||
        !plan.completion_carrier.hasValue() ||
        plan.completion_carrier.index >= sem_ir_->typeCount() ||
        plan.family == 0 ||
        plan.family >= static_cast<std::uint32_t>(
                           interop::ForeignCompletionFamily::Count) ||
        plan.projection >= interop::ForeignCompletionProjectionKind::Count ||
        plan.input_effect >= interop::ForeignCompletionInputEffect::Count ||
        plan.carrier_lane == core::AnyId::InvalidIndex ||
        plan.result_lane == core::AnyId::InvalidIndex || plan.abi_epoch != 1 ||
        !std::ranges::is_sorted(plan.roles) ||
        std::ranges::adjacent_find(plan.roles) != plan.roles.end() ||
        std::ranges::any_of(plan.roles, [](auto role) {
          return role >= ForeignOperationCompletionRole::Count;
        })) {
      error = "LowIR foreign operation completion plan is stale or ambiguous";
      return false;
    }
    const auto &operation = foreign_operation_plans_.get(plan.operation);
    if (operation.operation_fingerprint != plan.operation_fingerprint ||
        operation.completion_projection != plan.projection ||
        operation.completion_input_effect != plan.input_effect ||
        operation.completion_carrier_lane != plan.carrier_lane ||
        operation.completion_result_lane != plan.result_lane ||
        operation.wake_callback_lanes != plan.wake_callback_lanes) {
      error = "LowIR foreign operation completion plan has stale operation";
      return false;
    }
  }
  if (callback_adapter_plan_by_type_.size() != sem_ir_->typeCount()) {
    error = "LowIR has an incomplete callback adapter plan index";
    return false;
  }
  std::size_t expected_adapter_count = 0;
  for (std::uint32_t index = 0; index < sem_ir_->typeCount(); ++index) {
    const auto type = TypeId(index);
    const auto plan_id = callback_adapter_plan_by_type_[index];
    if (sem_ir_->type(type).kind != SemTypeKind::CallbackAdapter) {
      if (plan_id.hasValue()) {
        error = "non-adapter type owns a callback adapter plan";
        return false;
      }
      continue;
    }
    ++expected_adapter_count;
    if (!plan_id.hasValue() ||
        plan_id.index >= callback_adapter_plans_.size()) {
      error = "callback adapter type has no verified LowIR plan";
      return false;
    }
    std::string plan_error;
    const auto expected = buildCallbackAdapterPlan(type, plan_error);
    if (!expected || expected->abi_epoch != 10 ||
        expected->local_release_authority !=
            CallbackReleaseAuthority::Retained ||
        !expected->context_carrier.hasValue() ||
        *expected != callback_adapter_plans_[plan_id.index]) {
      error = plan_error.empty()
                  ? "LowIR callback adapter plan does not match its type"
                  : std::move(plan_error);
      return false;
    }
  }
  if (expected_adapter_count != callback_adapter_plans_.size()) {
    error = "LowIR callback adapter plan table has duplicate entries";
    return false;
  }
  if (callback_completion_plan_by_type_.size() != sem_ir_->typeCount()) {
    error = "LowIR has an incomplete callback completion plan index";
    return false;
  }
  std::size_t expected_completion_count = 0;
  for (std::uint32_t index = 0; index < sem_ir_->typeCount(); ++index) {
    const auto type = TypeId(index);
    const auto plan_id = callback_completion_plan_by_type_[index];
    if (sem_ir_->type(type).kind != SemTypeKind::CallbackCompletion) {
      if (plan_id.hasValue()) {
        error = "non-completion type owns a callback completion plan";
        return false;
      }
      continue;
    }
    ++expected_completion_count;
    if (!plan_id.hasValue() ||
        plan_id.index >= callback_completion_plans_.size()) {
      error = "callback completion type has no verified LowIR plan";
      return false;
    }
    std::string plan_error;
    const auto expected = buildCallbackCompletionPlan(type, plan_error);
    if (!expected || expected->abi_epoch != 12 ||
        *expected != callback_completion_plans_[plan_id.index]) {
      error = plan_error.empty()
                  ? "LowIR callback completion plan does not match its type"
                  : std::move(plan_error);
      return false;
    }
  }
  if (expected_completion_count != callback_completion_plans_.size()) {
    error = "LowIR callback completion plan table has duplicate entries";
    return false;
  }
  if (callback_readiness_plan_by_type_.size() != sem_ir_->typeCount()) {
    error = "LowIR has an incomplete callback readiness plan index";
    return false;
  }
  std::size_t expected_readiness_count = 0;
  for (std::uint32_t index = 0; index < sem_ir_->typeCount(); ++index) {
    const auto type = TypeId(index);
    const auto plan_id = callback_readiness_plan_by_type_[index];
    const auto poll_capable =
        sem_ir_->type(type).kind == SemTypeKind::CallbackCompletion &&
        (sem_ir_->typeBlock(TypeBlockId(sem_ir_->type(type).arg0)).size() ==
             5 ||
         sem_ir_->typeBlock(TypeBlockId(sem_ir_->type(type).arg0)).size() == 7);
    if (!poll_capable) {
      if (plan_id.hasValue()) {
        error = "non-pollable type owns a callback readiness plan";
        return false;
      }
      continue;
    }
    ++expected_readiness_count;
    if (!plan_id.hasValue() ||
        plan_id.index >= callback_readiness_plans_.size()) {
      error = "pollable callback completion has no readiness plan";
      return false;
    }
    std::string plan_error;
    const auto expected = buildCallbackReadinessPlan(type, plan_error);
    if (!expected || expected->abi_epoch != 13 ||
        *expected != callback_readiness_plans_[plan_id.index]) {
      error = plan_error.empty()
                  ? "LowIR callback readiness plan does not match its type"
                  : std::move(plan_error);
      return false;
    }
  }
  if (expected_readiness_count != callback_readiness_plans_.size()) {
    error = "LowIR callback readiness plan table has duplicate entries";
    return false;
  }
  if (callback_wake_plan_by_type_.size() != sem_ir_->typeCount()) {
    error = "LowIR has an incomplete callback wake plan index";
    return false;
  }
  std::size_t expected_wake_count = 0;
  for (std::uint32_t index = 0; index < sem_ir_->typeCount(); ++index) {
    const auto type = TypeId(index);
    const auto plan_id = callback_wake_plan_by_type_[index];
    const auto is_completion =
        sem_ir_->type(type).kind == SemTypeKind::CallbackCompletion &&
        sem_ir_->typeBlock(TypeBlockId(sem_ir_->type(type).arg0)).size() == 7;
    const auto is_wake = sem_ir_->type(type).kind == SemTypeKind::CallbackWake;
    if (!is_completion && !is_wake) {
      if (plan_id.hasValue()) {
        error = "non-wake-capable type owns a callback wake plan";
        return false;
      }
      continue;
    }
    if (!plan_id.hasValue() || plan_id.index >= callback_wake_plans_.size()) {
      error = "wake-capable callback type has no epoch-14 plan";
      return false;
    }
    TypeId completion = type;
    if (is_wake) {
      const auto fields =
          sem_ir_->typeBlock(TypeBlockId(sem_ir_->type(type).arg0));
      if (fields.size() != 1) {
        error = "callback wake type has invalid completion storage";
        return false;
      }
      completion = fields[0];
    } else {
      ++expected_wake_count;
    }
    std::string plan_error;
    const auto expected = buildCallbackWakePlan(completion, plan_error);
    if (!expected || expected->abi_epoch != 14 ||
        *expected != callback_wake_plans_[plan_id.index]) {
      error = plan_error.empty()
                  ? "LowIR callback wake plan does not match its type"
                  : std::move(plan_error);
      return false;
    }
  }
  if (expected_wake_count != callback_wake_plans_.size()) {
    error = "LowIR callback wake plan table has duplicate entries";
    return false;
  }
  if (callback_registration_plan_by_type_.size() != sem_ir_->typeCount()) {
    error = "LowIR has an incomplete callback registration plan index";
    return false;
  }
  std::size_t expected_registration_count = 0;
  for (std::uint32_t index = 0; index < sem_ir_->typeCount(); ++index) {
    const auto type = TypeId(index);
    const auto plan_id = callback_registration_plan_by_type_[index];
    if (sem_ir_->type(type).kind != SemTypeKind::CallbackRegistration) {
      if (plan_id.hasValue()) {
        error = "non-registration type owns a callback registration plan";
        return false;
      }
      continue;
    }
    ++expected_registration_count;
    if (!plan_id.hasValue() ||
        plan_id.index >= callback_registration_plans_.size()) {
      error = "callback registration type has no verified LowIR plan";
      return false;
    }
    std::string plan_error;
    const auto expected = buildCallbackRegistrationPlan(type, plan_error);
    if (!expected || expected->abi_epoch != 11 ||
        *expected != callback_registration_plans_[plan_id.index]) {
      error = plan_error.empty()
                  ? "LowIR callback registration plan does not match its type"
                  : std::move(plan_error);
      return false;
    }
  }
  if (expected_registration_count != callback_registration_plans_.size()) {
    error = "LowIR callback registration plan table has duplicate entries";
    return false;
  }
  return true;
}


} // namespace chtholly::compiler::internal
