#pragma once

#include "chtholly/Compiler/LowerToLowIR.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>

namespace chtholly::compiler::internal {

struct LoweringFunctionState {
  FunctionId current_function;
  TypeId current_return_type;
  SlotId return_slot;
  LowBlockId return_block;
  bool unified_return = true;
};

struct LoweringLoopTarget;
struct LoweringActiveTaskScope;
struct LoweringSessionState;

struct LoweringPendingCleanupGraph {
  enum class Role : std::uint8_t { PreCommit, Transferred, Cancellation };

  InstId origin;
  LowBlockId entry;
  std::size_t begin = 0;
  std::size_t end = 0;
  Role role = Role::Transferred;
  std::vector<SlotId> local_slots;
};

// Owns per-function reset, return-block finalization, and cleanup-graph
// materialization. All containers are references into SemToLowContext; the
// service does not create a parallel lowering session or copy ownership facts.
struct LoweringFunctionService {
  struct State {
    LoweringSessionState &session;
    LoweringFunctionState &function;
    std::vector<std::vector<LowInstId>> &pending_blocks;
    std::vector<LoweringPendingCleanupGraph> &pending_cleanup_graphs;
    std::unordered_map<std::uint32_t, CoroutineCleanupGraphId>
        &suspension_pre_cleanup;
    std::unordered_map<std::uint32_t, CoroutineCleanupGraphId>
        &suspension_transferred_cleanup;
    std::unordered_map<std::uint32_t, CoroutineCleanupGraphId>
        &cancellation_cleanup;
    std::vector<LoweringLoopTarget> &loop_targets;
    std::vector<LoweringActiveTaskScope> &active_task_scopes;
    std::unordered_set<std::uint32_t> &scope_protected_suspensions;
    std::unordered_map<std::string, LowBlockId> &cleanup_tails;
    std::unordered_map<std::uint32_t, LowInstId> &values;
    std::unordered_map<std::uint32_t, SlotId> &slots;
    std::unordered_map<std::uint32_t, LowPlaceId> &places;
    std::unordered_set<std::uint32_t> &write_only_values;
    std::unordered_set<std::uint32_t> &take_values;
    std::unordered_map<std::uint32_t, TypeId> &borrow_values;
    std::unordered_map<std::uint32_t, SemPlaceId> &borrow_places;
    std::vector<SlotId> &function_slots;
    std::uint32_t &next_parameter;
    std::function<LowBlockId()> new_block;
    std::function<void(InstBlockId, LowBlockId &)> lower_block;
    std::function<void(InstBlockId)> collect_write_only_values;
    std::function<bool()> is_representation_pack;
    std::function<bool(TypeId)> has_conversion;
    std::function<LowBlockId(LowBlockId, InstId,
                             const PlaceCleanupPlan &)> emit_cleanup;
    std::function<LowBlockId(const PlaceCleanupPlan &, InstId, LowBlockId)>
        cleanup_tail;
    std::function<void(const PlaceCleanupPlan &, InstId,
                       LoweringPendingCleanupGraph::Role)>
        add_pending_cleanup_graph;
    std::function<LowInstId(LowBlockId, TypeId, InstId, LowInstId)>
        emit_transfer_return;
    std::function<LowInstId(LowBlockId, TypeId, InstId, SlotId, LowInstId)>
        emit_initialize;
    std::function<LowInstId(LowBlockId, TypeId, InstId, SlotId)>
        emit_load;
    std::function<LowInstId(LowBlockId, TypeId, InstId, LowInstId)>
        emit_return;
    std::function<LowInstId(LowBlockId, TypeId, InstId)>
        emit_unreachable;
    std::function<LowInstId(LowBlockId, TypeId, InstId)>
        emit_void;
    std::function<LowInstId(LowBlockId, TypeId, InstId)>
        emit_return_in_place;
    std::function<LowInstId(LowBlockId, TypeId, InstId)>
        emit_cleanup_end;
  };

