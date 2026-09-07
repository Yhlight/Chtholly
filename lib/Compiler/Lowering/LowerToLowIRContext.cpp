#include "chtholly/Compiler/LowerToLowIR.h"

#include "LowerToLowIRInternal.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace chtholly::compiler {
namespace {

class SemToLowContext {
public:
  SemToLowContext(
      const SemIR &sem_ir, core::Arena &arena,
      std::string_view normalized_target_triple,
      std::span<const NominalTypeLayoutArtifact> nominal_layouts,
      std::span<const LowNominalLayoutBinding> nominal_layout_bindings)
      : session_state_(sem_ir, arena, normalized_target_triple,
                       nominal_layouts, nominal_layout_bindings),
        expression_state_{session_state_.sem_ir, session_state_.low_ir,
                          pending_blocks_, values_, write_only_values_, take_values_,
                          borrow_values_, borrow_places_,
                          [this](InstId id) { return valueFor(id); }},
        aggregate_state_{
            session_state_.sem_ir, session_state_.low_ir, pending_blocks_,
            values_, [this](InstId id) { return valueFor(id); },
            [this](LowBlockId block, TypeId type, InstId origin,
                   LowInstId object) {
              return packValue(block, type, origin, object);
            }},
        place_state_{
            session_state_.sem_ir,
            session_state_.low_ir,
            pending_blocks_,
            values_,
            borrow_values_,
            borrow_places_,
            take_values_,
            function_state_.current_function,
            [this](InstId id) { return valueFor(id); },
            [this](LocalId local) { return slotFor(local); },
            [this](SemPlaceId place) { return placeFor(place); },
            [this](const SemPlace &place) { return lowerPlace(place); },
            [this](TypeId type) { return hasConversion(type); },
            [this](TypeId type) { return isRepresentationObjectType(type); },
            [this](LowBlockId block, TypeId type, InstId origin,
                   LowInstId object) {
              return packValue(block, type, origin, object);
            },
            [this](LowBlockId block, TypeId type, InstId origin,
                   LowInstId value) {
              return unpackValue(block, type, origin, value);
            },
            [this](TypeId type, SemCanonicalFunctionRole role) {
              return lifecycleTarget(type, role);
            }},
        control_state_(pending_blocks_, next_parameter_),
        loop_state_{
            session_state_, pending_blocks_, function_slots_, values_,
            loop_targets_, [this]() { return active_task_scopes_.size(); },
            [this]() { return newBlock(); },
            [this](InstBlockId block, LowBlockId &entry) {
              lowerBlock(block, entry);
            },
            [this](LowBlockId block) { return isTerminated(block); },
            [this](LowBlockId block, InstId origin,
                   const PlaceCleanupPlan &cleanup) {
              return emitCleanup(block, origin, cleanup);
            },
            [this](const PlaceCleanupPlan &cleanup, InstId origin,
                   LowBlockId successor) {
              return cleanupTail(cleanup, origin, successor);
            },
            [this](const PlaceCleanupPlan &cleanup) {
              return hasCustomCleanup(cleanup);
            },
            [this](LowBlockId block, std::size_t depth,
                   CoroutineTaskGroupExitIntent intent) {
              return emitTaskScopeDrains(block, depth, intent);
            },
            [this](InstId id) { return valueFor(id); }},
        ownership_state_(function_slots_, slots_, places_, root_places_),
        binding_state_{
            session_state_, values_,
            [this](InstId id) { return valueFor(id); },
            [this](LocalId local) { return slotFor(local); },
            [this](SlotId slot) { return rootPlaceFor(slot); },
            [this](SemPlaceId place) { return placeFor(place); },
            [this](FunctionRefId target) { return isConstructor(target); },
            [this](FunctionRefId target, TypeId destination) {
              return isInfallibleConstructorFor(target, destination);
            },
            [this](InstId id) { return expressionCategory(session_state_.sem_ir, id); },
            [this](LowBlockId block, InstId origin,
                   std::span<const PlaceCleanupAction> actions) {
              return emitCleanupActions(block, origin, actions);
            },
            [this](LowBlockId block, TypeId type, InstId origin) {
              return emit<LowVoidValue>(block, type, origin);
            },
            [this](LowBlockId block, TypeId type, InstId origin, SlotId slot) {
              return emit<LowBorrow>(block, type, origin, slot);
            },
            [this](LowBlockId block, TypeId type, InstId origin,
                   ConstructPlanId plan) {
              return emit<LowConstruct>(block, type, origin, plan);
            },
            [this](LowBlockId block, TypeId type, InstId origin, SlotId slot,
                   LowInstId value) {
              return emit<LowInitializeFromValue>(block, type, origin, slot,
                                                  value);
            },
            [this](LowBlockId block, TypeId type, InstId origin, SlotId slot,
                   LowInstId value) {
              return emit<LowTransfer>(block, type, origin, slot, value);
            },
            [this](LowBlockId block, TypeId type, InstId origin, SlotId slot,
                   LowInstId value) {
              return emit<LowInitialize>(block, type, origin, slot, value);
            },
            [this](LowBlockId block, TypeId type, InstId origin,
                   LowValueBlockId operands, FieldIndex field) {
              return emit<LowProjectionStore>(block, type, origin, operands,
                                               field);
            },
            [this](LowBlockId block, TypeId type, InstId origin,
                   LowValueBlockId operands, FieldIndex field) {
              return emit<LowProjectionInit>(block, type, origin, operands,
                                              field);
            },
            [this](LowBlockId block, TypeId type, InstId origin,
                   LowPlaceId place) {
              return emit<LowMarkInitialized>(block, type, origin, place);
            },
            [this](LowBlockId block, TypeId type, InstId origin,
                   LowPlaceId place, LowInstId value) {
              return emit<LowInitializePlaceFromValue>(block, type, origin,
                                                        place, value);
            },
            [this](LowBlockId block, TypeId type, InstId origin,
                   LowPlaceId place, LowInstId value) {
              return emit<LowInitializePlace>(block, type, origin, place,
                                              value);
            },
            [this](std::span<const LowInstId> values) {
              return session_state_.low_ir.addValueBlock(values);
            },
            [this](LowBlockId block, TypeId type, InstId origin, LowValueBlockId operands, const SemPlace &base) {
              return emit<LowIndexStore>(block, type, origin, operands, lowerPlace(base));
            }},
        destroy_state_{session_state_, pending_blocks_},
        coroutine_state_(suspension_pre_cleanup_,
                         suspension_transferred_cleanup_, cancellation_cleanup_),
        interop_state_{session_state_.sem_ir, session_state_.low_ir},
        cleanup_state_{
            session_state_.sem_ir,
            [this](InstBlockId block_id, LowBlockId &current) {
              lowerBlock(block_id, current);
            },
            [this](LowBlockId block_id) { return isTerminated(block_id); },
            [this]() { return newBlock(); },
            [this](SemPlaceId place) { return placeFor(place); },
            [this](LocalId local) { return slotFor(local); },
            [this](LowBlockId block_id, InstId origin, SlotId slot) {
              (void)emit<LowEndLifetime>(
                  block_id, session_state_.sem_ir.voidType(), origin, slot);
            },
            [this](LowBlockId block_id, TypeId type, InstId origin,
                   LowPlaceId place) {
              return emit<LowIsInitialized>(block_id, type, origin, place);
            },
            [this](LowBlockId block_id, InstId origin, LowInstId condition,
                   LowBlockId true_block, LowBlockId false_block) {
              (void)emit<LowBranchIf>(
                  block_id, session_state_.sem_ir.voidType(), origin, condition,
                  session_state_.low_ir.addTargets({true_block, false_block}));
            },
            [this](LowBlockId block_id, InstId origin, LowBlockId target) {
              (void)emit<LowBranch>(block_id,
                                    session_state_.sem_ir.voidType(), origin,
                                    target);
            },
            [this](LowBlockId block_id, InstId origin,
                   const PlaceCleanupAction &action) {
              return emitCleanupAction(block_id, origin, action);
            },
            // Action-level callbacks keep all instruction construction in the
            // active SemToLowContext while the cleanup service owns ordering.
            [this](SemPlaceId place) { return computedProjectionCleanup(place); },
            [this](LowBlockId block_id, TypeId type, InstId origin,
                   LowPlaceId place) {
              return emit<LowPlaceAddress>(block_id, type, origin, place);
            },
            [this](LowBlockId block_id, TypeId type, InstId origin,
                   LowInstId owner, FieldIndex field) {
              return emit<LowProjectionTake>(block_id, type, origin, owner,
                                             field);
            },
            [this](LowPlaceId place) {
              return session_state_.low_ir.place(place).type;
            },
            [this](LowBlockId block_id, InstId origin, LowPlaceId place) {
              (void)emit<LowMarkMoved>(block_id,
                                       session_state_.sem_ir.voidType(), origin,
                                       place);
            },
            [this](LowBlockId block_id, InstId origin, TypeId type,
                   LowInstId value) {
              return emitDestroyValue(block_id, origin, type, value);
            },
            [this](LowBlockId block_id, InstId origin, LowPlaceId place) {
              return emitDestroy(block_id, origin, place);
            }},
        coroutine_instruction_state_{session_state_, pending_blocks_, values_,
                                     function_slots_, function_state_},
        coroutine_frame_state_{
            session_state_, suspension_pre_cleanup_,
            suspension_transferred_cleanup_, cancellation_cleanup_,
            scope_protected_suspensions_,
            [this](SlotId slot) { return rootPlaceFor(slot); }},
        coroutine_scope_state_{
            coroutine_instruction_state_, active_task_scopes_,
            scope_protected_suspensions_,
            [this](InstBlockId block_id, LowBlockId &current) {
              lowerBlock(block_id, current);
            },
            [this](LowBlockId block_id) { return isTerminated(block_id); },
            [this](LowBlockId block_id, InstId origin,
                   const PlaceCleanupPlan &cleanup) {
              return emitCleanup(block_id, origin, cleanup);
            }} {}

