#include "chtholly/Compiler/LowIR.h"

#include "chtholly/Compiler/BuiltinOperator.h"
#include "chtholly/Compiler/CallableOwnership.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cctype>
#include <deque>
#include <functional>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace chtholly::compiler {
namespace {
#include "LowIRForeignABIHelpersInternal.h"
} // namespace

void LowIR::buildForeignAbiLayouts() {
  foreign_abi_layout_by_target_.assign(sem_ir_->functionRefCount(),
                                       ForeignAbiLayoutId::invalid());
  for (std::uint32_t index = 0; index < sem_ir_->functionRefCount(); ++index) {
    const auto target = FunctionRefId(index);
    if (!isForeign(*sem_ir_, target))
      continue;
    std::string error;
    auto layout = buildForeignAbiLayout(target, error);
    if (!layout) {
      if (foreign_abi_error_.empty())
        foreign_abi_error_ = std::move(error);
      continue;
    }
    const auto id = ForeignAbiLayoutId(
        static_cast<std::uint32_t>(foreign_abi_layouts_.size()));
    foreign_abi_layouts_.push_back(std::move(*layout));
    foreign_abi_layout_by_target_[index] = id;
  }
  foreign_abi_layout_by_callback_type_.assign(sem_ir_->typeCount(),
                                              ForeignAbiLayoutId::invalid());
  for (std::uint32_t index = 0; index < sem_ir_->typeCount(); ++index) {
    const auto type = TypeId(index);
    const auto kind = sem_ir_->type(type).kind;
    if (kind != SemTypeKind::CFunctionPointer &&
        kind != SemTypeKind::CVariadicFunctionPointer)
      continue;
    std::string error;
    auto layout = buildCallbackAbiLayout(type, error);
    if (!layout) {
      if (foreign_abi_error_.empty())
        foreign_abi_error_ = std::move(error);
      continue;
    }
    const auto id = ForeignAbiLayoutId(
        static_cast<std::uint32_t>(foreign_abi_layouts_.size()));
    foreign_abi_layouts_.push_back(std::move(*layout));
    foreign_abi_layout_by_callback_type_[index] = id;
  }
  callback_adapter_plan_by_type_.assign(sem_ir_->typeCount(),
                                        CallbackAdapterPlanId::invalid());
  for (std::uint32_t index = 0; index < sem_ir_->typeCount(); ++index) {
    const auto adapter_type = TypeId(index);
    const auto &adapter = sem_ir_->type(adapter_type);
    if (adapter.kind != SemTypeKind::CallbackAdapter)
      continue;
    const auto fields = sem_ir_->typeBlock(TypeBlockId(adapter.arg0));
    if (fields.size() != 3)
      continue;
    std::string error;
    (void)addForeignAbiCallLayout(fields[0], {}, error);
    if (error.empty())
      (void)addForeignAbiCallLayout(fields[2], {}, error);
    auto plan = error.empty() ? buildCallbackAdapterPlan(adapter_type, error)
                              : std::nullopt;
    if (!plan) {
      if (foreign_abi_error_.empty())
        foreign_abi_error_ = std::move(error);
      continue;
    }
    const auto id = CallbackAdapterPlanId(
        static_cast<std::uint32_t>(callback_adapter_plans_.size()));
    callback_adapter_plans_.push_back(std::move(*plan));
    callback_adapter_plan_by_type_[index] = id;
  }
  callback_completion_plan_by_type_.assign(sem_ir_->typeCount(),
                                           CallbackCompletionPlanId::invalid());
  for (std::uint32_t index = 0; index < sem_ir_->typeCount(); ++index) {
    const auto type = TypeId(index);
    if (sem_ir_->type(type).kind != SemTypeKind::CallbackCompletion)
      continue;
    const auto fields =
        sem_ir_->typeBlock(TypeBlockId(sem_ir_->type(type).arg0));
    if (fields.size() != 4 && fields.size() != 5 && fields.size() != 7)
      continue;
    std::string error;
    (void)addForeignAbiCallLayout(fields[3], {}, error);
    if (error.empty() && fields.size() >= 5)
      (void)addForeignAbiCallLayout(fields[4], {}, error);
    auto plan =
        error.empty() ? buildCallbackCompletionPlan(type, error) : std::nullopt;
    if (!plan) {
      if (foreign_abi_error_.empty())
        foreign_abi_error_ = std::move(error);
      continue;
    }
    const auto id = CallbackCompletionPlanId(
        static_cast<std::uint32_t>(callback_completion_plans_.size()));
    callback_completion_plans_.push_back(std::move(*plan));
    callback_completion_plan_by_type_[index] = id;
  }
  callback_readiness_plan_by_type_.assign(sem_ir_->typeCount(),
                                          CallbackReadinessPlanId::invalid());
  for (std::uint32_t index = 0; index < sem_ir_->typeCount(); ++index) {
    const auto type = TypeId(index);
    if (sem_ir_->type(type).kind != SemTypeKind::CallbackCompletion)
      continue;
    const auto fields =
        sem_ir_->typeBlock(TypeBlockId(sem_ir_->type(type).arg0));
    if (fields.size() != 5 && fields.size() != 7)
      continue;
    std::string error;
    auto plan = buildCallbackReadinessPlan(type, error);
    if (!plan) {
      if (foreign_abi_error_.empty())
        foreign_abi_error_ = std::move(error);
      continue;
    }
    const auto id = CallbackReadinessPlanId(
        static_cast<std::uint32_t>(callback_readiness_plans_.size()));
    callback_readiness_plans_.push_back(std::move(*plan));
    callback_readiness_plan_by_type_[index] = id;
  }
  callback_wake_plan_by_type_.assign(sem_ir_->typeCount(),
                                     CallbackWakePlanId::invalid());
  for (std::uint32_t index = 0; index < sem_ir_->typeCount(); ++index) {
    const auto type = TypeId(index);
    if (sem_ir_->type(type).kind != SemTypeKind::CallbackCompletion)
      continue;
    const auto fields =
        sem_ir_->typeBlock(TypeBlockId(sem_ir_->type(type).arg0));
    if (fields.size() != 7)
      continue;
    std::string error;
    (void)addForeignAbiCallLayout(fields[5], {}, error);
    if (error.empty())
      (void)addForeignAbiCallLayout(fields[6], {}, error);
    const auto arm_parameters = sem_ir_->callbackArmParameters(type);
    const auto arm_values =
        sem_ir_->typeBlock(TypeBlockId(sem_ir_->type(fields[5]).arg0));
    if (error.empty() && arm_parameters[3] < arm_values.size())
      (void)addForeignAbiCallLayout(arm_values[arm_parameters[3]], {}, error);
    auto plan =
        error.empty() ? buildCallbackWakePlan(type, error) : std::nullopt;
    if (!plan) {
      if (foreign_abi_error_.empty())
        foreign_abi_error_ = std::move(error);
      continue;
    }
    const auto id = CallbackWakePlanId(
        static_cast<std::uint32_t>(callback_wake_plans_.size()));
    callback_wake_plans_.push_back(std::move(*plan));
    callback_wake_plan_by_type_[index] = id;
    for (std::uint32_t wake_index = 0; wake_index < sem_ir_->typeCount();
         ++wake_index) {
      const auto wake = TypeId(wake_index);
      if (sem_ir_->type(wake).kind != SemTypeKind::CallbackWake)
        continue;
      const auto wake_fields =
          sem_ir_->typeBlock(TypeBlockId(sem_ir_->type(wake).arg0));
      if (wake_fields.size() == 1 && wake_fields[0] == type)
        callback_wake_plan_by_type_[wake_index] = id;
    }
  }
  callback_registration_plan_by_type_.assign(
      sem_ir_->typeCount(), CallbackRegistrationPlanId::invalid());
  for (std::uint32_t index = 0; index < sem_ir_->typeCount(); ++index) {
    const auto type = TypeId(index);
    if (sem_ir_->type(type).kind != SemTypeKind::CallbackRegistration)
      continue;
    std::string error;
    const auto fields =
        sem_ir_->typeBlock(TypeBlockId(sem_ir_->type(type).arg0));
    if (fields.size() != 5 && fields.size() != 7 && fields.size() != 8 &&
        fields.size() != 10)
      continue;
    const auto callback_plan = callbackAdapterPlanFor(fields[0]);
    (void)addForeignAbiCallLayout(fields[2], {}, error);
    if (error.empty())
      (void)addForeignAbiCallLayout(fields[3], {}, error);
    if (error.empty())
      (void)addForeignAbiCallLayout(fields[4], {}, error);
    if (error.empty() && fields.size() >= 7)
      (void)addForeignAbiCallLayout(fields[5], {}, error);
    if (error.empty() && fields.size() >= 7)
      (void)addForeignAbiCallLayout(fields[6], {}, error);
    if (error.empty() && fields.size() >= 8)
      (void)addForeignAbiCallLayout(fields[7], {}, error);
    if (error.empty() && fields.size() == 10)
      (void)addForeignAbiCallLayout(fields[8], {}, error);
    if (error.empty() && fields.size() == 10)
      (void)addForeignAbiCallLayout(fields[9], {}, error);
    auto plan = error.empty() ? buildCallbackRegistrationPlan(type, error)
                              : std::nullopt;
    if (!plan) {
      if (foreign_abi_error_.empty())
        foreign_abi_error_ = std::move(error);
      continue;
    }
    plan->callback_plan = callback_plan;
    const auto id = CallbackRegistrationPlanId(
        static_cast<std::uint32_t>(callback_registration_plans_.size()));
    callback_registration_plans_.push_back(std::move(*plan));
    callback_registration_plan_by_type_[index] = id;
  }
}