  [[nodiscard]] static LowFunctionId lower(FunctionId function_id,
                                            State &state);
};

// These state records are non-owning views over the lowering session's
// working stores. They make ownership explicit before method-level extraction:
// service code receives a state reference instead of copying maps or creating
// parallel lowering sessions.
struct LoweringExpressionState {
  const SemIR &sem_ir;
  LowIR &low_ir;
  std::vector<std::vector<LowInstId>> &pending_blocks;
  std::unordered_map<std::uint32_t, LowInstId> &values;
  std::unordered_set<std::uint32_t> &write_only_values;
  std::unordered_set<std::uint32_t> &take_values;
  std::unordered_map<std::uint32_t, TypeId> &borrow_values;
  std::unordered_map<std::uint32_t, SemPlaceId> &borrow_places;
  std::function<LowInstId(InstId)> value_for;
};

struct LoweringExpressionService {
  [[nodiscard]] static bool isConstructor(const SemIR &sem_ir,
                                          FunctionRefId target);
  [[nodiscard]] static bool isInfallibleConstructorFor(
      const SemIR &sem_ir, FunctionRefId target, TypeId destination);
  [[nodiscard]] static bool hasConversion(const SemIR &sem_ir, TypeId type);
  [[nodiscard]] static bool isRepresentationObjectType(
      const SemIR &sem_ir, FunctionId current_function, TypeId type);
  [[nodiscard]] static bool isRepresentationPack(
      const SemIR &sem_ir, FunctionId current_function);
  static void integer(InstId semantic_id, SemIntegerLiteral semantic,
                      LowBlockId current, LoweringExpressionState &state);
  static void floating(InstId semantic_id, SemFloatLiteral semantic,
                       LowBlockId current, LoweringExpressionState &state);
  static void boolean(InstId semantic_id, SemBoolLiteral semantic,
                      LowBlockId current, LoweringExpressionState &state);
  static void character(InstId semantic_id, SemCharLiteral semantic,
                        LowBlockId current, LoweringExpressionState &state);
  static void voidValue(InstId semantic_id, SemVoidValue semantic,
                        LowBlockId current, LoweringExpressionState &state);
  static void string(InstId semantic_id, SemStringLiteral semantic,
                     LowBlockId current, LoweringExpressionState &state);
  static void nullPointer(InstId semantic_id, SemNullPointer semantic,
                          LowBlockId current, LoweringExpressionState &state);
  static void call(InstId semantic_id, SemCall semantic, LowBlockId current,
                   LoweringExpressionState &state);
  static void collectWriteOnlyValues(InstBlockId block,
                                     LoweringExpressionState &state);
  [[nodiscard]] static LowInstId lowerConstant(
      ConstantId constant, InstId semantic_id, LowBlockId current,
      LoweringExpressionState &state,
      const std::function<LowInstId(LowBlockId, TypeId, InstId, LowInstId)> &
      pack_value);
};

struct LoweringAggregateState {
  const SemIR &sem_ir;
  LowIR &low_ir;
  std::vector<std::vector<LowInstId>> &pending_blocks;
  std::unordered_map<std::uint32_t, LowInstId> &values;
  std::function<LowInstId(InstId)> value_for;
  std::function<LowInstId(LowBlockId, TypeId, InstId, LowInstId)> pack_value;
};

struct LoweringAggregateService {
  static void arrayLiteral(InstId semantic_id, SemArrayLiteral semantic,
                           LowBlockId current, LoweringAggregateState &state);
  static void tupleLiteral(InstId semantic_id, SemTupleLiteral semantic,
                           LowBlockId current, LoweringAggregateState &state);
  static void slice(InstId semantic_id, SemSlice semantic, LowBlockId current,
                    LoweringAggregateState &state);
  static void aggregateInit(InstId semantic_id, SemAggregateInit semantic,
                            LowBlockId current,
                            LoweringAggregateState &state);
  static void closure(InstId semantic_id, SemClosure semantic,
                      LowBlockId current, LoweringAggregateState &state);
  static void boundMethod(InstId semantic_id, SemBoundMethod semantic,
                          LowBlockId current,
                          LoweringAggregateState &state);
  static void unionInit(InstId semantic_id, SemUnionInit semantic,
                        LowBlockId current, LoweringAggregateState &state);
  static void enumInit(InstId semantic_id, SemEnumInit semantic,
                       LowBlockId current, LoweringAggregateState &state);
  static void enumTag(InstId semantic_id, SemEnumTag semantic,
                      LowBlockId current, LoweringAggregateState &state);
  static void enumPayloadAccess(InstId semantic_id,
                                SemEnumPayloadAccess semantic,
                                LowBlockId current,
                                LoweringAggregateState &state);
};

struct LoweringPlaceState {
  const SemIR &sem_ir;
  LowIR &low_ir;
  std::vector<std::vector<LowInstId>> &pending_blocks;
  std::unordered_map<std::uint32_t, LowInstId> &values;
  std::unordered_map<std::uint32_t, TypeId> &borrow_values;
  std::unordered_map<std::uint32_t, SemPlaceId> &borrow_places;
  std::unordered_set<std::uint32_t> &take_values;
  FunctionId &current_function;
  std::function<LowInstId(InstId)> value_for;
  std::function<SlotId(LocalId)> slot_for;
  std::function<LowPlaceId(SemPlaceId)> place_for;
  std::function<LowPlaceId(const SemPlace &)> lower_place;
  std::function<bool(TypeId)> has_conversion;
  std::function<bool(TypeId)> is_representation_object_type;
  std::function<LowInstId(LowBlockId, TypeId, InstId, LowInstId)> pack_value;
  std::function<LowInstId(LowBlockId, TypeId, InstId, LowInstId)> unpack_value;
  std::function<FunctionRefId(TypeId, SemCanonicalFunctionRole)>
      lifecycle_target;
};

struct LoweringPlaceService {
  static void nameRef(InstId semantic_id, SemNameRef semantic,
                      LowBlockId current, LoweringPlaceState &state);
  static void borrowLocal(InstId semantic_id, SemBorrowLocal semantic,
                          LowBlockId current, LoweringPlaceState &state);
  static void borrowPlace(InstId semantic_id, SemBorrowPlace semantic,
                          LowBlockId current, LoweringPlaceState &state);
  static void carrierView(InstId semantic_id, SemCarrierView semantic,
                          LowBlockId current, LoweringPlaceState &state);
  static void dereference(InstId semantic_id, SemDereference semantic,
                          LowBlockId current, LoweringPlaceState &state);
  static void memberAccess(InstId semantic_id, SemMemberAccess semantic,
                           LowBlockId current, LoweringPlaceState &state);
  static void structFieldAccess(InstId semantic_id,
                                SemStructFieldAccess semantic,
                                LowBlockId current,
                                LoweringPlaceState &state);
  static void unionFieldAccess(InstId semantic_id,
                               SemUnionFieldAccess semantic,
                               LowBlockId current, LoweringPlaceState &state);
  static void index(InstId semantic_id, SemIndex semantic, LowBlockId current,
                    LoweringPlaceState &state);
  static void move(InstId semantic_id, SemMove semantic, LowBlockId current,
                   LoweringPlaceState &state);
  static void copy(InstId semantic_id, SemCopy semantic, LowBlockId current,
                   LoweringPlaceState &state);
};

struct LoweringSessionState;

struct LoweringControlState {
  std::vector<std::vector<LowInstId>> &pending_blocks;
  std::uint32_t &next_parameter;
};

struct LoweringControlService {
  [[nodiscard]] static LowBlockId newBlock(
      LoweringSessionState &session,
      std::vector<std::vector<LowInstId>> &pending_blocks);
  [[nodiscard]] static bool isTerminated(
      const LoweringSessionState &session,
      const std::vector<std::vector<LowInstId>> &pending_blocks,
      LowBlockId block);
  [[nodiscard]] static InstId semanticBlockResult(
      const SemIR &sem_ir, std::span<const InstId> instructions);
  [[nodiscard]] static bool semanticConditionIsAlwaysTrue(
      const SemIR &sem_ir, InstBlockId block);
  [[nodiscard]] static bool semanticBlockContainsLoopBreak(
      const SemIR &sem_ir, InstBlockId block, std::uint32_t nested_loops);
  [[nodiscard]] static bool isDirectPlacementCall(
      const SemIR &sem_ir, InstId call, InstId consumer);
};

struct LoweringIfState {
  LoweringSessionState &session;
  std::vector<SlotId> &function_slots;
  std::unordered_map<std::uint32_t, LowInstId> &values;
  std::function<LowBlockId()> new_block;
  std::function<void(InstBlockId, LowBlockId &)> lower_block;
  std::function<bool(LowBlockId)> is_terminated;
  std::function<LowInstId(InstId)> value_for;
  std::function<LowBlockId(LowBlockId, InstId, const PlaceCleanupPlan &)>
      emit_cleanup;
  std::function<void(LowBlockId, InstId, LowBlockId)> emit_branch;
  std::function<void(LowBlockId, InstId, LowInstId, TargetPairId)>
      emit_branch_if;
  std::function<void(LowBlockId, InstId, SlotId, LowInstId)> emit_initialize;
  std::function<LowInstId(LowBlockId, TypeId, InstId, SlotId)> emit_load;
  std::function<LowInstId(LowBlockId, TypeId, InstId)> emit_void;
  std::function<void(LowBlockId, InstId)> emit_unreachable;
};

struct LoweringIfService {
  static void lower(InstId semantic_id, SemIf semantic, LowBlockId &current,
                    LoweringIfState &state);
  static void scopedBlock(InstId semantic_id, SemScopedBlock semantic,
                          LowBlockId &current, LoweringIfState &state);
};

struct LoweringLoopTarget {
  LowBlockId break_target;
  LowBlockId continue_target;
  std::size_t task_scope_depth = 0;
};

// Loop lowering owns control-flow construction for while/for/do-while,
// break/continue, and switch expressions. The state aliases the active
// lowering session and delegates block/cleanup operations back to the owner;
// no semantic or LowIR stores are copied into this service.
struct LoweringLoopState {
  LoweringSessionState &session;
  std::vector<std::vector<LowInstId>> &pending_blocks;
  std::vector<SlotId> &function_slots;
  std::unordered_map<std::uint32_t, LowInstId> &values;
  std::vector<LoweringLoopTarget> &loop_targets;
  std::function<std::size_t()> active_task_scope_depth;
  std::function<LowBlockId()> new_block;
  std::function<void(InstBlockId, LowBlockId &)> lower_block;
  std::function<bool(LowBlockId)> is_terminated;
  std::function<LowBlockId(LowBlockId, InstId, const PlaceCleanupPlan &)>
      emit_cleanup;
  std::function<LowBlockId(const PlaceCleanupPlan &, InstId, LowBlockId)>
      cleanup_tail;
  std::function<bool(const PlaceCleanupPlan &)> has_custom_cleanup;
  std::function<LowBlockId(LowBlockId, std::size_t,
                           CoroutineTaskGroupExitIntent)>
      task_scope_drains;
  std::function<LowInstId(InstId)> value_for;
};

struct LoweringLoopService {
  static void whileLoop(InstId semantic_id, SemWhile semantic,
                        LowBlockId &current, LoweringLoopState &state);
  static void forLoop(InstId semantic_id, SemFor semantic,
                      LowBlockId &current, LoweringLoopState &state);
  static void doWhile(InstId semantic_id, SemDoWhile semantic,
                      LowBlockId &current, LoweringLoopState &state);
  static void breakLoop(InstId semantic_id, LowBlockId &current,
                        LoweringLoopState &state);
  static void continueLoop(InstId semantic_id, LowBlockId &current,
                           LoweringLoopState &state);
  static void switchValue(InstId semantic_id, SemSwitch semantic,
                          LowBlockId &current, LoweringLoopState &state);
};

struct LoweringOwnershipState {
  std::vector<SlotId> &function_slots;
  std::unordered_map<std::uint32_t, SlotId> &slots;
  std::unordered_map<std::uint32_t, LowPlaceId> &places;
  std::unordered_map<std::uint32_t, LowPlaceId> &root_places;
};

struct LoweringOwnershipService {
  struct ComputedProjectionCleanup {
    LowPlaceId owner;
    TypeId owner_type;
    std::uint32_t field = 0;
  };
  [[nodiscard]] static SlotId slotFor(
      LoweringSessionState &session, LocalId local,
      LoweringOwnershipState &state);
  [[nodiscard]] static LowPlaceId rootPlaceFor(
      LoweringSessionState &session, SlotId slot,
      LoweringOwnershipState &state);
  [[nodiscard]] static LowPlaceId lowerPlace(
      const SemPlace &place, LoweringSessionState &session,
      LoweringOwnershipState &state,
      const std::function<const SemType &(TypeId)> &semantic_type);
  [[nodiscard]] static std::optional<ComputedProjectionCleanup>
  computedProjectionCleanup(
      SemPlaceId semantic_place, LoweringSessionState &session,
      const std::function<const SemType &(TypeId)> &semantic_type,
      const std::function<LowPlaceId(const SemPlace &)> &lower_place);
  [[nodiscard]] static FunctionRefId lifecycleTarget(
      const LoweringSessionState &session, TypeId type,
      SemCanonicalFunctionRole role);
};

// Binding/assignment lowering owns initialization and reinitialization policy
// while borrowing the active lowering session's stores. Every emitter callback
// appends to the current block owned by SemToLowContext; this state never
// copies pending blocks, ownership facts, or semantic values.
struct LoweringBindingState {
  LoweringSessionState &session;
  std::unordered_map<std::uint32_t, LowInstId> &values;
  std::function<LowInstId(InstId)> value_for;
  std::function<SlotId(LocalId)> slot_for;
  std::function<LowPlaceId(SlotId)> root_place_for;
  std::function<LowPlaceId(SemPlaceId)> place_for;
  std::function<bool(FunctionRefId)> is_constructor;
  std::function<bool(FunctionRefId, TypeId)> is_infallible_constructor;
  std::function<SemExprCategory(InstId)> expression_category;
  std::function<LowBlockId(LowBlockId, InstId,
                           std::span<const PlaceCleanupAction>)>
      emit_cleanup_actions;
  std::function<LowInstId(LowBlockId, TypeId, InstId)> emit_void;
  std::function<LowInstId(LowBlockId, TypeId, InstId, SlotId)> emit_borrow;
  std::function<LowInstId(LowBlockId, TypeId, InstId, ConstructPlanId)>
      emit_construct;
  std::function<LowInstId(LowBlockId, TypeId, InstId, SlotId, LowInstId)>
      emit_initialize_from_value;
  std::function<LowInstId(LowBlockId, TypeId, InstId, SlotId, LowInstId)>
      emit_transfer;
  std::function<LowInstId(LowBlockId, TypeId, InstId, SlotId, LowInstId)>
      emit_initialize;
  std::function<LowInstId(LowBlockId, TypeId, InstId, LowValueBlockId,
                          FieldIndex)>
      emit_projection_store;
  std::function<LowInstId(LowBlockId, TypeId, InstId, LowValueBlockId,
                          FieldIndex)>
      emit_projection_init;
  std::function<LowInstId(LowBlockId, TypeId, InstId, LowPlaceId)>
      emit_mark_initialized;
  std::function<LowInstId(LowBlockId, TypeId, InstId, LowPlaceId, LowInstId)>
      emit_initialize_place_from_value;
  std::function<LowInstId(LowBlockId, TypeId, InstId, LowPlaceId, LowInstId)>
      emit_initialize_place;
  std::function<LowValueBlockId(std::span<const LowInstId>)> add_value_block;
  std::function<LowInstId(LowBlockId, TypeId, InstId, LowValueBlockId, const SemPlace &)> emit_index_store;
};

struct LoweringBindingService {
  static void bindName(InstId semantic_id, SemBindName semantic,
                       LowBlockId current, LoweringBindingState &state);
  static void declareUninitialized(InstId semantic_id,
                                   SemDeclareUninitialized semantic,
                                   LowBlockId current,
                                   LoweringBindingState &state);
  static void outPlace(InstId semantic_id, SemOutPlace semantic,
                       LowBlockId current, LoweringBindingState &state);
  static void initializePlace(InstId semantic_id, SemInitializePlace semantic,
                              LowBlockId current,
                              LoweringBindingState &state);
  static void assign(InstId semantic_id, SemAssign semantic,
                     LowBlockId &current, LoweringBindingState &state);
  static void placement(InstId semantic_id, SemPlacement semantic,
                        LowBlockId &current, LoweringBindingState &state);
};

struct LoweringCleanupService {
  struct State {
    // Non-owning callbacks into the active lowering session. The service does
    // not own or duplicate pending blocks, PlaceState maps, or LowIR values.
    const SemIR &sem_ir;
    std::function<void(InstBlockId, LowBlockId &)> lower_block;
    std::function<bool(LowBlockId)> is_terminated;
    std::function<LowBlockId()> new_block;
    std::function<LowPlaceId(SemPlaceId)> place_for;
    std::function<SlotId(LocalId)> slot_for;
    std::function<void(LowBlockId, InstId, SlotId)> emit_end_lifetime;
    std::function<LowInstId(LowBlockId, TypeId, InstId, LowPlaceId)>
        emit_is_initialized;
    std::function<void(LowBlockId, InstId, LowInstId, LowBlockId, LowBlockId)>
        emit_branch_if;
    std::function<void(LowBlockId, InstId, LowBlockId)> emit_branch;
    std::function<LowBlockId(LowBlockId, InstId,
                             const PlaceCleanupAction &)>
        emit_cleanup_action;
    std::function<std::optional<LoweringOwnershipService::ComputedProjectionCleanup>(
        SemPlaceId)>
        computed_projection_cleanup;
    std::function<LowInstId(LowBlockId, TypeId, InstId, LowPlaceId)>
        emit_place_address;
    std::function<LowInstId(LowBlockId, TypeId, InstId, LowInstId,
                            FieldIndex)>
        emit_projection_take;
    std::function<TypeId(LowPlaceId)> place_type;
    std::function<void(LowBlockId, InstId, LowPlaceId)> emit_mark_moved;
    std::function<LowBlockId(LowBlockId, InstId, TypeId, LowInstId)>
        emit_destroy_value;
    std::function<LowBlockId(LowBlockId, InstId, LowPlaceId)> emit_destroy;
  };
  [[nodiscard]] static LowBlockId action(const PlaceCleanupAction &action,
                                          LowBlockId block_id, InstId origin,
                                          State &state);
  [[nodiscard]] static LowBlockId emit(
      const PlaceCleanupPlan &plan, LowBlockId block_id, InstId origin,
      State &state);
  [[nodiscard]] static bool hasCustomCleanup(const SemIR &sem_ir,
                                             const PlaceCleanupPlan &plan);
  [[nodiscard]] static LowBlockId tail(
      const PlaceCleanupPlan &plan, InstId origin, LowBlockId successor,
      LoweringSessionState &session,
      std::unordered_map<std::string, LowBlockId> &cleanup_tails,
      std::vector<std::vector<LowInstId>> &pending_blocks,
      const std::function<LowBlockId()> &new_block,
      const std::function<LowBlockId(LowBlockId, InstId,
                                     const PlaceCleanupPlan &)> &emit_cleanup);
};

struct LoweringDestroyState {
  LoweringSessionState &session;
  std::vector<std::vector<LowInstId>> &pending_blocks;
};

struct LoweringDestroyService {
  [[nodiscard]] static LowBlockId place(LowBlockId block, InstId origin,
                                        LowPlaceId place,
                                        LoweringDestroyState &state);
  [[nodiscard]] static LowBlockId value(LowBlockId block, InstId origin,
                                        TypeId type, LowInstId value,
                                        LoweringDestroyState &state);
};

struct LoweringCoroutineState {
  std::unordered_map<std::uint32_t, CoroutineCleanupGraphId>
      &suspension_pre_cleanup;
  std::unordered_map<std::uint32_t, CoroutineCleanupGraphId>
      &suspension_transferred_cleanup;
  std::unordered_map<std::uint32_t, CoroutineCleanupGraphId>
      &cancellation_cleanup;
};

struct LoweringCoroutineService {
  [[nodiscard]] static SlotId wakeSlotFor(const LoweringSessionState &session,
                                           LowInstId value);
  [[nodiscard]] static CoroutineTaskCreatePlanId buildTaskCreatePlan(
      LoweringSessionState &session, FunctionRefId target,
      CoroutineTaskCreateMode mode, TypeId checked_type);
  [[nodiscard]] static CoroutineTaskCompletionSetPlanId
  buildCompletionSetPlan(LoweringSessionState &session, FunctionId scaffold,
                         std::span<const InstId> operands, TypeId set_type);
  [[nodiscard]] static CoroutineTaskCompletionCombinePlanId
  buildCompletionCombinePlan(LoweringSessionState &session, FunctionId scaffold,
                             InstId semantic_id, InstId set,
                             CoroutineTaskCompletionCombineKind operation,
                             TypeId result_type);
};

struct LoweringCoroutineInstructionState {
  LoweringSessionState &session;
  std::vector<std::vector<LowInstId>> &pending_blocks;
  std::unordered_map<std::uint32_t, LowInstId> &values;
  std::vector<SlotId> &function_slots;
  LoweringFunctionState &function;
};

struct LoweringCoroutineInstructionService {
  static void unary(InstId semantic_id, TypeId type, LowBlockId current,
                    LowInstId operand,
                    LoweringCoroutineInstructionState &state,
                    LowInstKind kind);
  static void taskCreate(InstId semantic_id, FunctionRefId target,
                         InstBlockId operands, CoroutineTaskCreateMode mode,
                         TypeId checked_type, LowBlockId current,
                         LowInstId active_task_group,
                         LoweringCoroutineInstructionState &state);
  static void completionArm(InstId semantic_id,
                            SemCoroutineTaskCompletionArm semantic,
                            LowBlockId current,
                            LoweringCoroutineInstructionState &state);
  static void completionReady(InstId semantic_id,
                              SemCoroutineTaskCompletionReady semantic,
                              LowBlockId current,
                              LoweringCoroutineInstructionState &state);
  static void completionDetach(InstId semantic_id,
                               SemCoroutineTaskCompletionDetach semantic,
                               LowBlockId current,
                               LoweringCoroutineInstructionState &state);
  static void completionSetCreate(
      InstId semantic_id, SemCoroutineTaskCompletionSetCreate semantic,
      LowBlockId current, LoweringCoroutineInstructionState &state);
  using CleanupTailFn = std::function<LowBlockId(
      const PlaceCleanupPlan &, InstId, LowBlockId)>;
  using TaskScopeDrainFn = std::function<LowBlockId(
      LowBlockId, std::size_t, CoroutineTaskGroupExitIntent)>;
  [[nodiscard]] static LowBlockId payloadTerminal(
      InstId semantic_id, InstId payload, bool selected_error,
      LowBlockId current, LoweringCoroutineInstructionState &state,
      const CleanupTailFn &cleanup_tail,
      const TaskScopeDrainFn &task_scope_drains);
  [[nodiscard]] static LowBlockId cancelledTerminal(
      InstId semantic_id, LowBlockId current,
      LoweringCoroutineInstructionState &state,
      const CleanupTailFn &cleanup_tail,
      const TaskScopeDrainFn &task_scope_drains);
};

struct LoweringActiveTaskScope {
  LowInstId group;
  InstId origin;
};

struct LoweringCoroutineScopeState {
  LoweringCoroutineInstructionState &instruction;
  std::vector<LoweringActiveTaskScope> &active_scopes;
  std::unordered_set<std::uint32_t> &protected_suspensions;
  std::function<void(InstBlockId, LowBlockId &)> lower_block;
  std::function<bool(LowBlockId)> is_terminated;
  std::function<LowBlockId(LowBlockId, InstId, const PlaceCleanupPlan &)>
      emit_cleanup;
};

struct LoweringCoroutineScopeService {
  [[nodiscard]] static LowBlockId drain(
      LowBlockId current, InstId origin, LowInstId group,
      CoroutineTaskGroupExitIntent exit_intent,
      LoweringCoroutineScopeState &state);
  [[nodiscard]] static LowBlockId drains(
      LowBlockId current, std::size_t target_depth,
      CoroutineTaskGroupExitIntent exit_intent,
      LoweringCoroutineScopeState &state);
  static void cancellationCheck(InstId origin,
                                const PlaceCleanupPlan &cleanup,
                                LowBlockId &current,
                                LoweringCoroutineScopeState &state);
  static void taskScope(InstId semantic_id, SemCoroutineTaskScope semantic,
                        LowBlockId &current,
                        LoweringCoroutineScopeState &state);
  static void suspend(InstId semantic_id, SemCoroutineSuspend semantic,
                      LowBlockId &current,
                      LoweringCoroutineScopeState &state);
  static void cancellationCheck(
      InstId semantic_id, SemCoroutineCancellationCheck semantic,
      LowBlockId &current, LoweringCoroutineScopeState &state);
  static void completionCombine(
      InstId semantic_id, InstId set,
      CoroutineTaskCompletionCombineKind operation, TypeId result_type,
      LowBlockId &current, LoweringCoroutineScopeState &state);
};

struct LoweringCoroutineFrameState {
  LoweringSessionState &session;
  const std::unordered_map<std::uint32_t, CoroutineCleanupGraphId>
      &suspension_pre_cleanup;
  const std::unordered_map<std::uint32_t, CoroutineCleanupGraphId>
      &suspension_transferred_cleanup;
  const std::unordered_map<std::uint32_t, CoroutineCleanupGraphId>
      &cancellation_cleanup;
  const std::unordered_set<std::uint32_t> &scope_protected_suspensions;
  std::function<LowPlaceId(SlotId)> root_place_for;
};

struct LoweringCoroutineFrameService {
  static void build(FunctionId function, LowFunctionId low_function,
                    LoweringCoroutineFrameState &state);
};

struct LoweringInteropService {
  [[nodiscard]] static ForeignOperationPlanId operationPlan(
      LowIR &low_ir, const interop::ForeignOperationArtifact &operation);
  [[nodiscard]] static ForeignOperationCompletionPlanId completionPlan(
      LowIR &low_ir, ForeignOperationPlanId operation_id,
      const interop::ForeignOperationArtifact &operation,
      TypeId completion_carrier);
  static void foreignFunctionRef(InstId semantic_id,
                                 SemForeignFunctionRef semantic,
                                 LowBlockId current,
                                 LoweringExpressionState &state);
  static void callbackAdapter(InstId semantic_id,
                              SemCallbackAdapter semantic,
                              LowBlockId current,
                              LoweringExpressionState &state);
  static void indirectForeignCall(InstId semantic_id,
                                  SemIndirectForeignCall semantic,
                                  LowBlockId current,
                                  LoweringExpressionState &state);
  static void callbackRegistrationBinding(
      InstId semantic_id, SemCallbackRegistrationBinding semantic,
      LoweringExpressionState &state);
  static void makeCallbackRegistration(
      InstId semantic_id, SemMakeCallbackRegistration semantic,
      LowBlockId current, LoweringExpressionState &state);
  static void callbackRegistrationActive(
      InstId semantic_id, SemCallbackRegistrationActive semantic,
      LowBlockId current, LoweringExpressionState &state);
  static void callbackRegistrationFinish(
      InstId semantic_id, InstId registration, TypeId result_type,
      LowBlockId current, LoweringExpressionState &state, bool cancel);
  static void callbackRegistrationCancelAsync(
      InstId semantic_id, SemCallbackCancelAsync semantic, LowBlockId current,
      LoweringExpressionState &state);
  static void callbackCompletionPending(
      InstId semantic_id, SemCallbackCompletionPending semantic,
      LowBlockId current, LoweringExpressionState &state);
  static void callbackCompletionPoll(
      InstId semantic_id, SemCallbackCompletionPoll semantic,
      LowBlockId current, LoweringExpressionState &state);
  static void callbackWait(InstId semantic_id, SemCallbackWait semantic,
                           LowBlockId current,
                           LoweringExpressionState &state);
  static void makeCallbackAdapter(InstId semantic_id,
                                  SemMakeCallbackAdapter semantic,
                                  LowBlockId current,
                                  std::span<const LowInstId> fields,
                                  LoweringExpressionState &state);
  enum class ForeignOperationProjectionKind : std::uint8_t {
    Completion,
    Wake,
    Port,
  };
  struct ForeignOperationProjectionState {
    LoweringSessionState &session;
    std::unordered_map<std::uint32_t, LowInstId> &values;
    std::function<LowInstId(InstId)> value_for;
    std::function<LowInstId(
        LowBlockId, TypeId, InstId, ForeignOperationCompletionPlanId,
        LowValueBlockId, ForeignOperationProjectionKind)> emit;
  };
  static void foreignOperationProjection(
      InstId semantic_id, FunctionRefId target, InstBlockId operands,
      TypeId result_type, LowBlockId current,
      ForeignOperationProjectionKind kind,
      ForeignOperationProjectionState &state);
  struct ForeignOperationCallState {
    LoweringSessionState &session;
    std::unordered_map<std::uint32_t, LowInstId> &values;
    std::function<LowInstId(InstId)> value_for;
    std::function<LowInstId(LowBlockId, TypeId, InstId,
                            ForeignCallOutcomePlanId, LowValueBlockId)>
        emit_foreign_result;
    std::function<void(InstId, FunctionRefId, InstBlockId, TypeId,
                       LowBlockId &)> lower_plain_call;
  };
  static void foreignOperationCall(InstId semantic_id,
                                   SemForeignOperationCall semantic,
                                   LowBlockId current,
                                   ForeignOperationCallState &state);
};

struct LoweringInteropState {
  const SemIR &sem_ir;
  LowIR &low_ir;
};

struct LoweringSessionState {
  LoweringSessionState(
      const SemIR &semantic_ir, core::Arena &arena,
      std::string_view normalized_target_triple,
      std::span<const NominalTypeLayoutArtifact> nominal_layouts,
      std::span<const LowNominalLayoutBinding> nominal_layout_bindings)
      : sem_ir(semantic_ir),
        low_ir(arena, semantic_ir, std::string(normalized_target_triple),
               nominal_layouts, nominal_layout_bindings) {}

  const SemIR &sem_ir;
  LowIR low_ir;
};

[[nodiscard]] LowIR lowerToLowIRContext(
    const SemIR &sem_ir, core::Arena &arena,
    std::string_view normalized_target_triple,
    std::span<const NominalTypeLayoutArtifact> nominal_layouts,
    std::span<const LowNominalLayoutBinding> nominal_layout_bindings);

} // namespace chtholly::compiler::internal