  LowIR run();

private:
  [[nodiscard]] const SemInst &semanticInst(SemIRInstRef ref) const;
  [[nodiscard]] const SemType &semanticType(SemIRTypeRef ref) const;
  [[nodiscard]] const SemFunction &
  semanticFunction(SemIRFunctionRef ref) const;
  [[nodiscard]] SemIRInstRef semanticInstRef(InstId id) const {
    return makeSemIRRef(session_state_.sem_ir, id);
  }
  [[nodiscard]] SemIRTypeRef semanticTypeRef(TypeId id) const {
    return makeSemIRRef(session_state_.sem_ir, id);
  }

  [[nodiscard]] LowBlockId newBlock();

  [[nodiscard]] std::vector<LowInstId> &block(LowBlockId id) {
    return pending_blocks_[id.index - session_state_.low_ir.blockCount()];
  }

  template <typename InstT>
  [[nodiscard]] LowInstId
  emit(LowBlockId block_id, TypeId type, InstId origin,
       typename InstT::Arg0Type arg0 = typename InstT::Arg0Type{},
       typename InstT::Arg1Type arg1 = typename InstT::Arg1Type{}) {
    const auto id = session_state_.low_ir.addInst(InstT{type, arg0, arg1}, origin);
    block(block_id).push_back(id);
    return id;
  }