TypeId LowIR::cDefaultPromotedType(TypeId type) const {
  if (!type.hasValue() || type.index >= sem_ir_->typeCount())
    return TypeId::invalid();
  const auto &value = sem_ir_->type(type);
  if (value.kind == SemTypeKind::Bool ||
      (value.kind == SemTypeKind::Integer && value.arg0 < 32))
    return sem_ir_->i32Type();
  if (value.kind == SemTypeKind::Float && value.arg0 == 32)
    return sem_ir_->f64Type();
  if ((value.kind == SemTypeKind::Integer &&
       (value.arg0 == 32 || value.arg0 == 64)) ||
      (value.kind == SemTypeKind::Float && value.arg0 == 64) ||
      value.kind == SemTypeKind::Reference ||
      value.kind == SemTypeKind::RawPointer ||
      value.kind == SemTypeKind::CFunctionPointer ||
      value.kind == SemTypeKind::CVariadicFunctionPointer)
    return type;
  return TypeId::invalid();
}

ForeignAbiCallLayoutId
LowIR::addForeignAbiCallLayout(FunctionRefId target,
                               std::span<const TypeId> source_suffix_types,
                               std::string &error) {
  return addForeignAbiCallLayout(foreignAbiLayoutFor(target),
                                 source_suffix_types, error);
}

