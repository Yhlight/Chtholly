#include "LowerToLowIRInternal.h"

#include <array>
#include <cassert>

namespace chtholly::compiler::internal {
namespace {

class DestroyEmitter {
public:
  explicit DestroyEmitter(LoweringDestroyState &state) : state_(state) {}
  [[nodiscard]] LowBlockId destroyPlace(LowBlockId block_id, InstId origin,
                                       LowPlaceId place) {
    const auto type = state_.session.low_ir.place(place).type;
    const auto representation = state_.session.sem_ir.typeRepresentation(type);
    // Callback registrations carry a target-specific terminal ABI rather
    // than a semantic lifecycle function. Their ordinary place cleanup is
    // lowered by LLVM to unregister plus the appropriate release handoff.
    if (state_.session.sem_ir.type(type).kind == SemTypeKind::CallbackRegistration) {
      const auto plan = state_.session.low_ir.callbackRegistrationPlanFor(type);
      assert(plan.hasValue());
      const std::array operands{
          emit<LowLoad>(block_id, type, origin, state_.session.low_ir.place(place).root)};
      (void)emit<LowFinishCallbackRegistration>(
          block_id, state_.session.sem_ir.voidType(), origin, plan,
          state_.session.low_ir.addValueBlock(operands));
    } else if (state_.session.sem_ir.type(type).kind == SemTypeKind::Nominal &&
               state_.session.sem_ir.nominalType(NominalTypeId(state_.session.sem_ir.type(type).arg0))
                       .kind == NominalKind::ForeignResource) {
      const auto &resource =
          state_.session.sem_ir.nominalType(NominalTypeId(state_.session.sem_ir.type(type).arg0));
      const auto loaded =
          emit<LowLoad>(block_id, type, origin, state_.session.low_ir.place(place).root);
      if (resource.foreign_registration_storage_type.hasValue()) {
        const auto storage = emit<LowForeignResourceUnwrap>(
            block_id, resource.foreign_registration_storage_type, origin,
            loaded);
        const auto plan = state_.session.low_ir.callbackRegistrationPlanFor(
            resource.foreign_registration_storage_type);
        const std::array operands{storage};
        (void)emit<LowFinishCallbackRegistration>(
            block_id, state_.session.sem_ir.voidType(), origin, plan,
            state_.session.low_ir.addValueBlock(operands));
      } else {
        (void)emit<LowFinishForeignResource>(block_id, state_.session.sem_ir.voidType(),
                                             origin, loaded);
      }
    } else if (state_.session.sem_ir.type(type).kind == SemTypeKind::ForeignCompletion) {
      const auto &resource =
          state_.session.sem_ir.nominalType(NominalTypeId(state_.session.sem_ir.type(type).arg0));
      const auto loaded =
          emit<LowLoad>(block_id, type, origin, state_.session.low_ir.place(place).root);
      if (resource.foreign_completion_storage_type.hasValue()) {
        const auto storage = emit<LowForeignResourceUnwrap>(
            block_id, resource.foreign_completion_storage_type, origin, loaded);
        const auto plan = state_.session.low_ir.callbackCompletionPlanFor(
            resource.foreign_completion_storage_type);
        const std::array operands{storage};
        (void)emit<LowFinishCallbackCompletion>(
            block_id, state_.session.sem_ir.voidType(), origin, plan,
            state_.session.low_ir.addValueBlock(operands));
      } else {
        (void)emit<LowFinishForeignCompletion>(block_id, state_.session.sem_ir.voidType(),
                                               origin, loaded);
      }
    } else if (state_.session.sem_ir.type(type).kind == SemTypeKind::ForeignWake) {
      const auto &resource =
          state_.session.sem_ir.nominalType(NominalTypeId(state_.session.sem_ir.type(type).arg0));
      const auto loaded =
          emit<LowLoad>(block_id, type, origin, state_.session.low_ir.place(place).root);
      const auto storage = emit<LowForeignResourceUnwrap>(
          block_id, resource.foreign_wake_storage_type, origin, loaded);
      const auto plan =
          state_.session.low_ir.callbackWakePlanFor(resource.foreign_completion_storage_type);
      const std::array operands{storage};
      (void)emit<LowFinishCallbackWake>(block_id, state_.session.sem_ir.voidType(), origin,
                                        plan, state_.session.low_ir.addValueBlock(operands));
    } else if (state_.session.sem_ir.type(type).kind == SemTypeKind::CallbackCompletion) {
      const auto plan = state_.session.low_ir.callbackCompletionPlanFor(type);
      assert(plan.hasValue());
      const std::array operands{
          emit<LowLoad>(block_id, type, origin, state_.session.low_ir.place(place).root)};
      (void)emit<LowFinishCallbackCompletion>(block_id, state_.session.sem_ir.voidType(),
                                              origin, plan,
                                              state_.session.low_ir.addValueBlock(operands));
    } else if (state_.session.sem_ir.type(type).kind == SemTypeKind::CallbackWake) {
      const auto plan = state_.session.low_ir.callbackWakePlanFor(type);
      assert(plan.hasValue());
      const std::array operands{
          emit<LowLoad>(block_id, type, origin, state_.session.low_ir.place(place).root)};
      (void)emit<LowFinishCallbackWake>(block_id, state_.session.sem_ir.voidType(), origin,
                                        plan, state_.session.low_ir.addValueBlock(operands));
    } else if (state_.session.sem_ir.type(type).kind == SemTypeKind::CoroutineTask) {
      const auto loaded =
          emit<LowLoad>(block_id, type, origin, state_.session.low_ir.place(place).root);
      (void)emit<LowFinishCoroutineTask>(block_id, state_.session.sem_ir.voidType(), origin,
                                         loaded);
    } else if (state_.session.sem_ir.type(type).kind ==
               SemTypeKind::CoroutineTaskCompletion) {
      const auto loaded =
          emit<LowLoad>(block_id, type, origin, state_.session.low_ir.place(place).root);
      (void)emit<LowFinishCoroutineTaskCompletion>(block_id, state_.session.sem_ir.voidType(),
                                                   origin, loaded);
    } else if (state_.session.sem_ir.type(type).kind ==
               SemTypeKind::CoroutineTaskCompletionSet) {
      const auto loaded =
          emit<LowLoad>(block_id, type, origin, state_.session.low_ir.place(place).root);
      (void)emit<LowFinishCoroutineTaskCompletionSet>(
          block_id, state_.session.sem_ir.voidType(), origin, loaded);
    } else if (state_.session.sem_ir.type(type).kind == SemTypeKind::CoroutineTaskSelection) {
      const auto loaded =
          emit<LowLoad>(block_id, type, origin, state_.session.low_ir.place(place).root);
      (void)emit<LowFinishCoroutineTaskSelection>(block_id, state_.session.sem_ir.voidType(),
                                                  origin, loaded);
    } else if (state_.session.sem_ir.type(type).kind == SemTypeKind::CoroutineChecked) {
      const auto loaded =
          emit<LowLoad>(block_id, type, origin, state_.session.low_ir.place(place).root);
      (void)emit<LowFinishCoroutineChecked>(block_id, state_.session.sem_ir.voidType(),
                                            origin, loaded);
    } else if (state_.session.sem_ir.type(type).kind == SemTypeKind::Nominal &&
               representation.destroy == DestroyReprKind::Trivial) {
      const auto object = emit<LowPlaceAddress>(block_id, type, origin, place);
      return destroyValue(block_id, origin, type, object);
    } else if (representation.destroy == DestroyReprKind::Custom) {
      const auto target = lifecycleTarget(type, SemCanonicalFunctionRole::Drop);
      assert(target.hasValue());
      (void)emit<LowLifecycleDestroy>(block_id, state_.session.sem_ir.voidType(), origin,
                                      target, place);
    } else {
      (void)emit<LowDestroy>(block_id, state_.session.sem_ir.voidType(), origin, place);
    }
    return block_id;
  }