  [[nodiscard]] bool isTerminated(LowBlockId block_id) const;

  [[nodiscard]] bool semanticConditionIsAlwaysTrue(InstBlockId block_id) const;

  [[nodiscard]] InstId
  semanticBlockResult(std::span<const InstId> instructions) const;

  [[nodiscard]] bool
  semanticBlockContainsCurrentLoopBreak(InstBlockId block_id) const;
  [[nodiscard]] bool
  semanticBlockContainsLoopBreak(InstBlockId block_id,
                                 std::uint32_t nested_loops) const;

  [[nodiscard]] SlotId slotFor(LocalId local);

  [[nodiscard]] LowPlaceId placeFor(SemPlaceId semantic_place) {
    if (const auto found = places_.find(semantic_place.index);
        found != places_.end())
      return found->second;
    const auto &place = session_state_.sem_ir.placeStates().place(semantic_place);
    const auto result = lowerPlace(place);
    places_.emplace(semantic_place.index, result);
    return result;
  }

  [[nodiscard]] LowPlaceId rootPlaceFor(SlotId slot);

  [[nodiscard]] LowPlaceId lowerPlace(const SemPlace &place);

  [[nodiscard]] std::optional<internal::LoweringOwnershipService::ComputedProjectionCleanup>
  computedProjectionCleanup(SemPlaceId semantic_place);

