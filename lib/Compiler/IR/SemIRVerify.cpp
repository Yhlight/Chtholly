#include "chtholly/Compiler/SemIR.h"

#include "chtholly/Compiler/BuiltinOperator.h"
#include "chtholly/Compiler/CallableOwnership.h"
#include "chtholly/Compiler/CarrierView.h"

#include "SemIRVerificationContext.h"

#include <array>
#include <cassert>
#include <limits>
#include <ranges>
#include <sstream>
#include <unordered_set>

namespace chtholly::compiler {
namespace {

constexpr std::uint32_t UnionFieldUnsafeBit = 1U << 31U;
constexpr std::uint32_t UnionFieldIndexMask = ~UnionFieldUnsafeBit;
constexpr std::uint32_t ProjectionIndexMask = 0x7fffffffU;
constexpr std::uint32_t ProjectionKindShift = 31U;

std::size_t combineHash(std::size_t hash, std::uint32_t value) {
  hash ^= static_cast<std::size_t>(value) + 0x9e3779b9U + (hash << 6U) +
          (hash >> 2U);
  return hash;
}

constexpr std::uint32_t MutableReferenceBit = 1U;
constexpr std::uint32_t ParameterProvenanceBit = 1U << 1U;
constexpr std::uint32_t ProvenanceIndexShift = 2U;

std::uint32_t encodeReferenceFacts(SemReferenceMutability mutability,
                                   SemReferenceProvenanceKind provenance_kind,
                                   std::uint32_t provenance_index) {
  auto result =
      mutability == SemReferenceMutability::Mutable ? MutableReferenceBit : 0U;
  if (provenance_kind == SemReferenceProvenanceKind::Parameter) {
    assert(provenance_index <=
           (std::numeric_limits<std::uint32_t>::max() >> ProvenanceIndexShift));
    result |=
        ParameterProvenanceBit | (provenance_index << ProvenanceIndexShift);
  }
  return result;
}

std::uint16_t semanticCapability(CallableSemanticRole role) {
  if (role == CallableSemanticRole::None || role >= CallableSemanticRole::Count)
    return CallableCapabilityNone;
  return static_cast<std::uint16_t>(1U << (static_cast<unsigned>(role) - 1U));
}

CallableSemanticDomain semanticDomain(CallableSemanticRole role) {
  if (role == CallableSemanticRole::Constructor)
    return CallableSemanticDomain::NominalConstruction;
  if (role == CallableSemanticRole::Copy || role == CallableSemanticRole::Drop)
    return CallableSemanticDomain::Lifecycle;
  if (role == CallableSemanticRole::Pack || role == CallableSemanticRole::Init)
    return CallableSemanticDomain::ValueRepresentation;
  if (role >= CallableSemanticRole::ProjectionLoad &&
      role <= CallableSemanticRole::ProjectionBorrowMut)
    return CallableSemanticDomain::ObjectProjection;
  if (role >= CallableSemanticRole::ObjectInit &&
      role <= CallableSemanticRole::ObjectDrop)
    return CallableSemanticDomain::ObjectShell;
  return CallableSemanticDomain::Ordinary;
}

} // namespace


bool SemIR::verify(std::string &error) const {
  internal::SemIRVerificationContext verification(*this);
  if (!verification.verifyStructure(error) ||
      !verification.verifyEntityTables(error) ||
      !verification.verifyTypeRecords(error))
    return false;
  if (!verification.verifyInstructions(error))
    return false;
  if (!verification.verifyConstantRecords(error))
    return false;
  if (!verification.verifyControlFlow(error) ||
      !verification.verifyFunctions(error))
    return false;
  for (const auto &descriptor : typed_channel_descriptors_) {
    if (!descriptor.owner_function.hasValue() ||
        descriptor.owner_function.index >= functionCount() ||
        !descriptor.payload_type.hasValue() ||
        descriptor.payload_type.index >= types_.size() ||
        !descriptor.payload_type_fingerprint.hasValue() ||
        !descriptor.layout_fingerprint.hasValue() ||
        !descriptor.lifecycle_fingerprint.hasValue() ||
        descriptor.outcome_fingerprint != makeChannelOutcome().fingerprint() ||
        descriptor.runtime_abi_epoch == 0 ||
        !descriptor.concurrency.transferable ||
        descriptor.component_identity.empty() ||
        descriptor.operation_identity.empty() ||
        descriptor.operation_kind == ComponentAbi2OperationKind::Invoke ||
        descriptor.lease_policy > ComponentAbi2LeasePolicy::Shared ||
        !descriptor.component_descriptor_digest.hasValue() ||
        type(descriptor.payload_type).kind == SemTypeKind::Reference ||
        type(descriptor.payload_type).kind == SemTypeKind::Slice ||
        type(descriptor.payload_type).kind == SemTypeKind::RawPointer ||
        type(descriptor.payload_type).kind == SemTypeKind::TypeParameter ||
        type(descriptor.payload_type).kind == SemTypeKind::TypeProjection ||
        (descriptor.move_function.hasValue() &&
         descriptor.move_function.index >= functionRefCount()) ||
        (descriptor.drop_function.hasValue() &&
         descriptor.drop_function.index >= functionRefCount()) ||
        (descriptor.move_target &&
         (descriptor.move_target->kind != PublicEntityKind::Function ||
          !descriptor.move_target->expected_fingerprint.hasValue())) ||
        (descriptor.drop_target &&
         (descriptor.drop_target->kind != PublicEntityKind::Function ||
          !descriptor.drop_target->expected_fingerprint.hasValue()))) {
      error = "typed channel descriptor has invalid SemIR facts";
      return false;
    }
  }
  if (!verification.verifyDeclarationRecords(error))
    return false;
  std::vector<FunctionId> instruction_owners(insts_.size());
  for (std::uint32_t function_index = 0; function_index < functions_.size();
       ++function_index) {
    const auto function_id = FunctionId(function_index);
    std::vector<InstBlockId> pending{function(function_id).body};
    std::unordered_set<std::uint32_t> visited;
    while (!pending.empty()) {
      const auto block = pending.back();
      pending.pop_back();
      if (!visited.insert(block.index).second)
        continue;
      for (const auto instruction : instBlock(block)) {
        if (instruction_owners[instruction.index].hasValue() &&
            instruction_owners[instruction.index] != function_id) {
          error = "instruction is shared by multiple function bodies";
          return false;
        }
        instruction_owners[instruction.index] = function_id;
        const auto &body_inst = inst(instruction);
        if (body_inst.kind == SemInstKind::If ||
            body_inst.kind == SemInstKind::Switch ||
            body_inst.kind == SemInstKind::SwitchArm)
          pending.push_back(InstBlockId(body_inst.arg1));
        if (body_inst.kind == SemInstKind::IfArm)
          pending.push_back(InstBlockId(body_inst.arg0));
        if (body_inst.kind == SemInstKind::While) {
          pending.push_back(InstBlockId(body_inst.arg0));
          pending.push_back(InstBlockId(body_inst.arg1));
        }
        if (body_inst.kind == SemInstKind::For) {
          pending.push_back(InstBlockId(body_inst.arg0));
          pending.push_back(InstBlockId(body_inst.arg1));
        }
        if (body_inst.kind == SemInstKind::ForClause)
          pending.push_back(InstBlockId(body_inst.arg1));
        if (body_inst.kind == SemInstKind::DoWhile) {
          pending.push_back(InstBlockId(body_inst.arg0));
          pending.push_back(InstBlockId(body_inst.arg1));
        }
        if (body_inst.kind == SemInstKind::Defer)
          pending.push_back(InstBlockId(body_inst.arg0));
        if (body_inst.kind == SemInstKind::CoroutineTaskScope)
          pending.push_back(InstBlockId(body_inst.arg0));
      }
    }
  }
  std::unordered_map<std::uint32_t, InstId> local_bindings;
  std::unordered_set<std::uint32_t> duplicate_bindings;
  for (std::uint32_t index = 0; index < insts_.size(); ++index) {
    const auto &candidate = inst(InstId(index));
    if (candidate.kind != SemInstKind::BindName)
      continue;
    if (!local_bindings.emplace(candidate.arg0, InstId(index)).second)
      duplicate_bindings.insert(candidate.arg0);
  }
  for (std::uint32_t index = 0; index < insts_.size(); ++index) {
    const auto extension_id = InstId(index);
    const auto &extension = inst(extension_id);
    if (extension.kind != SemInstKind::ExtendTemporary)
      continue;
    const auto binding = local_bindings.find(extension.arg0);
    if (!instruction_owners[extension_id.index].hasValue() ||
        binding == local_bindings.end() ||
        duplicate_bindings.contains(extension.arg0) ||
        binding->second.index <= extension_id.index ||
        instruction_owners[extension_id.index] !=
            instruction_owners[binding->second.index]) {
      error = "temporary lifetime extension has no unique local binding";
      return false;
    }
    const auto borrowed = InstId(inst(binding->second).arg1);
    if (inst(borrowed).kind != SemInstKind::BorrowPlace ||
        instruction_owners[extension_id.index] !=
            instruction_owners[borrowed.index]) {
      error = "temporary lifetime extension binding is not a local borrow";
      return false;
    }
    auto source = InstId(inst(borrowed).arg0);
    while (inst(source).kind == SemInstKind::StructFieldAccess ||
           inst(source).kind == SemInstKind::Index) {
      if (instruction_owners[extension_id.index] !=
          instruction_owners[source.index]) {
        error = "temporary lifetime extension projection crosses a function";
        return false;
      }
      if (inst(source).kind == SemInstKind::StructFieldAccess) {
        const auto base = InstId(inst(source).arg0);
        const auto &base_type = type(TypeId(inst(base).type));
        const auto field = integer(IntegerId(inst(source).arg1));
        if (base_type.kind != SemTypeKind::Nominal || field < 0 ||
            static_cast<std::uint64_t>(field) >=
                nominalType(NominalTypeId(base_type.arg0)).fields.size() ||
            nominalType(NominalTypeId(base_type.arg0))
                    .fields[static_cast<std::size_t>(field)]
                    .projection_kind !=
                PublicObjectProjectionKind::StableAddress) {
          error = "temporary lifetime extension uses an unstable projection";
          return false;
        }
      }
      source = InstId(inst(source).arg0);
    }
    if (source != InstId(extension.arg1) ||
        instruction_owners[extension_id.index] !=
            instruction_owners[source.index]) {
      error = "temporary lifetime extension does not borrow its cleanup root";
      return false;
    }
  }
  for (std::uint32_t index = 0; index < insts_.size(); ++index) {
    const auto &instruction = inst(InstId(index));
    const auto kind = instruction.kind;
    const auto owner = instruction_owners[index];
    if (kind == SemInstKind::InterfaceCall) {
      if (!owner.hasValue() ||
          (function(owner).flags & SemFunctionTemplate) == 0) {
        error = "symbolic interface call is not owned by a generic template";
        return false;
      }
      const auto encoded =
          static_cast<std::uint64_t>(integer(IntegerId(instruction.arg0)));
      const auto constraint_index = static_cast<std::uint32_t>(encoded >> 32U);
      const auto requirement_index = static_cast<std::uint32_t>(encoded);
      const auto constraints = functionConstraints(owner);
      if (constraint_index >= constraints.size() ||
          constraints[constraint_index].interface_id.index >=
              interfaces_.size() ||
          requirement_index >=
              interface(constraints[constraint_index].interface_id)
                  .requirements.size() ||
          interface(constraints[constraint_index].interface_id)
                  .requirements[requirement_index]
                  .kind != SemInterfaceRequirementKind::Function) {
        error = "symbolic interface call has an invalid requirement slot";
        return false;
      }
    }
    if (kind == SemInstKind::Return && owner.hasValue() &&
        (function(owner).flags & SemFunctionAsync) != 0) {
      error = "async scaffold retains an ordinary return terminal";
      return false;
    }
    const auto scaffold_operation =
        kind == SemInstKind::CoroutineSuspend ||
        kind == SemInstKind::CoroutineCancellationCheck ||
        kind == SemInstKind::CoroutineRuntimeFault ||
        kind == SemInstKind::CoroutineReturnSuccess ||
        kind == SemInstKind::CoroutineReturnError ||
        kind == SemInstKind::CoroutineReturnCancelled ||
        kind == SemInstKind::CoroutineExecutorSwitch ||
        kind == SemInstKind::CoroutineChildTaskCreate ||
        kind == SemInstKind::CoroutineTaskScope ||
        kind == SemInstKind::CoroutineTaskCompletionArm ||
        kind == SemInstKind::CoroutineTaskCompletionReady ||
        kind == SemInstKind::CoroutineTaskCompletionDetach ||
        kind == SemInstKind::CoroutineTaskCompletionSetCreate ||
        kind == SemInstKind::CoroutineTaskCompletionWaitAll ||
        kind == SemInstKind::CoroutineTaskCompletionSelect ||
        kind == SemInstKind::CoroutineTaskCompletionRace ||
        kind == SemInstKind::CoroutineTaskSelectionWinner ||
        kind == SemInstKind::CoroutineTaskSelectionTakeRemaining;
    const auto driver_operation = kind == SemInstKind::CoroutineTaskCreate;
    const auto shared_task_operation =
        kind == SemInstKind::CoroutineTaskJoin ||
        kind == SemInstKind::CoroutineTaskQuery ||
        kind == SemInstKind::CoroutineTaskTakeResult ||
        kind == SemInstKind::CoroutineTaskTakeError ||
        kind == SemInstKind::CoroutineCheckedStatus ||
        kind == SemInstKind::CoroutineCheckedTake ||
        kind == SemInstKind::CoroutineOutcomeCompleted ||
        kind == SemInstKind::CoroutineOutcomeFailed ||
        kind == SemInstKind::CoroutineOutcomeCancelled;
    const auto protocol_operation =
        scaffold_operation || driver_operation || shared_task_operation;
    if (!protocol_operation)
      continue;
    if (!owner.hasValue()) {
      error = "coroutine operation is not owned by a function body";
      return false;
    }
    const auto &owner_function = function(owner);
    const auto is_scaffold =
        (owner_function.flags & SemFunctionAsync) != 0 &&
        (owner_function.flags & (SemFunctionTemplate | SemFunctionSpecific)) ==
            0 &&
        type(owner_function.type).kind == SemTypeKind::AsyncFunction;
    const auto is_driver =
        (owner_function.flags & SemFunctionCoroutineTaskDriver) != 0;
    if ((scaffold_operation && !is_scaffold) ||
        (driver_operation && !is_driver) ||
        (shared_task_operation && !is_driver && !is_scaffold)) {
      error = scaffold_operation
                  ? "coroutine operation requires an internal async scaffold"
                  : "task protocol operation requires an internal task driver";
      return false;
    }
    if (kind == SemInstKind::CoroutineReturnSuccess &&
        TypeId(inst(InstId(instruction.arg0)).type) !=
            asyncSuccessType(owner_function.type)) {
      error = "coroutine success terminal payload does not match its scaffold";
      return false;
    }
    if (kind == SemInstKind::CoroutineReturnError) {
      const auto expected = asyncErrorType(owner_function.type);
      if (!expected ||
          TypeId(inst(InstId(instruction.arg0)).type) != *expected) {
        error = "coroutine error terminal payload does not match its scaffold";
        return false;
      }
    }
  }
  std::vector<std::uint32_t> checked_status_uses(insts_.size());
  std::vector<std::uint32_t> checked_take_uses(insts_.size());
  std::vector<std::uint32_t> selection_remaining_value_uses(insts_.size());
  std::vector<std::uint32_t> selection_remaining_local_uses(locals_.size());
  for (std::uint32_t index = 0; index < insts_.size(); ++index) {
    const auto &consumer = inst(InstId(index));
    if (consumer.kind == SemInstKind::CoroutineCheckedStatus ||
        consumer.kind == SemInstKind::CoroutineCheckedTake) {
      const auto source = InstId(consumer.arg0);
      if (instruction_owners[index] != instruction_owners[source.index]) {
        error = "coroutine checked carrier crosses a function boundary";
        return false;
      }
      if (consumer.kind == SemInstKind::CoroutineCheckedStatus)
        ++checked_status_uses[source.index];
      else
        ++checked_take_uses[source.index];
    }
    if (consumer.kind == SemInstKind::CoroutineTaskSelectionTakeRemaining) {
      auto source = InstId(consumer.arg0);
      if (inst(source).kind == SemInstKind::Move)
        source = InstId(inst(source).arg0);
      if (inst(source).kind == SemInstKind::NameRef)
        ++selection_remaining_local_uses[inst(source).arg0];
      else
        ++selection_remaining_value_uses[source.index];
    }
  }
  if (std::ranges::any_of(selection_remaining_value_uses,
                          [](auto uses) { return uses > 1; }) ||
      std::ranges::any_of(selection_remaining_local_uses,
                          [](auto uses) { return uses > 1; })) {
    error = "coroutine task selection remaining set is taken more than once";
    return false;
  }
  for (std::uint32_t index = 0; index < insts_.size(); ++index) {
    if (type(TypeId(inst(InstId(index)).type)).kind !=
        SemTypeKind::CoroutineChecked)
      continue;
    const auto producer = inst(InstId(index)).kind;
    if (producer != SemInstKind::CoroutineTaskCreate &&
        producer != SemInstKind::CoroutineChildTaskCreate &&
        producer != SemInstKind::CoroutineTaskQuery &&
        producer != SemInstKind::CoroutineTaskTakeResult &&
        producer != SemInstKind::CoroutineTaskTakeError &&
        producer != SemInstKind::CoroutineTaskCompletionArm &&
        producer != SemInstKind::CoroutineTaskCompletionSetCreate &&
        producer != SemInstKind::CoroutineTaskCompletionSelect &&
        producer != SemInstKind::CoroutineTaskCompletionRace) {
      error = "coroutine checked carrier has an invalid producer";
      return false;
    }
    if (checked_status_uses[index] == 0 || checked_take_uses[index] != 1) {
      error = "coroutine checked carrier requires status and exactly one take";
      return false;
    }
  }
  const auto task_key = [&](InstId id) {
    const auto &value = inst(id);
    return value.kind == SemInstKind::NameRef
               ? (UINT64_C(1) << 32U) | value.arg0
               : static_cast<std::uint64_t>(id.index);
  };
  const auto checked_success = [&](InstId condition) -> InstId {
    const auto &equal = inst(condition);
    if (equal.kind != SemInstKind::Equal)
      return InstId::invalid();
    const auto matches = [&](InstId status, InstId zero) {
      return inst(status).kind == SemInstKind::CoroutineCheckedStatus &&
             inst(zero).kind == SemInstKind::IntegerLiteral &&
             integer(IntegerId(inst(zero).arg0)) == 0;
    };
    if (matches(InstId(equal.arg0), InstId(equal.arg1)))
      return InstId(inst(InstId(equal.arg0)).arg0);
    if (matches(InstId(equal.arg1), InstId(equal.arg0)))
      return InstId(inst(InstId(equal.arg1)).arg0);
    return InstId::invalid();
  };
  const auto joined_task = [&](InstId condition) -> InstId {
    const auto &equal = inst(condition);
    if (equal.kind != SemInstKind::Equal)
      return InstId::invalid();
    const auto matches = [&](InstId join, InstId zero) {
      return inst(join).kind == SemInstKind::CoroutineTaskJoin &&
             inst(zero).kind == SemInstKind::IntegerLiteral &&
             integer(IntegerId(inst(zero).arg0)) == 0;
    };
    if (matches(InstId(equal.arg0), InstId(equal.arg1)))
      return InstId(inst(InstId(equal.arg0)).arg0);
    if (matches(InstId(equal.arg1), InstId(equal.arg0)))
      return InstId(inst(InstId(equal.arg1)).arg0);
    return InstId::invalid();
  };
  enum class RefinedOutcome : std::uint8_t {
    None,
    Completed,
    Failed,
    Cancelled
  };
  struct OutcomeRefinement {
    std::uint64_t task = 0;
    RefinedOutcome outcome = RefinedOutcome::None;
  };
  const auto refined_outcome = [&](InstId condition) {
    const auto &predicate = inst(condition);
    if (predicate.kind != SemInstKind::CoroutineOutcomeCompleted &&
        predicate.kind != SemInstKind::CoroutineOutcomeFailed &&
        predicate.kind != SemInstKind::CoroutineOutcomeCancelled)
      return OutcomeRefinement{};
    auto take_id = InstId(predicate.arg0);
    if (inst(take_id).kind == SemInstKind::If) {
      const auto checked = checked_success(InstId(inst(take_id).arg0));
      if (!checked.hasValue())
        return OutcomeRefinement{};
      const auto arms = instBlock(InstBlockId(inst(take_id).arg1));
      if (arms.empty())
        return OutcomeRefinement{};
      const auto body = instBlock(InstBlockId(inst(arms.front()).arg0));
      const auto found = std::ranges::find_if(body, [&](InstId candidate) {
        const auto &value = inst(candidate);
        return value.kind == SemInstKind::CoroutineCheckedTake &&
               value.arg0 == checked.index;
      });
      if (found == body.end())
        return OutcomeRefinement{};
      take_id = *found;
    }
    const auto &take = inst(take_id);
    if (take.kind != SemInstKind::CoroutineCheckedTake)
      return OutcomeRefinement{};
    const auto &query = inst(InstId(take.arg0));
    if (query.kind != SemInstKind::CoroutineTaskQuery)
      return OutcomeRefinement{};
    const auto outcome =
        predicate.kind == SemInstKind::CoroutineOutcomeCompleted
            ? RefinedOutcome::Completed
        : predicate.kind == SemInstKind::CoroutineOutcomeFailed
            ? RefinedOutcome::Failed
            : RefinedOutcome::Cancelled;
    return OutcomeRefinement{task_key(InstId(query.arg0)), outcome};
  };
  const auto verify_protocol_block =
      [&](const auto &self, InstBlockId block,
          std::unordered_set<std::uint32_t> successful_checked,
          std::unordered_set<std::uint64_t> joined,
          std::unordered_map<std::uint64_t, RefinedOutcome> outcomes,
          std::string &protocol_error) -> bool {
    for (const auto id : instBlock(block)) {
      const auto &value = inst(id);
      if (value.kind == SemInstKind::CoroutineCheckedTake &&
          !successful_checked.contains(value.arg0)) {
        protocol_error = "coroutine checked extraction is not success-refined";
        return false;
      }
      if (value.kind == SemInstKind::CoroutineTaskQuery &&
          !joined.contains(task_key(InstId(value.arg0)))) {
        protocol_error = "coroutine task query is not dominated by join";
        return false;
      }
      if (value.kind == SemInstKind::CoroutineTaskTakeResult ||
          value.kind == SemInstKind::CoroutineTaskTakeError) {
        const auto expected = value.kind == SemInstKind::CoroutineTaskTakeResult
                                  ? RefinedOutcome::Completed
                                  : RefinedOutcome::Failed;
        const auto found = outcomes.find(task_key(InstId(value.arg0)));
        if (found == outcomes.end() || found->second != expected) {
          protocol_error =
              "coroutine task payload take lacks outcome refinement";
          return false;
        }
      }
      if (value.kind == SemInstKind::If) {
        auto nested_checked = successful_checked;
        auto nested_joined = joined;
        auto nested_outcomes = outcomes;
        if (const auto checked = checked_success(InstId(value.arg0));
            checked.hasValue())
          nested_checked.insert(checked.index);
        if (const auto task = joined_task(InstId(value.arg0)); task.hasValue())
          nested_joined.insert(task_key(task));
        const auto outcome = refined_outcome(InstId(value.arg0));
        if (outcome.outcome != RefinedOutcome::None)
          nested_outcomes[outcome.task] = outcome.outcome;
        for (const auto arm_id : instBlock(InstBlockId(value.arg1))) {
          const auto &arm = inst(arm_id);
          if (!self(self, InstBlockId(arm.arg0), nested_checked, nested_joined,
                    nested_outcomes, protocol_error))
            return false;
        }
      } else if (value.kind == SemInstKind::While) {
        if (!self(self, InstBlockId(value.arg0), successful_checked, joined,
                  outcomes, protocol_error) ||
            !self(self, InstBlockId(value.arg1), successful_checked, joined,
                  outcomes, protocol_error))
          return false;
      } else if (value.kind == SemInstKind::For) {
        for (const auto clause_id : instBlock(InstBlockId(value.arg0)))
          if (!self(self, InstBlockId(inst(clause_id).arg1), successful_checked,
                    joined, outcomes, protocol_error))
            return false;
        if (!self(self, InstBlockId(value.arg1), successful_checked, joined,
                  outcomes, protocol_error))
          return false;
      } else if (value.kind == SemInstKind::DoWhile) {
        if (!self(self, InstBlockId(value.arg1), successful_checked, joined,
                  outcomes, protocol_error) ||
            !self(self, InstBlockId(value.arg0), successful_checked, joined,
                  outcomes, protocol_error))
          return false;
      } else if (value.kind == SemInstKind::Switch) {
        for (const auto arm : instBlock(InstBlockId(value.arg1)))
          if (!self(self, InstBlockId(inst(arm).arg1), successful_checked,
                    joined, outcomes, protocol_error))
            return false;
      } else if (value.kind == SemInstKind::Defer) {
        if (!self(self, InstBlockId(value.arg0), successful_checked, joined,
                  outcomes, protocol_error))
          return false;
      }
    }
    return true;
  };
  for (std::uint32_t index = 0; index < functions_.size(); ++index) {
    const auto function_id = FunctionId(index);
    if ((function(function_id).flags & (SemFunctionCoroutineTaskDriver |
                                        SemFunctionCoroutineScaffold)) != 0 &&
        !verify_protocol_block(verify_protocol_block,
                               function(function_id).body, {}, {}, {}, error))
      return false;
  }
  const auto alpha_equivalent_type = [&](const auto &self, TypeId lhs,
                                         TypeId rhs,
                                         std::uint32_t depth = 0) -> bool {
    if (lhs == rhs)
      return true;
    if (depth > 128 || !lhs.hasValue() || !rhs.hasValue())
      return false;
    const auto &left = type(lhs);
    const auto &right = type(rhs);
    if (left.kind != right.kind)
      return false;
    if (left.kind == SemTypeKind::TypeParameter)
      return left.arg1 == right.arg1;
    if (left.kind == SemTypeKind::Array || left.kind == SemTypeKind::Slice ||
        left.kind == SemTypeKind::Reference ||
        left.kind == SemTypeKind::RawPointer)
      return left.arg1 == right.arg1 &&
             self(self, TypeId(left.arg0), TypeId(right.arg0), depth + 1);
    if (left.kind == SemTypeKind::Tuple) {
      const auto lhs_elements = typeBlock(TypeBlockId(left.arg0));
      const auto rhs_elements = typeBlock(TypeBlockId(right.arg0));
      if (lhs_elements.size() != rhs_elements.size())
        return false;
      for (std::size_t index = 0; index < lhs_elements.size(); ++index)
        if (!self(self, lhs_elements[index], rhs_elements[index], depth + 1))
          return false;
      return true;
    }
    if (left.kind == SemTypeKind::Nominal) {
      if (left.arg0 != right.arg0)
        return false;
      const auto lhs_arguments = typeBlock(TypeBlockId(left.arg1));
      const auto rhs_arguments = typeBlock(TypeBlockId(right.arg1));
      if (lhs_arguments.size() != rhs_arguments.size())
        return false;
      for (std::size_t index = 0; index < lhs_arguments.size(); ++index)
        if (!self(self, lhs_arguments[index], rhs_arguments[index], depth + 1))
          return false;
      return true;
    }
    if (left.kind == SemTypeKind::Function) {
      const auto lhs_parameters = typeBlock(TypeBlockId(left.arg0));
      const auto rhs_parameters = typeBlock(TypeBlockId(right.arg0));
      if (lhs_parameters.size() != rhs_parameters.size())
        return false;
      for (std::size_t index = 0; index < lhs_parameters.size(); ++index)
        if (!self(self, lhs_parameters[index], rhs_parameters[index],
                  depth + 1))
          return false;
      return self(self, TypeId(left.arg1), TypeId(right.arg1), depth + 1);
    }
    return false;
  };
  for (std::size_t index = 0; index < function_refs_.size(); ++index) {
    const auto &reference =
        functionRef(FunctionRefId(static_cast<std::uint32_t>(index)));
    const auto is_local = reference.local_function.hasValue();
    const auto is_imported = reference.import_ir_inst.hasValue();
    const auto is_canonical_external = reference.public_entity.hasValue();
    if ((!is_local && !is_imported && !is_canonical_external) ||
        reference.local_type.index >= types_.size() ||
        (is_imported && !reference.public_entity.hasValue()) ||
        (!is_local && !reference.public_entity.hasValue()) ||
        reference.generic.hasValue() != reference.specific.hasValue()) {
      error = "function reference does not have exactly one valid target";
      return false;
    }
    const auto &local_type = type(reference.local_type);
    if (local_type.kind != SemTypeKind::Function &&
        local_type.kind != SemTypeKind::AsyncFunction) {
      error = "function reference has an incompatible signature";
      return false;
    }
    if (is_local) {
      const auto contextual_generic_reference =
          reference.local_function.index < functions_.size() &&
          reference.generic.hasValue() &&
          !functionRefConcreteArguments(
               FunctionRefId(static_cast<std::uint32_t>(index)))
               .empty() &&
          alpha_equivalent_type(alpha_equivalent_type,
                                function(reference.local_function).type,
                                reference.local_type);
      if (reference.local_function.index >= functions_.size() ||
          (function(reference.local_function).type != reference.local_type &&
           !contextual_generic_reference)) {
        error = "local function reference does not match its declaration";
        return false;
      }
      if (!is_canonical_external)
        continue;
    }
    auto canonical = reference.public_entity;
    if (is_imported) {
      std::string canonical_error;
      canonical =
          imports_.canonicalEntity(reference.import_ir_inst, canonical_error);
      if (!canonical.hasValue() || canonical != reference.public_entity) {
        if (canonical_error.empty())
          canonical_error = "imported function has an inconsistent entity";
        error = canonical_error;
        return false;
      }
    }
    const auto *public_function = imports_.tryGetEntity(canonical);
    if (!public_function) {
      error = "imported function reference has no public entity";
      return false;
    }
    if (is_local) {
      const auto &local_function = function(reference.local_function);
      const auto declaration_specific =
          (local_function.flags & SemFunctionTemplate) != 0 &&
          local_function.generic.hasValue() &&
          local_function.specific !=
              values_->generics().generic(local_function.generic).self_specific;
      if ((local_function.flags & SemFunctionSpecific) != 0 ||
          declaration_specific)
        continue;
    }
    if (!is_local && reference.specific.hasValue()) {
      if (!reference.generic.hasValue() ||
          reference.specific.index >= values_->generics().specificCount() ||
          values_->generics().specific(reference.specific).generic !=
              reference.generic ||
          public_function->generic != reference.generic) {
        error = "imported concrete function reference has an invalid specific";
        return false;
      }
      continue;
    }
    const auto local_parameters = typeBlock(TypeBlockId(local_type.arg0));
    const auto &public_parameters = public_function->parameters;
    if (local_parameters.size() != public_parameters.size()) {
      error = "imported function reference has an incompatible signature";
      return false;
    }
    for (std::size_t parameter = 0; parameter < local_parameters.size();
         ++parameter) {
      if (!matchesPublicType(local_parameters[parameter],
                             public_parameters[parameter])) {
        error = "function reference has an incompatible parameter";
        return false;
      }
    }
    if ((local_type.kind == SemTypeKind::AsyncFunction) !=
            (public_function->execution_kind ==
             PublicFunctionExecutionKind::Async) ||
        (local_type.kind == SemTypeKind::AsyncFunction &&
         public_function->coroutine_constructor !=
             PublicCoroutineConstructorABI{1, true, true, true, true})) {
      error = "function reference has an incompatible execution contract";
      return false;
    }
    const auto local_result = local_type.kind == SemTypeKind::AsyncFunction
                                  ? asyncSuccessType(reference.local_type)
                                  : TypeId(local_type.arg1);
    if (!matchesPublicType(local_result, public_function->return_type)) {
      error = "function reference has an incompatible return type";
      return false;
    }
    if (local_type.kind == SemTypeKind::AsyncFunction) {
      const auto local_error = asyncErrorType(reference.local_type);
      if (local_error.has_value() != public_function->error_type.has_value() ||
          (local_error &&
           !matchesPublicType(*local_error, *public_function->error_type))) {
        error = "function reference has an incompatible error type";
        return false;
      }
    }
  }
  for (const auto id : instBlock(top_block_)) {
    if (id.index >= insts_.size() ||
        (inst(id).kind != SemInstKind::FunctionDecl &&
         inst(id).kind != SemInstKind::ConstantDecl)) {
      error = "top block contains a non-declaration instruction";
      return false;
    }
  }
  for (const auto &[type_index, witness] : nominal_semantic_witnesses_) {
    if (type_index >= types_.size() ||
        type(TypeId(type_index)).kind != SemTypeKind::Nominal ||
        !witness.verify(error)) {
      if (error.empty())
        error = "SemIR contains an invalid nominal semantic witness binding";
      return false;
    }
    const auto carrier = value_repr_carriers_.find(type_index);
    const auto object_carrier = object_repr_carriers_.find(type_index);
    if (witness.representation.value_repr == ValueReprKind::Custom) {
      if (carrier == value_repr_carriers_.end() ||
          carrier->second.index >= types_.size() ||
          type(carrier->second).kind == SemTypeKind::Invalid ||
          type(carrier->second).kind == SemTypeKind::Void ||
          type(carrier->second).kind == SemTypeKind::Function ||
          type(carrier->second).kind == SemTypeKind::TypeParameter ||
          type(carrier->second).kind == SemTypeKind::Count) {
        error = "SemIR custom value witness has an invalid local carrier";
        return false;
      }
    } else if (carrier != value_repr_carriers_.end()) {
      error = "SemIR non-custom value witness has a local carrier";
      return false;
    }
    if (witness.representation.object_repr == ObjectReprKind::Custom) {
      if (object_carrier == object_repr_carriers_.end() ||
          object_carrier->second.index >= types_.size() ||
          type(object_carrier->second).kind != SemTypeKind::Nominal) {
        error = "SemIR custom object witness has an invalid local carrier";
        return false;
      }
      const auto projections = object_field_projections_.find(type_index);
      if (projections == object_field_projections_.end() ||
          projections->second.size() !=
              nominalType(NominalTypeId(type(TypeId(type_index)).arg0))
                  .fields.size()) {
        error = "SemIR custom object witness has invalid local projections";
        return false;
      }
      for (std::size_t projection_index = 0;
           projection_index < projections->second.size(); ++projection_index)
        if (projections->second[projection_index].empty() &&
            witness.object_field_projections[projection_index].kind !=
                ObjectFieldProjectionKind::Computed) {
          error = "SemIR addressable object projection has no physical path";
          return false;
        }
    } else if (object_carrier != object_repr_carriers_.end()) {
      error = "SemIR non-custom object witness has a local carrier";
      return false;
    }
  }
  for (const auto &entry : value_repr_carriers_)
    if (!nominal_semantic_witnesses_.contains(entry.first)) {
      error = "SemIR value carrier has no nominal semantic witness";
      return false;
    }
  for (const auto &entry : object_repr_carriers_)
    if (!nominal_semantic_witnesses_.contains(entry.first)) {
      error = "SemIR object carrier has no nominal semantic witness";
      return false;
    }
  for (const auto &entry : object_field_projections_)
    if (!nominal_semantic_witnesses_.contains(entry.first)) {
      error = "SemIR object projection has no nominal semantic witness";
      return false;
    }
  if (!verifyCarrierViews(*this, error))
    return false;
  return true;
}




} // namespace chtholly::compiler
