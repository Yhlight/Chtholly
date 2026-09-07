#include "LowerToLowIRInternal.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <string>
#include <vector>

namespace chtholly::compiler::internal {
namespace {

template <typename InstT>
LowInstId emit(LoweringExpressionState &state, LowBlockId block, InstT inst,
               InstId origin) {
  const auto id = state.low_ir.addInst(inst, origin);
  state.pending_blocks[block.index - state.low_ir.blockCount()].push_back(id);
  return id;
}

} // namespace

ForeignOperationPlanId LoweringInteropService::operationPlan(
    LowIR &low_ir, const interop::ForeignOperationArtifact &operation) {
  ForeignOperationPlan plan;
  plan.operation_fingerprint = operation.fingerprint;
  plan.success_lanes = operation.success_lanes;
  plan.failure_lanes = operation.failure_lanes;
  plan.success_payload_kind =
      static_cast<std::uint8_t>(operation.success_payload);
  plan.failure_payload_kind =
      static_cast<std::uint8_t>(operation.failure_payload);
  plan.completion_projection = operation.completion_projection;
  plan.completion_input_effect = operation.completion_input_effect;
  plan.completion_carrier_lane = operation.completion_carrier_lane;
  plan.completion_result_lane = operation.completion_result_lane;
  plan.wake_callback_lanes = operation.wake_callback_lanes;
  for (const auto &port : operation.ports) {
    auto *lanes = port.kind == interop::ForeignPortKind::Input
                      ? &plan.input_lanes
                  : port.kind == interop::ForeignPortKind::Callback
                      ? &plan.callback_lanes
                  : port.kind == interop::ForeignPortKind::Output
                      ? &plan.output_lanes
                      : nullptr;
    if (lanes)
      lanes->insert(lanes->end(), port.lanes.begin(), port.lanes.end());
  }
  for (auto *lanes : {&plan.input_lanes, &plan.callback_lanes,
                      &plan.output_lanes}) {
    std::ranges::sort(*lanes);
    lanes->erase(std::unique(lanes->begin(), lanes->end()), lanes->end());
  }
  return low_ir.addForeignOperationPlan(std::move(plan));
}

ForeignOperationCompletionPlanId LoweringInteropService::completionPlan(
    LowIR &low_ir, ForeignOperationPlanId operation_id,
    const interop::ForeignOperationArtifact &operation,
    TypeId completion_carrier) {
  ForeignOperationCompletionPlan plan;
  plan.operation = operation_id;
  plan.operation_fingerprint = operation.fingerprint;
  plan.family = static_cast<std::uint32_t>(operation.completion_family);
  for (const auto &capability : operation.capabilities) {
    std::optional<ForeignOperationCompletionRole> role;
    if (capability.path == "async.action.wait")
      role = ForeignOperationCompletionRole::Wait;
    else if (capability.path == "async.action.poll")
      role = ForeignOperationCompletionRole::Poll;
    else if (capability.path == "async.action.arm")
      role = ForeignOperationCompletionRole::Arm;
    else if (capability.path == "async.action.detach")
      role = ForeignOperationCompletionRole::Detach;
    else if (capability.path == "async.action.cancel_async")
      role = ForeignOperationCompletionRole::CancelAsync;
    if (role && std::ranges::find(plan.roles, *role) == plan.roles.end())
      plan.roles.push_back(*role);
  }
  std::ranges::sort(plan.roles);
  plan.completion_carrier = completion_carrier;
  plan.wake_plan = low_ir.callbackWakePlanFor(completion_carrier);
  plan.projection = operation.completion_projection;
  plan.input_effect = operation.completion_input_effect;
  plan.carrier_lane = operation.completion_carrier_lane;
  plan.result_lane = operation.completion_result_lane;
  plan.readiness_success_literal = operation.readiness_success_literal;
  plan.wake_callback_lanes = operation.wake_callback_lanes;
  return low_ir.addForeignOperationCompletionPlan(std::move(plan));
}

void LoweringInteropService::foreignFunctionRef(
    InstId semantic_id, SemForeignFunctionRef semantic, LowBlockId current,
    LoweringExpressionState &state) {
  state.values[semantic_id.index] =
      emit(state, current,
           LowForeignFunctionRef{semantic.type, semantic.arg0, {}}, semantic_id);
}

void LoweringInteropService::callbackAdapter(
    InstId semantic_id, SemCallbackAdapter semantic, LowBlockId current,
    LoweringExpressionState &state) {
  std::string error;
  const auto plan =
      state.low_ir.addForeignAbiThunkPlan(semantic.arg0, semantic.type, error);
  state.values[semantic_id.index] =
      emit(state, current, LowCallbackAdapter{semantic.type, plan, {}},
           semantic_id);
}

void LoweringInteropService::indirectForeignCall(
    InstId semantic_id, SemIndirectForeignCall semantic, LowBlockId current,
    LoweringExpressionState &state) {
  const auto callback_type =
      TypeId(state.sem_ir.inst(semantic.arg0).type);
  const auto fixed_count = state.sem_ir
                               .typeBlock(TypeBlockId(
                                   state.sem_ir.type(callback_type).arg0))
                               .size();
  std::vector<LowInstId> operands{state.value_for(semantic.arg0)};
  std::vector<TypeId> suffix_types;
  const auto semantic_arguments = state.sem_ir.instBlock(semantic.arg1);
  operands.reserve(semantic_arguments.size() + 1);
  for (std::size_t index = 0; index < semantic_arguments.size(); ++index) {
    auto argument = state.value_for(semantic_arguments[index]);
    if (index >= fixed_count) {
      const auto source_type = TypeId(state.low_ir.inst(argument).type);
      suffix_types.push_back(source_type);
      const auto promoted = state.low_ir.cDefaultPromotedType(source_type);
      if (promoted.hasValue() && promoted != source_type)
        argument = emit(state, current,
                        LowForeignDefaultPromote{promoted, argument, {}},
                        semantic_id);
    }
    operands.push_back(argument);
  }
  std::string error;
  const auto call_layout = state.low_ir.addForeignAbiCallLayout(
      callback_type, suffix_types, error);
  state.values[semantic_id.index] = emit(
      state, current,
      LowIndirectForeignCall{semantic.type, call_layout,
                             state.low_ir.addValueBlock(operands)},
      semantic_id);
}

void LoweringInteropService::callbackRegistrationBinding(
    InstId semantic_id, SemCallbackRegistrationBinding semantic,
    LoweringExpressionState &state) {
  state.values[semantic_id.index] = state.value_for(semantic.arg1);
}

void LoweringInteropService::makeCallbackRegistration(
    InstId semantic_id, SemMakeCallbackRegistration semantic,
    LowBlockId current, LoweringExpressionState &state) {
  std::vector<LowInstId> fields;
  const auto operands = state.sem_ir.instBlock(semantic.arg0);
  const auto semantic_fields = state.sem_ir.typeBlock(
      TypeBlockId(state.sem_ir.type(semantic.type).arg0));
  const auto fixed_count =
      semantic_fields.size() == 5 ? 4U : semantic_fields.size() - 1U;
  for (const auto field : operands.first(fixed_count))
    fields.push_back(state.value_for(field));
  for (const auto &binding :
       state.sem_ir.callbackRegistrationBindings(semantic.type)) {
    const auto found = std::ranges::find_if(
        operands.subspan(fixed_count), [&](InstId operand) {
          const auto &inst = state.sem_ir.inst(operand);
          return inst.kind == SemInstKind::CallbackRegistrationBinding &&
                 state.sem_ir.identifier(
                     state.sem_ir.name(NameId(inst.arg0)).text) == binding.name;
        });
    assert(found != operands.end());
    fields.push_back(state.value_for(InstId(state.sem_ir.inst(*found).arg1)));
  }
  const auto plan =
      state.low_ir.callbackRegistrationPlanFor(semantic.type);
  state.values[semantic_id.index] = emit(
      state, current,
      LowMakeCallbackRegistration{semantic.type, plan,
                                  state.low_ir.addValueBlock(fields)},
      semantic_id);
}

void LoweringInteropService::callbackRegistrationActive(
    InstId semantic_id, SemCallbackRegistrationActive semantic,
    LowBlockId current, LoweringExpressionState &state) {
  state.values[semantic_id.index] = emit(
      state, current,
      LowCallbackRegistrationActive{semantic.type, state.value_for(semantic.arg0),
                                    {}},
      semantic_id);
}

void LoweringInteropService::callbackRegistrationFinish(
    InstId semantic_id, InstId registration, TypeId result_type,
    LowBlockId current, LoweringExpressionState &state, bool cancel) {
  const auto registration_type =
      TypeId(state.sem_ir.inst(registration).type);
  const auto plan =
      state.low_ir.callbackRegistrationPlanFor(registration_type);
  const std::array operands{state.value_for(registration)};
  if (cancel)
    state.values[semantic_id.index] = emit(
        state, current,
        LowCallbackRegistrationCancel{result_type, plan,
                                      state.low_ir.addValueBlock(operands)},
        semantic_id);
  else
    state.values[semantic_id.index] = emit(
        state, current,
        LowCallbackRegistrationUnregister{result_type, plan,
                                          state.low_ir.addValueBlock(operands)},
        semantic_id);
}

void LoweringInteropService::callbackRegistrationCancelAsync(
    InstId semantic_id, SemCallbackCancelAsync semantic, LowBlockId current,
    LoweringExpressionState &state) {
  const auto registration_type =
      TypeId(state.sem_ir.inst(semantic.arg0).type);
  const auto plan =
      state.low_ir.callbackRegistrationPlanFor(registration_type);
  assert(plan.hasValue() &&
         state.low_ir.callbackRegistrationPlan(plan).completion_plan.hasValue());
  const std::array operands{state.value_for(semantic.arg0)};
  state.values[semantic_id.index] = emit(
      state, current,
      LowCallbackRegistrationCancelAsync{semantic.type, plan,
                                         state.low_ir.addValueBlock(operands)},
      semantic_id);
}

void LoweringInteropService::callbackCompletionPending(
    InstId semantic_id, SemCallbackCompletionPending semantic,
    LowBlockId current, LoweringExpressionState &state) {
  state.values[semantic_id.index] = emit(
      state, current,
      LowCallbackCompletionPending{semantic.type, state.value_for(semantic.arg0),
                                   {}},
      semantic_id);
}

void LoweringInteropService::callbackCompletionPoll(
    InstId semantic_id, SemCallbackCompletionPoll semantic,
    LowBlockId current, LoweringExpressionState &state) {
  const auto completion_type =
      TypeId(state.sem_ir.inst(semantic.arg0).type);
  const auto plan = state.low_ir.callbackReadinessPlanFor(completion_type);
  assert(plan.hasValue());
  const std::array operands{state.value_for(semantic.arg0)};
  state.values[semantic_id.index] = emit(
      state, current,
      LowCallbackCompletionPoll{semantic.type, plan,
                                state.low_ir.addValueBlock(operands)},
      semantic_id);
}

void LoweringInteropService::callbackWait(
    InstId semantic_id, SemCallbackWait semantic, LowBlockId current,
    LoweringExpressionState &state) {
  const auto operand_type = TypeId(state.sem_ir.inst(semantic.arg0).type);
  const std::array operands{state.value_for(semantic.arg0)};
  if (state.sem_ir.type(operand_type).kind == SemTypeKind::CallbackWake) {
    const auto plan = state.low_ir.callbackWakePlanFor(operand_type);
    state.values[semantic_id.index] = emit(
        state, current,
        LowCallbackWakeWait{semantic.type, plan,
                            state.low_ir.addValueBlock(operands)},
        semantic_id);
  } else {
    const auto plan = state.low_ir.callbackCompletionPlanFor(operand_type);
    state.values[semantic_id.index] = emit(
        state, current,
        LowCallbackCompletionWait{semantic.type, plan,
                                  state.low_ir.addValueBlock(operands)},
        semantic_id);
  }
}

void LoweringInteropService::makeCallbackAdapter(
    InstId semantic_id, SemMakeCallbackAdapter semantic, LowBlockId current,
    std::span<const LowInstId> fields, LoweringExpressionState &state) {
  const auto block = state.low_ir.addValueBlock(fields);
  state.values[semantic_id.index] =
      emit(state, current, LowMakeCallbackAdapter{semantic.type, block, {}},
           semantic_id);
}

void LoweringInteropService::foreignOperationProjection(
    InstId semantic_id, FunctionRefId target, InstBlockId operands,
    TypeId result_type, LowBlockId current,
    ForeignOperationProjectionKind kind,
    ForeignOperationProjectionState &state) {
  const auto &sem_ir = state.session.sem_ir;
  auto &low_ir = state.session.low_ir;
  const auto &reference = sem_ir.functionRef(target);
  const auto *operation =
      reference.local_function.hasValue()
          ? (sem_ir.functionDeclaration(reference.local_function)
                     .interop_artifact
                 ? sem_ir.importIRs().interopRegistry().resolve(
                       *sem_ir.functionDeclaration(reference.local_function)
                            .interop_artifact)
                 : nullptr)
          : [&]() -> const interop::ForeignOperationArtifact * {
              const auto *entity =
                  sem_ir.importIRs().tryGetEntity(reference.public_entity);
              return entity && entity->interop_artifact
                         ? sem_ir.importIRs().interopRegistry().resolve(
                               *entity->interop_artifact)
                         : nullptr;
            }();
  std::vector<LowInstId> lowered_operands;
  for (const auto operand : sem_ir.instBlock(operands))
    lowered_operands.push_back(state.value_for(operand));

  auto plan = operation
                  ? low_ir.foreignOperationCompletionPlanFor(
                        operation->fingerprint)
                  : ForeignOperationCompletionPlanId::invalid();
  if (operation && !plan.hasValue()) {
    const auto operation_id = operationPlan(low_ir, *operation);
    const auto completion_carrier =
        lowered_operands.empty()
            ? result_type
            : TypeId(low_ir.inst(lowered_operands.front()).type);
    plan = completionPlan(low_ir, operation_id, *operation,
                          completion_carrier);
  }
  const auto block = low_ir.addValueBlock(lowered_operands);
  state.values[semantic_id.index] =
      state.emit(current, result_type, semantic_id, plan, block, kind);
}

void LoweringInteropService::foreignOperationCall(
    InstId semantic_id, SemForeignOperationCall semantic, LowBlockId current,
    ForeignOperationCallState &state) {
    // The operation call uses the same verified ABI call machinery as a
    // direct foreign call; its distinct SemIR kind prevents the operation
    // metadata from being mistaken for a plain scalar fast path.
    const auto &reference = state.session.sem_ir.functionRef(semantic.arg0);
    const auto *operation =
        reference.local_function.hasValue()
            ? (state.session.sem_ir.functionDeclaration(reference.local_function)
                       .interop_artifact
                   ? state.session.sem_ir.importIRs().interopRegistry().resolve(
                         *state.session.sem_ir.functionDeclaration(reference.local_function)
                              .interop_artifact)
                   : nullptr)
            : [&]() -> const interop::ForeignOperationArtifact * {
      const auto *entity =
          state.session.sem_ir.importIRs().tryGetEntity(reference.public_entity);
      return entity && entity->interop_artifact
                 ? state.session.sem_ir.importIRs().interopRegistry().resolve(
                       *entity->interop_artifact)
                 : nullptr;
    }();
    if (operation) {
      const auto operation_plan = operationPlan(
          state.session.low_ir, *operation);

      // A callback capability is one logical port, but its C ABI is the
      // canonical entry/context/release triple. Derive all three reverse
      // thunks from the callback adapter fields; LLVM consumes only this
      // source-independent plan.
      const auto callback_port =
          std::ranges::find_if(operation->ports, [](const auto &port) {
            return port.kind == interop::ForeignPortKind::Callback;
          });
      if (callback_port != operation->ports.end()) {
        ForeignOperationCallbackPlan callback_plan;
        callback_plan.operation = operation_plan;
        callback_plan.authority = static_cast<CallbackReleaseAuthority>(
            operation->callback_authority);
        if (callback_port->lanes.size() == callback_plan.lanes.size())
          std::ranges::copy(callback_port->lanes, callback_plan.lanes.begin());

        const auto semantic_arguments = state.session.sem_ir.instBlock(semantic.arg1);
        const auto callback_argument =
            std::ranges::find_if(semantic_arguments, [&](InstId argument) {
              return state.session.sem_ir.type(TypeId(state.session.sem_ir.inst(argument).type)).kind ==
                     SemTypeKind::CallbackAdapter;
            });
        bool callback_plan_valid =
            callback_argument != semantic_arguments.end();
        if (callback_plan_valid) {
          const auto adapter_type =
              TypeId(state.session.sem_ir.inst(*callback_argument).type);
          callback_plan.adapter = state.session.low_ir.callbackAdapterPlanFor(adapter_type);
          callback_plan_valid = callback_plan.adapter.hasValue();
          const auto adapter_fields =
              state.session.sem_ir.inst(*callback_argument).kind ==
                      SemInstKind::MakeCallbackAdapter
                  ? state.session.sem_ir.instBlock(
                        InstBlockId(state.session.sem_ir.inst(*callback_argument).arg0))
                  : std::span<const InstId>{};
          const auto callback_types =
              state.session.sem_ir.typeBlock(TypeBlockId(state.session.sem_ir.type(adapter_type).arg0));
          if (adapter_fields.size() == 3 && callback_types.size() == 3) {
            for (std::size_t index = 0; index < 3; ++index) {
              if (state.session.sem_ir.inst(adapter_fields[index]).kind !=
                  SemInstKind::CallbackAdapter)
                continue;
              const auto source =
                  state.session.sem_ir.getAs<SemCallbackAdapter>(adapter_fields[index]).arg0;
              std::string thunk_error;
              const auto thunk = state.session.low_ir.addForeignAbiThunkPlan(
                  source, callback_types[index], thunk_error);
              if (!thunk.hasValue())
                callback_plan_valid = false;
              if (index == 0)
                callback_plan.entry_thunk = thunk;
              else if (index == 1)
                callback_plan.context_thunk = thunk;
              else
                callback_plan.release_thunk = thunk;
}
          } else {
            callback_plan_valid = false;
          }
        }
        // A bind wrapper may project an already-materialized adapter into
        // entry/context/release lanes. In that boundary there is no static
        // reverse-target source to thunk again; the adapter plan remains the
        // authority for its fields. Only publish the operation callback plan
        // when all three target-aware thunk queries were actually available.
        if (callback_plan_valid)
          (void)state.session.low_ir.addForeignOperationCallbackPlan(
              std::move(callback_plan));
      }
      // Completion operations share one operation-backed family.  Roles are
      // derived from canonical capability paths, never from resource names.
      if (operation->completion_family !=
          interop::ForeignCompletionFamily::None) {
        (void)completionPlan(
            state.session.low_ir, operation_plan, *operation, semantic.type);
      }
    }
    if (operation &&
        operation->error_extractor !=
            interop::ForeignOperationArtifact::ErrorExtractor::None) {
      std::vector<LowInstId> arguments;
      for (const auto argument : state.session.sem_ir.instBlock(semantic.arg1))
        arguments.push_back(state.value_for(argument));
      std::string error;
      const auto call_layout =
          state.session.low_ir.addForeignAbiCallLayout(semantic.arg0, {}, error);
      const auto shape = state.session.sem_ir.canonicalResultShape(semantic.type);
      const auto outcome_shape =
          shape ? state.session.sem_ir.canonicalReadOutcomeShape(shape->success)
                : std::nullopt;
      const auto &callee_type = state.session.sem_ir.type(reference.local_type);
      ForeignCallOutcomePlan outcome_plan{
          .call_layout = call_layout,
          .raw_result_type = TypeId(callee_type.arg1),
          .error_physical_type =
              operation->outcome_projection ==
                          interop::ForeignOperationArtifact::OutcomeProjection::Fread &&
                      operation->error_extractor ==
                          interop::ForeignOperationArtifact::ErrorExtractor::Errno
                  ? state.session.sem_ir.i32Type()
                  : TypeId(callee_type.arg1),
          .projected_result_type = semantic.type,
          .error_type = shape ? shape->error : TypeId::invalid(),
          .extractor = operation->error_extractor,
          .predicate = operation->error_predicate,
          .success_payload = operation->error_success_payload,
          .intervals = operation->error_intervals,
          .predicate_width = operation->error_predicate_width,
          .predicate_signed = operation->error_predicate_signed,
          .predicate_inverted = operation->error_predicate_inverted,
          .outcome_projection = operation->outcome_projection,
          .outcome_type = outcome_shape ? shape->success : TypeId::invalid(),
          .slice_type = outcome_shape ? outcome_shape->data : TypeId::invalid(),
          .element_type = outcome_shape
                              ? TypeId(state.session.sem_ir.type(outcome_shape->data).arg0)
                              : TypeId::invalid(),
          .outcome_buffer_lane = operation->outcome_buffer_lane,
          .outcome_capacity_lane = operation->outcome_capacity_lane,
          .outcome_count_lane = operation->outcome_count_lane,
          .outcome_context_lane = operation->outcome_context_lane,
          .outcome_size_lane = operation->outcome_size_lane,
          .outcome_eof_symbol = operation->outcome_eof_symbol,
          .outcome_ferror_symbol = operation->outcome_ferror_symbol,
          .outcome_count_type =
              operation->outcome_count_type
                  ? state.session.sem_ir.referencePointee(
                        state.session.low_ir
                            .foreignAbiLayout(
                                state.session.low_ir.foreignAbiCallLayout(call_layout)
                                    .function_layout)
                            .parameters[operation->outcome_count_lane]
                            .semantic_type)
                  : TypeId::invalid(),
          .argument_sources = operation->argument_sources,
          .data_variant = outcome_shape ? outcome_shape->data_variant
                                        : core::AnyId::InvalidIndex,
          .eof_variant = outcome_shape ? outcome_shape->eof_variant
                                       : core::AnyId::InvalidIndex,
          .operation_fingerprint = operation->fingerprint};
      const auto plan =
          state.session.low_ir.addForeignCallOutcomePlan(std::move(outcome_plan));
      state.values[semantic_id.index] = state.emit_foreign_result(
          current, semantic.type, semantic_id, plan,
          state.session.low_ir.addValueBlock(arguments));
      return;
    }
    state.lower_plain_call(semantic_id, semantic.arg0, semantic.arg1,
                        semantic.type, current);
  }

} // namespace chtholly::compiler::internal