  [[nodiscard]] LowInstId valueFor(InstId semantic_id) const {
    const auto found = values_.find(semantic_id.index);
    assert(found != values_.end());
    return found->second;
  }

  [[nodiscard]] bool isConstructor(FunctionRefId target) const;

  [[nodiscard]] bool isInfallibleConstructorFor(FunctionRefId target,
                                                TypeId destination) const;

  [[nodiscard]] bool isDirectPlacementCall(InstId call, InstId consumer) const;

  [[nodiscard]] bool hasConversion(TypeId type) const;

  [[nodiscard]] bool isRepresentationObjectType(TypeId type) const;

  [[nodiscard]] bool isRepresentationPack() const;

  [[nodiscard]] LowInstId packValue(LowBlockId current, TypeId type,
                                    InstId origin, LowInstId object) {
    return hasConversion(type)
               ? emit<LowPackValue>(current, type, origin, object)
               : object;
  }

  [[nodiscard]] LowInstId unpackValue(LowBlockId current, TypeId type,
                                      InstId origin, LowInstId value) {
    return hasConversion(type)
               ? emit<LowUnpackValue>(current, type, origin, value)
               : value;
  }

  [[nodiscard]] FunctionRefId lifecycleTarget(TypeId type,
                                               SemCanonicalFunctionRole role) const;

  [[nodiscard]] LowBlockId emitDestroy(LowBlockId block_id, InstId origin,
                                       LowPlaceId place) {
    return internal::LoweringDestroyService::place(
        block_id, origin, place, destroy_state_);
  }

  [[nodiscard]] LowBlockId emitDestroyValue(LowBlockId block_id, InstId origin,
                                            TypeId type, LowInstId value) {
    return internal::LoweringDestroyService::value(
        block_id, origin, type, value, destroy_state_);
  }
  [[nodiscard]] LowBlockId emitCleanupAction(LowBlockId block_id, InstId origin,
                                             const PlaceCleanupAction &action) {
    return internal::LoweringCleanupService::action(action, block_id, origin,
                                                    cleanup_state_);
  }

  [[nodiscard]] LowBlockId emitCleanup(LowBlockId block_id, InstId origin,
                                       const PlaceCleanupPlan &plan) {
    return internal::LoweringCleanupService::emit(plan, block_id, origin,
                                                  cleanup_state_);
  }

  [[nodiscard]] LowBlockId
  emitCleanupActions(LowBlockId block_id, InstId origin,
                     std::span<const PlaceCleanupAction> actions) {
    PlaceCleanupPlan plan;
    plan.actions.assign(actions.begin(), actions.end());
    return emitCleanup(block_id, origin, plan);
  }

  [[nodiscard]] bool hasCustomCleanup(const PlaceCleanupPlan &plan) const {
    return internal::LoweringCleanupService::hasCustomCleanup(
        session_state_.sem_ir, plan);
  }

  void collectWriteOnlyValues(InstBlockId block_id);

  [[nodiscard]] LowBlockId cleanupTail(const PlaceCleanupPlan &plan,
                                       InstId origin, LowBlockId successor);

#include "LowerToLowIRContextExpression.inc"

#include "LowerToLowIRContextInterop.inc"

#include "LowerToLowIRContextCoroutine.inc"

#include "LowerToLowIRContextBinding.inc"
#include "LowerToLowIRContextControl.inc"