ForeignAbiCallLayoutId
LowIR::addForeignAbiCallLayout(TypeId callback_type,
                               std::span<const TypeId> source_suffix_types,
                               std::string &error) {
  return addForeignAbiCallLayout(foreignAbiLayoutForCallback(callback_type),
                                 source_suffix_types, error);
}

ForeignAbiCallLayoutId
LowIR::addForeignAbiCallLayout(ForeignAbiLayoutId fixed_id,
                               std::span<const TypeId> source_suffix_types,
                               std::string &error) {
  error.clear();
  if (!fixed_id.hasValue()) {
    error = "foreign call has no fixed ABI layout";
    return ForeignAbiCallLayoutId::invalid();
  }
  const auto &fixed = foreignAbiLayout(fixed_id);
  if ((!fixed.is_variadic && !source_suffix_types.empty()) ||
      (fixed.is_variadic && fixed.parameters.empty())) {
    error = "foreign call suffix disagrees with the callable signature";
    return ForeignAbiCallLayoutId::invalid();
  }
  for (std::uint32_t index = 0; index < foreign_abi_call_layouts_.size();
       ++index) {
    const auto &existing = foreign_abi_call_layouts_[index];
    if (existing.function_layout == fixed_id &&
        std::ranges::equal(existing.source_suffix_types, source_suffix_types))
      return ForeignAbiCallLayoutId(index);
  }
  ForeignAbiCallLayout call{.function_layout = fixed_id};
  call.source_suffix_types.assign(source_suffix_types.begin(),
                                  source_suffix_types.end());
  for (const auto source : source_suffix_types) {
    const auto promoted = cDefaultPromotedType(source);
    if (!promoted.hasValue()) {
      error = "foreign variadic suffix has an unsupported semantic type";
      return ForeignAbiCallLayoutId::invalid();
    }
    const auto &type = sem_ir_->type(promoted);
    ForeignAbiValueLayout value;
    value.kind = ForeignPassKind::Scalar;
    value.semantic_type = promoted;
    if (type.kind == SemTypeKind::Reference ||
        type.kind == SemTypeKind::RawPointer ||
        type.kind == SemTypeKind::CFunctionPointer ||
        type.kind == SemTypeKind::CVariadicFunctionPointer) {
      value.lanes.push_back({ForeignPhysicalKind::Pointer, 64});
      value.size = value.alignment = 8;
    } else if (type.kind == SemTypeKind::Integer) {
      value.lanes.push_back({ForeignPhysicalKind::Integer, type.arg0});
      value.size = value.alignment = type.arg0 / 8U;
    } else if (type.kind == SemTypeKind::Float && type.arg0 == 64) {
      value.lanes.push_back({ForeignPhysicalKind::Float64, 64});
      value.size = value.alignment = 8;
    } else {
      error = "C default promotion did not produce a transport scalar";
      return ForeignAbiCallLayoutId::invalid();
    }
    call.suffix.push_back(std::move(value));
  }
  const auto id = ForeignAbiCallLayoutId(
      static_cast<std::uint32_t>(foreign_abi_call_layouts_.size()));
  foreign_abi_call_layouts_.push_back(std::move(call));
  return id;
}

} // namespace chtholly::compiler