  [[nodiscard]] LowBlockId destroyValue(LowBlockId block_id, InstId origin,
                                            TypeId type, LowInstId value) {
    const auto representation = state_.session.sem_ir.typeRepresentation(type);
    if (state_.session.sem_ir.type(type).kind == SemTypeKind::CallbackRegistration) {
      const auto plan = state_.session.low_ir.callbackRegistrationPlanFor(type);
      const std::array operands{value};
      (void)emit<LowFinishCallbackRegistration>(
          block_id, state_.session.sem_ir.voidType(), origin, plan,
          state_.session.low_ir.addValueBlock(operands));
    } else if (state_.session.sem_ir.type(type).kind == SemTypeKind::Nominal &&
               state_.session.sem_ir.nominalType(NominalTypeId(state_.session.sem_ir.type(type).arg0))
                       .kind == NominalKind::ForeignResource) {
      const auto &resource =
          state_.session.sem_ir.nominalType(NominalTypeId(state_.session.sem_ir.type(type).arg0));
      if (resource.foreign_registration_storage_type.hasValue()) {
        const auto storage = emit<LowForeignResourceUnwrap>(
            block_id, resource.foreign_registration_storage_type, origin,
            value);
        const auto plan = state_.session.low_ir.callbackRegistrationPlanFor(
            resource.foreign_registration_storage_type);
        const std::array operands{storage};
        (void)emit<LowFinishCallbackRegistration>(
            block_id, state_.session.sem_ir.voidType(), origin, plan,
            state_.session.low_ir.addValueBlock(operands));
      } else {
        (void)emit<LowFinishForeignResource>(block_id, state_.session.sem_ir.voidType(),
                                             origin, value);
      }
    } else if (state_.session.sem_ir.type(type).kind == SemTypeKind::ForeignCompletion) {
      const auto &resource =
          state_.session.sem_ir.nominalType(NominalTypeId(state_.session.sem_ir.type(type).arg0));
      if (resource.foreign_completion_storage_type.hasValue()) {
        const auto storage = emit<LowForeignResourceUnwrap>(
            block_id, resource.foreign_completion_storage_type, origin, value);
        const auto plan = state_.session.low_ir.callbackCompletionPlanFor(
            resource.foreign_completion_storage_type);
        const std::array operands{storage};
        (void)emit<LowFinishCallbackCompletion>(
            block_id, state_.session.sem_ir.voidType(), origin, plan,
            state_.session.low_ir.addValueBlock(operands));
      } else {
        (void)emit<LowFinishForeignCompletion>(block_id, state_.session.sem_ir.voidType(),
                                               origin, value);
      }
    } else if (state_.session.sem_ir.type(type).kind == SemTypeKind::ForeignWake) {
      const auto &resource =
          state_.session.sem_ir.nominalType(NominalTypeId(state_.session.sem_ir.type(type).arg0));
      const auto storage = emit<LowForeignResourceUnwrap>(
          block_id, resource.foreign_wake_storage_type, origin, value);
      const auto plan =
          state_.session.low_ir.callbackWakePlanFor(resource.foreign_completion_storage_type);
      const std::array operands{storage};
      (void)emit<LowFinishCallbackWake>(block_id, state_.session.sem_ir.voidType(), origin,
                                        plan, state_.session.low_ir.addValueBlock(operands));
    } else if (state_.session.sem_ir.type(type).kind == SemTypeKind::CallbackCompletion) {
      const auto plan = state_.session.low_ir.callbackCompletionPlanFor(type);
      assert(plan.hasValue());
      const std::array operands{value};
      (void)emit<LowFinishCallbackCompletion>(block_id, state_.session.sem_ir.voidType(),
                                              origin, plan,
                                              state_.session.low_ir.addValueBlock(operands));
    } else if (state_.session.sem_ir.type(type).kind == SemTypeKind::CallbackWake) {
      const auto plan = state_.session.low_ir.callbackWakePlanFor(type);
      assert(plan.hasValue());
      const std::array operands{value};
      (void)emit<LowFinishCallbackWake>(block_id, state_.session.sem_ir.voidType(), origin,
                                        plan, state_.session.low_ir.addValueBlock(operands));
    } else if (state_.session.sem_ir.type(type).kind == SemTypeKind::CoroutineTask) {
      (void)emit<LowFinishCoroutineTask>(block_id, state_.session.sem_ir.voidType(), origin,
                                         value);
    } else if (state_.session.sem_ir.type(type).kind ==
               SemTypeKind::CoroutineTaskCompletion) {
      (void)emit<LowFinishCoroutineTaskCompletion>(block_id, state_.session.sem_ir.voidType(),
                                                   origin, value);
    } else if (state_.session.sem_ir.type(type).kind ==
               SemTypeKind::CoroutineTaskCompletionSet) {
      (void)emit<LowFinishCoroutineTaskCompletionSet>(
          block_id, state_.session.sem_ir.voidType(), origin, value);
    } else if (state_.session.sem_ir.type(type).kind == SemTypeKind::CoroutineTaskSelection) {
      (void)emit<LowFinishCoroutineTaskSelection>(block_id, state_.session.sem_ir.voidType(),
                                                  origin, value);
    } else if (state_.session.sem_ir.type(type).kind == SemTypeKind::CoroutineChecked) {
      (void)emit<LowFinishCoroutineChecked>(block_id, state_.session.sem_ir.voidType(),
                                            origin, value);
    } else if (state_.session.sem_ir.type(type).kind == SemTypeKind::Nominal &&
               representation.destroy == DestroyReprKind::Trivial) {
      const auto &nominal =
          state_.session.sem_ir.nominalType(NominalTypeId(state_.session.sem_ir.type(type).arg0));
      if (nominal.kind == NominalKind::Enum) {
        const auto tag =
            emit<LowEnumTag>(block_id, state_.session.sem_ir.i32Type(), origin, value);
        const auto done = newBlock();
        auto dispatch = block_id;
        for (std::uint32_t variant = 0; variant < nominal.variants.size();
             ++variant) {
          const auto active = newBlock();
          const auto next =
              variant + 1 == nominal.variants.size() ? done : newBlock();
          const auto expected_id =
              state_.session.sem_ir.addInteger(nominal.variants[variant].discriminant);
          const auto expected = emit<LowIntegerConstant>(
              dispatch, state_.session.sem_ir.i32Type(), origin, expected_id);
          const auto matches = emit<LowEqual>(dispatch, state_.session.sem_ir.boolType(),
                                              origin, tag, expected);
          (void)emit<LowBranchIf>(dispatch, state_.session.sem_ir.voidType(), origin, matches,
                                  state_.session.low_ir.addTargets({active, next}));
          auto active_tail = active;
          for (std::size_t field = nominal.variants[variant].fields.size();
               field != 0; --field) {
            const auto field_index = static_cast<std::uint32_t>(field - 1);
            const auto field_type =
                state_.session.sem_ir.enumPayloadFieldType(type, variant, field_index);
            if (state_.session.sem_ir.typeRepresentation(field_type).destroy ==
                DestroyReprKind::None)
              continue;
            const auto encoded =
                (static_cast<std::uint64_t>(variant) << 32U) | field_index;
            const auto projection =
                state_.session.sem_ir.addInteger(static_cast<std::int64_t>(encoded));
            const auto payload = emit<LowEnumPayload>(
                active_tail, field_type, origin, value, projection);
            active_tail =
                destroyValue(active_tail, origin, field_type, payload);
          }
          (void)emit<LowBranch>(active_tail, state_.session.sem_ir.voidType(), origin, done);
          dispatch = next;
        }
        return done;
      }
      const auto &object = state_.session.low_ir.typeRepresentation(type);
      for (std::size_t index = object.field_projections.size(); index != 0;
           --index) {
        const auto field_index = static_cast<std::uint32_t>(index - 1);
        const auto field_type = state_.session.sem_ir.nominalFieldType(type, field_index);
        if (state_.session.sem_ir.typeRepresentation(field_type).destroy ==
            DestroyReprKind::None)
          continue;
        const auto field = emit<LowProjectionLoad>(
            block_id, field_type, origin, value, FieldIndex(field_index));
        block_id = destroyValue(block_id, origin, field_type, field);
      }
    } else if (representation.destroy == DestroyReprKind::Custom) {
      const auto target = lifecycleTarget(type, SemCanonicalFunctionRole::Drop);
      assert(target.hasValue());
      (void)emit<LowLifecycleDestroyValue>(block_id, state_.session.sem_ir.voidType(), origin,
                                           target, value);
    } else {
      (void)emit<LowDestroyValue>(block_id, state_.session.sem_ir.voidType(), origin, value);
    }
    return block_id;
  }

private:
  template <typename InstT>
  [[nodiscard]] LowInstId
  emit(LowBlockId block_id, TypeId type, InstId origin,
       typename InstT::Arg0Type arg0 = typename InstT::Arg0Type{},
       typename InstT::Arg1Type arg1 = typename InstT::Arg1Type{}) {
    const auto id = state_.session.low_ir.addInst(
        InstT{type, arg0, arg1}, origin);
    state_.pending_blocks[block_id.index - state_.session.low_ir.blockCount()]
        .push_back(id);
    return id;
  }

  [[nodiscard]] LowBlockId newBlock() {
    const auto id = LowBlockId(static_cast<std::uint32_t>(
        state_.session.low_ir.blockCount() + state_.pending_blocks.size()));
    state_.pending_blocks.emplace_back();
    return id;
  }

  [[nodiscard]] FunctionRefId lifecycleTarget(
      TypeId type, SemCanonicalFunctionRole role) const {
    return LoweringOwnershipService::lifecycleTarget(state_.session, type,
                                                       role);
  }

  LoweringDestroyState &state_;
};

} // namespace

LowBlockId LoweringDestroyService::place(LowBlockId block, InstId origin,
                                         LowPlaceId place,
                                         LoweringDestroyState &state) {
  return DestroyEmitter(state).destroyPlace(block, origin, place);
}

LowBlockId LoweringDestroyService::value(LowBlockId block, InstId origin,
                                         TypeId type, LowInstId value,
                                         LoweringDestroyState &state) {
  return DestroyEmitter(state).destroyValue(block, origin, type, value);
}

} // namespace chtholly::compiler::internal