  [[nodiscard]] LowFunctionId lowerFunction(FunctionId function_id) {
    internal::LoweringFunctionService::State state{
        session_state_, function_state_, pending_blocks_, pending_cleanup_graphs_,
        suspension_pre_cleanup_, suspension_transferred_cleanup_,
        cancellation_cleanup_, loop_targets_, active_task_scopes_,
        scope_protected_suspensions_, cleanup_tails_, values_, slots_, places_,
        write_only_values_, take_values_, borrow_values_, borrow_places_,
        function_slots_, next_parameter_,
        [this]() { return newBlock(); },
        [this](InstBlockId block, LowBlockId &entry) {
          lowerBlock(block, entry);
        },
        [this](InstBlockId block) { collectWriteOnlyValues(block); },
        [this]() { return isRepresentationPack(); },
        [this](TypeId type) { return hasConversion(type); },
        [this](LowBlockId block, InstId origin,
               const PlaceCleanupPlan &cleanup) {
          return emitCleanup(block, origin, cleanup);
        },
        [this](const PlaceCleanupPlan &cleanup, InstId origin,
               LowBlockId successor) {
          return cleanupTail(cleanup, origin, successor);
        },
        [this](const PlaceCleanupPlan &plan, InstId origin,
               internal::LoweringPendingCleanupGraph::Role role) {
          addPendingCleanupGraph(plan, origin, role);
        },
        [this](LowBlockId block, TypeId type, InstId origin, LowInstId value) {
          return emit<LowTransferReturn>(block, type, origin, value);
        },
        [this](LowBlockId block, TypeId type, InstId origin, SlotId slot,
               LowInstId value) {
          return emit<LowInitialize>(block, type, origin, slot, value);
        },
        [this](LowBlockId block, TypeId type, InstId origin, SlotId slot) {
          return emit<LowLoad>(block, type, origin, slot);
        },
        [this](LowBlockId block, TypeId type, InstId origin, LowInstId value) {
          return emit<LowReturn>(block, type, origin, value);
        },
        [this](LowBlockId block, TypeId type, InstId origin) {
          return emit<LowUnreachable>(block, type, origin);
        },
        [this](LowBlockId block, TypeId type, InstId origin) {
          return emit<LowVoidValue>(block, type, origin);
        },
        [this](LowBlockId block, TypeId type, InstId origin) {
          return emit<LowReturnInPlace>(block, type, origin);
        },
        [this](LowBlockId block, TypeId type, InstId origin) {
          return emit<LowCoroutineCleanupEnd>(block, type, origin);
        }};
    return internal::LoweringFunctionService::lower(function_id, state);
  }
  using PendingCleanupGraph = internal::LoweringPendingCleanupGraph;

  void addPendingCleanupGraph(const PlaceCleanupPlan &plan, InstId origin,
                              PendingCleanupGraph::Role role) {
    if (plan.actions.empty())
      return;
    cleanup_tails_.clear();
    const auto begin = pending_blocks_.size();
    const auto slot_begin = function_slots_.size();
    const auto entry = newBlock();
    const auto tail = emitCleanup(entry, origin, plan);
    (void)emit<LowCoroutineCleanupEnd>(tail, session_state_.sem_ir.voidType(), origin);
    pending_cleanup_graphs_.push_back({
        origin,
        entry,
        begin,
        pending_blocks_.size(),
        role,
        std::vector<SlotId>(function_slots_.begin() + slot_begin,
                            function_slots_.end()),
    });
  }

  [[nodiscard]] SlotId wakeSlotFor(LowInstId value) const;

  void buildCoroutineFramePlan(FunctionId function,
                               LowFunctionId low_function) {
    internal::LoweringCoroutineFrameService::build(
        function, low_function, coroutine_frame_state_);
  }
  internal::LoweringSessionState session_state_;
  std::vector<std::vector<LowInstId>> pending_blocks_;
  std::vector<PendingCleanupGraph> pending_cleanup_graphs_;
  std::unordered_map<std::uint32_t, CoroutineCleanupGraphId>
      suspension_pre_cleanup_;
  std::unordered_map<std::uint32_t, CoroutineCleanupGraphId>
      suspension_transferred_cleanup_;
  std::unordered_map<std::uint32_t, CoroutineCleanupGraphId>
      cancellation_cleanup_;
  using LoopTargets = internal::LoweringLoopTarget;
  std::vector<LoopTargets> loop_targets_;
  std::vector<internal::LoweringActiveTaskScope> active_task_scopes_;
  std::unordered_set<std::uint32_t> scope_protected_suspensions_;
  std::unordered_map<std::string, LowBlockId> cleanup_tails_;
  std::unordered_map<std::uint32_t, LowInstId> values_;
  std::unordered_map<std::uint32_t, SlotId> slots_;
  std::unordered_map<std::uint32_t, LowPlaceId> places_;
  std::unordered_map<std::uint32_t, LowPlaceId> root_places_;
  std::unordered_set<std::uint32_t> write_only_values_;
  std::unordered_set<std::uint32_t> take_values_;
  std::unordered_map<std::uint32_t, TypeId> borrow_values_;
  std::unordered_map<std::uint32_t, SemPlaceId> borrow_places_;
  std::vector<SlotId> function_slots_;
  std::uint32_t next_parameter_ = 0;
  internal::LoweringFunctionState function_state_;
  internal::LoweringExpressionState expression_state_;
  internal::LoweringAggregateState aggregate_state_;
  internal::LoweringPlaceState place_state_;
  internal::LoweringControlState control_state_;
  internal::LoweringLoopState loop_state_;
  internal::LoweringOwnershipState ownership_state_;
  internal::LoweringBindingState binding_state_;
  internal::LoweringDestroyState destroy_state_;
  internal::LoweringCoroutineState coroutine_state_;
  internal::LoweringInteropState interop_state_;
  internal::LoweringCleanupService::State cleanup_state_;
  internal::LoweringCoroutineInstructionState coroutine_instruction_state_;
  internal::LoweringCoroutineFrameState coroutine_frame_state_;
  internal::LoweringCoroutineScopeState coroutine_scope_state_;
};

const SemInst &SemToLowContext::semanticInst(SemIRInstRef ref) const {
  return session_state_.sem_ir.inst(ref.checked(session_state_.sem_ir));
}

bool SemToLowContext::isConstructor(FunctionRefId target) const {
  return internal::LoweringExpressionService::isConstructor(
      session_state_.sem_ir, target);
}

bool SemToLowContext::isInfallibleConstructorFor(FunctionRefId target,
                                                 TypeId destination) const {
  return internal::LoweringExpressionService::isInfallibleConstructorFor(
      session_state_.sem_ir, target, destination);
}

bool SemToLowContext::hasConversion(TypeId type) const {
  return internal::LoweringExpressionService::hasConversion(
      session_state_.sem_ir, type);
}

bool SemToLowContext::isRepresentationObjectType(TypeId type) const {
  return function_state_.current_function.hasValue() &&
         internal::LoweringExpressionService::isRepresentationObjectType(
             session_state_.sem_ir, function_state_.current_function, type);
}

bool SemToLowContext::isRepresentationPack() const {
  return function_state_.current_function.hasValue() &&
         internal::LoweringExpressionService::isRepresentationPack(
             session_state_.sem_ir, function_state_.current_function);
}

void SemToLowContext::collectWriteOnlyValues(InstBlockId block_id) {
  internal::LoweringExpressionService::collectWriteOnlyValues(block_id,
                                                              expression_state_);
}

InstId SemToLowContext::semanticBlockResult(
    std::span<const InstId> instructions) const {
  return internal::LoweringControlService::semanticBlockResult(
      session_state_.sem_ir, instructions);
}

bool SemToLowContext::semanticConditionIsAlwaysTrue(InstBlockId block_id) const {
  return internal::LoweringControlService::semanticConditionIsAlwaysTrue(
      session_state_.sem_ir, block_id);
}

bool SemToLowContext::semanticBlockContainsCurrentLoopBreak(
    InstBlockId block_id) const {
  return semanticBlockContainsLoopBreak(block_id, 0);
}

bool SemToLowContext::semanticBlockContainsLoopBreak(
    InstBlockId block_id, std::uint32_t nested_loops) const {
  return internal::LoweringControlService::semanticBlockContainsLoopBreak(
      session_state_.sem_ir, block_id, nested_loops);
}

bool SemToLowContext::isDirectPlacementCall(InstId call, InstId consumer) const {
  return internal::LoweringControlService::isDirectPlacementCall(
      session_state_.sem_ir, call, consumer);
}

LowPlaceId SemToLowContext::lowerPlace(const SemPlace &place) {
  return internal::LoweringOwnershipService::lowerPlace(
      place, session_state_, ownership_state_,
      [this](TypeId id) -> const SemType & {
        return semanticType(semanticTypeRef(id));
      });
}

FunctionRefId SemToLowContext::lifecycleTarget(
    TypeId type, SemCanonicalFunctionRole role) const {
  return internal::LoweringOwnershipService::lifecycleTarget(
      session_state_, type, role);
}

std::optional<internal::LoweringOwnershipService::ComputedProjectionCleanup>
SemToLowContext::computedProjectionCleanup(SemPlaceId semantic_place) {
  return internal::LoweringOwnershipService::computedProjectionCleanup(
      semantic_place, session_state_,
      [this](TypeId id) -> const SemType & {
        return semanticType(semanticTypeRef(id));
      },
      [this](const SemPlace &place) { return lowerPlace(place); });
}

LowInstId SemToLowContext::lowerConstant(ConstantId constant,
                                          InstId semantic_id,
                                          LowBlockId &current) {
  return internal::LoweringExpressionService::lowerConstant(
      constant, semantic_id, current, expression_state_,
      [this](LowBlockId block, TypeId type, InstId origin, LowInstId object) {
        return packValue(block, type, origin, object);
      });
}

LowBlockId SemToLowContext::cleanupTail(const PlaceCleanupPlan &plan,
                                        InstId origin, LowBlockId successor) {
  return internal::LoweringCleanupService::tail(
      plan, origin, successor, session_state_, cleanup_tails_, pending_blocks_,
      [this] { return newBlock(); },
      [this](LowBlockId block, InstId cleanup_origin,
             const PlaceCleanupPlan &cleanup_plan) {
        return emitCleanup(block, cleanup_origin, cleanup_plan);
      });
}

SlotId SemToLowContext::wakeSlotFor(LowInstId value) const {
  return internal::LoweringCoroutineService::wakeSlotFor(session_state_, value);
}

SlotId SemToLowContext::slotFor(LocalId local) {
  return internal::LoweringOwnershipService::slotFor(session_state_, local,
                                                      ownership_state_);
}

LowPlaceId SemToLowContext::rootPlaceFor(SlotId slot) {
  return internal::LoweringOwnershipService::rootPlaceFor(
      session_state_, slot, ownership_state_);
}

LowIR SemToLowContext::run() {
  // Materialize ABI-2 payload plans from canonical SemIR descriptors before
  // lowering any function. The plan owns only stable facts; runtime handles
  // and callback addresses remain target-local.
  std::uint32_t descriptor_index = 0;
  for (const auto &descriptor : session_state_.sem_ir.typedChannelDescriptors()) {
    PayloadOperationPlan plan;
    plan.descriptor_digest = descriptor.component_descriptor_digest;
    plan.payload_type_digest = descriptor.payload_type_fingerprint;
    plan.layout_digest = descriptor.layout_fingerprint;
    plan.lifecycle_digest = descriptor.lifecycle_fingerprint;
    plan.contract_digest = descriptor.component_descriptor_digest;
    plan.operation_kind = descriptor.operation_kind;
    plan.lease_policy = descriptor.lease_policy;
    plan.source_lane = descriptor_index++;
    plan.destination_lane = descriptor_index;
    plan.token_lane = 0;
    plan.outcome = session_state_.low_ir.addOutcomeDescriptor(makeChannelOutcome());
    plan.source_preserved_until_commit = true;
    plan.destination_initializes_on_commit =
        descriptor.operation_kind == ComponentAbi2OperationKind::Receive;
    ComponentAbi2Descriptor abi2_descriptor;
    abi2_descriptor.component_identity = descriptor.component_identity;
    abi2_descriptor.entity_identity = descriptor.operation_identity;
    abi2_descriptor.resource_identity = descriptor.operation_identity;
    abi2_descriptor.operation_kind = descriptor.operation_kind;
    abi2_descriptor.lease_policy = descriptor.lease_policy;
    abi2_descriptor.ownership_flags = descriptor.ownership_flags;
    abi2_descriptor.payload_type_digest = descriptor.payload_type_fingerprint;
    abi2_descriptor.layout_digest = descriptor.layout_fingerprint;
    abi2_descriptor.lifecycle_digest = descriptor.lifecycle_fingerprint;
    abi2_descriptor.contract_digest = descriptor.component_descriptor_digest;
    abi2_descriptor.runtime_abi_digest =
        StableFingerprint::fromCanonicalBytes("runtime-v1");
    plan.plan_fingerprint = componentAbi2PayloadPlanDigest(
        abi2_descriptor, plan.outcome.hasValue()
            ? session_state_.low_ir.outcomeDescriptor(plan.outcome).fingerprint()
            : StableFingerprint{}, plan.source_lane, plan.destination_lane,
        plan.token_lane, plan.source_preserved_until_commit,
        plan.destination_initializes_on_commit);
    (void)session_state_.low_ir.addPayloadOperationPlan(std::move(plan));
  }
  for (std::uint32_t index = 0;
       index < session_state_.sem_ir.functionCount(); ++index) {
    const auto function = FunctionId(index);
    if ((session_state_.sem_ir.function(function).flags &
         (SemFunctionTemplate | SemFunctionEvaluatorArtifact)) == 0 &&
        session_state_.sem_ir.functionDeclaration(function).kind ==
            SemCallableDeclarationKind::Definition) {
      const auto low_function = lowerFunction(function);
      if ((session_state_.sem_ir.function(function).flags &
           SemFunctionCoroutineScaffold) != 0)
        buildCoroutineFramePlan(function, low_function);
    }
  }
  return std::move(session_state_.low_ir);
}

void SemToLowContext::lowerBlock(InstBlockId semantic_block,
                                 LowBlockId &current) {
  const auto instructions = session_state_.sem_ir.instBlock(semantic_block);
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    const auto semantic_id = instructions[index];
    if (isTerminated(current))
      break;
    if (write_only_values_.contains(semantic_id.index))
      continue;
    if (index + 1 < instructions.size() &&
        isDirectPlacementCall(semantic_id, instructions[index + 1]))
      continue;
    const auto &semantic = session_state_.sem_ir.inst(semantic_id);
    if (semantic.kind == SemInstKind::Yield)
      continue;
    visitSemInst(semantic,
                 [&](auto typed) { lowerInst(semantic_id, typed, current); });
  }
}

const SemType &SemToLowContext::semanticType(SemIRTypeRef ref) const {
  return session_state_.sem_ir.type(ref.checked(session_state_.sem_ir));
}

const SemFunction &
SemToLowContext::semanticFunction(SemIRFunctionRef ref) const {
  return session_state_.sem_ir.function(ref.checked(session_state_.sem_ir));
}

LowBlockId SemToLowContext::newBlock() {
  return internal::LoweringControlService::newBlock(session_state_,
                                                    pending_blocks_);
}

bool SemToLowContext::isTerminated(LowBlockId block_id) const {
  return internal::LoweringControlService::isTerminated(
      session_state_, pending_blocks_, block_id);
}

} // namespace


LowIR internal::lowerToLowIRContext(
    const SemIR &sem_ir, core::Arena &arena,
    std::string_view normalized_target_triple,
    std::span<const NominalTypeLayoutArtifact> nominal_layouts,
    std::span<const LowNominalLayoutBinding> nominal_layout_bindings) {
  return SemToLowContext(sem_ir, arena, normalized_target_triple,
                         nominal_layouts, nominal_layout_bindings)
      .run();
}

} // namespace chtholly::compiler
