#include "chtholly/Compiler/LowIR.h"

#include "chtholly/Compiler/BuiltinOperator.h"
#include "chtholly/Compiler/CallableOwnership.h"

#include "LowIRVerificationContext.h"

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
constexpr auto Names = std::to_array<std::string_view>({
#define CHTHOLLY_COMPILER_LOW_INST(Name, Arg0, Arg1) #Name,
#include "chtholly/Compiler/LowIRKind.def"
});
constexpr auto Args = std::to_array<std::array<LowArgKind, 2>>({
#define CHTHOLLY_COMPILER_LOW_INST(Name, Arg0, Arg1)                               \
  std::array{LowArgKind::Arg0, LowArgKind::Arg1},
#include "chtholly/Compiler/LowIRKind.def"
});
static_assert(Names.size() == static_cast<std::size_t>(LowInstKind::Count));
static_assert(Args.size() == static_cast<std::size_t>(LowInstKind::Count));

} // namespace



bool LowIR::verify(std::string &error) const {
  internal::LowIRVerificationContext verification(*this);
  if (!verification.verifyPreconditions(error))
    return false;
  const auto verify_outcome = [&](OutcomeDescriptorId id,
                                  OutcomeCardinality expected,
                                  std::string_view owner) {
    const auto descriptor_ref = outcomeDescriptorRef(id);
    const auto *descriptor = checkedOutcomeDescriptor(descriptor_ref);
    if (!descriptor) {
      error = "LowIR " + std::string(owner) +
              " has no canonical outcome descriptor";
      return false;
    }
    if (!descriptor->verify(error)) {
      error = "LowIR " + std::string(owner) + " has invalid outcome: " +
              error;
      return false;
    }
    if (descriptor->cardinality != expected) {
      error = "LowIR " + std::string(owner) +
              " has an outcome with the wrong cardinality";
      return false;
    }
    return true;
  };
  for (const auto &plan : foreign_operation_plans_.values())
    if (!verify_outcome(plan.outcome, OutcomeCardinality::OneShot,
                        "foreign operation plan"))
      return false;
  if (payload_operation_plans_.size() != sem_ir_->typedChannelDescriptors().size()) {
    error = "LowIR payload plans do not cover semantic descriptors";
    return false;
  }
  std::size_t payload_index = 0;
  for (const auto &plan : payload_operation_plans_.values()) {
    const auto &source = sem_ir_->typedChannelDescriptors()[payload_index++];
    ComponentAbi2Descriptor expected;
    expected.component_identity = source.component_identity;
    expected.entity_identity = source.operation_identity;
    expected.resource_identity = source.operation_identity;
    expected.operation_kind = source.operation_kind;
    expected.lease_policy = source.lease_policy;
    expected.ownership_flags = source.ownership_flags;
    expected.payload_type_digest = source.payload_type_fingerprint;
    expected.layout_digest = source.layout_fingerprint;
    expected.lifecycle_digest = source.lifecycle_fingerprint;
    expected.contract_digest = source.component_descriptor_digest;
    expected.runtime_abi_digest = StableFingerprint::fromCanonicalBytes("runtime-v1");
    if (plan.descriptor_digest != source.component_descriptor_digest ||
        plan.payload_type_digest != source.payload_type_fingerprint ||
        plan.layout_digest != source.layout_fingerprint ||
        plan.lifecycle_digest != source.lifecycle_fingerprint ||
        plan.contract_digest != source.component_descriptor_digest ||
        plan.operation_kind != source.operation_kind || plan.lease_policy != source.lease_policy ||
        !plan.source_preserved_until_commit ||
        plan.destination_initializes_on_commit != (source.operation_kind == ComponentAbi2OperationKind::Receive)) {
      error = "LowIR payload plan disagrees with semantic ownership";
      return false;
    }

    if (!verify_outcome(plan.outcome, OutcomeCardinality::MultiSubmit,
                        "payload operation plan"))
      return false;
    if (!plan.plan_fingerprint.hasValue() || !plan.descriptor_digest.hasValue() ||
        !plan.payload_type_digest.hasValue() || !plan.layout_digest.hasValue() ||
        !plan.lifecycle_digest.hasValue() || !plan.contract_digest.hasValue() ||
        plan.operation_kind == ComponentAbi2OperationKind::Invoke ||
        plan.lease_policy > ComponentAbi2LeasePolicy::Shared ||
        plan.source_lane == core::AnyId::InvalidIndex ||
        plan.token_lane == core::AnyId::InvalidIndex) {
      error = "LowIR payload operation plan has invalid descriptor facts";
      return false;
    }
    const auto *outcome = checkedOutcomeDescriptor(plan.outcome);
    if (outcome && plan.plan_fingerprint != componentAbi2PayloadPlanDigest(
            expected, outcome->fingerprint(), plan.source_lane, plan.destination_lane,
            plan.token_lane, plan.source_preserved_until_commit, plan.destination_initializes_on_commit)) {
      error = "LowIR payload plan fingerprint mismatch";
      return false;
    }
    if (!outcome || !std::ranges::any_of(outcome->transitions, [&](const auto &t) {
          return t.consumes_token && t.token_lane == plan.token_lane;
        })) {
      error = "LowIR payload operation plan has no matching token lane";
      return false;
    }
  }
  for (const auto &plan : foreign_call_outcome_plans_.values())
    if (!verify_outcome(plan.outcome, OutcomeCardinality::OneShot,
                        "foreign call outcome plan"))
      return false;
  for (const auto &plan : foreign_operation_completion_plans_.values())
    if (!verify_outcome(plan.outcome, OutcomeCardinality::OneShot,
                        "foreign operation completion plan"))
      return false;
  for (const auto &plan : coroutine_task_completion_arm_plans_.values())
    if (!verify_outcome(plan.outcome, OutcomeCardinality::OneShot,
                        "coroutine completion arm plan"))
      return false;
  for (const auto &plan : coroutine_task_completion_set_plans_.values())
    if (!verify_outcome(plan.outcome, OutcomeCardinality::MultiSubmit,
                        "coroutine completion set plan"))
      return false;
  for (const auto &plan : coroutine_task_completion_combine_plans_.values())
    if (!verify_outcome(plan.outcome, OutcomeCardinality::MultiSubmit,
                        "coroutine completion combine plan"))
      return false;
  if (!verification.verifyStructure(error))
    return false;
  if (!verification.verifyCoroutinePlans(error))
    return false;
  if (!verification.verifyRepresentations(error) ||
      !verification.verifyAbiIndexes(error))
    return false;
  if (!verification.verifyForeignPlans(error))
    return false;
  const auto expected_capability = [](CallableSemanticRole role) {
    return role == CallableSemanticRole::None
               ? static_cast<std::uint16_t>(CallableCapabilityNone)
               : static_cast<std::uint16_t>(
                     1U << (static_cast<unsigned>(role) - 1U));
  };
  const auto target_contract_matches =
      [&](FunctionRefId target, TypeId owner, CallableSemanticDomain domain,
          CallableSemanticRole role, std::uint32_t projector_field,
          bool whole_carrier, std::span<const std::uint32_t> carrier_path,
          bool has_bit_range, std::uint32_t bit_begin, std::uint32_t bit_end) {
        if (!target.hasValue() || target.index >= sem_ir_->functionRefCount())
          return false;
        const auto &reference = sem_ir_->functionRef(target);
        const auto &function_type = sem_ir_->type(reference.local_type);
        if (function_type.kind != SemTypeKind::Function)
          return false;
        const auto parameters =
            sem_ir_->typeBlock(TypeBlockId(function_type.arg0));
        if (parameters.empty() ||
            sem_ir_->type(parameters.front()).kind != SemTypeKind::Reference ||
            sem_ir_->referencePointee(parameters.front()) != owner)
          return false;
        if (reference.local_function.hasValue()) {
          const auto &contract =
              sem_ir_->functionSemanticContract(reference.local_function);
          return contract.domain == domain && contract.role == role &&
                 contract.capability == expected_capability(role) &&
                 contract.projector_field == projector_field &&
                 contract.whole_carrier == whole_carrier &&
                 std::ranges::equal(contract.carrier_path, carrier_path) &&
                 contract.has_bit_range == has_bit_range &&
                 contract.bit_begin == bit_begin && contract.bit_end == bit_end;
        }
        const auto *entity =
            sem_ir_->importIRs().tryGetEntity(reference.public_entity);
        return entity && entity->semantic_contract.domain == domain &&
               entity->semantic_contract.role == role &&
               entity->semantic_contract.capability ==
                   expected_capability(role) &&
               entity->semantic_contract.projector_field == projector_field &&
               entity->semantic_contract.whole_carrier == whole_carrier &&
               std::ranges::equal(entity->semantic_contract.carrier_path,
                                  carrier_path) &&
               entity->semantic_contract.has_bit_range == has_bit_range &&
               entity->semantic_contract.bit_begin == bit_begin &&
               entity->semantic_contract.bit_end == bit_end;
      };
  const auto matches_lifecycle_target = [&](FunctionRefId target, TypeId type,
                                            SemCanonicalFunctionRole role) {
    if (!target.hasValue() || target.index >= sem_ir_->functionRefCount() ||
        sem_ir_->type(type).kind != SemTypeKind::Nominal)
      return false;
    const std::span<const std::uint32_t> no_path;
    const bool intrinsic_vec_drop = role == SemCanonicalFunctionRole::Drop &&
                                    sem_ir_->functionIntrinsicRole(target) ==
                                        CompilerIntrinsicRole::VecDrop;
    if (!intrinsic_vec_drop &&
        !target_contract_matches(
            target, type, CallableSemanticDomain::Lifecycle, role,
            core::AnyId::InvalidIndex, false, no_path, false, 0, 0))
      return false;
    const auto &reference = sem_ir_->functionRef(target);
    const auto &function_type = sem_ir_->type(reference.local_type);
    if (function_type.kind != SemTypeKind::Function ||
        TypeId(function_type.arg1) != sem_ir_->voidType())
      return false;
    const auto parameters = sem_ir_->typeBlock(TypeBlockId(function_type.arg0));
    const auto matches_reference = [&](TypeId parameter,
                                       SemReferenceMutability mutability) {
      return sem_ir_->type(parameter).kind == SemTypeKind::Reference &&
             sem_ir_->referencePointee(parameter) == type &&
             sem_ir_->referenceMutability(parameter) == mutability;
    };
    if ((role == SemCanonicalFunctionRole::Copy &&
         (parameters.size() != 2 ||
          !matches_reference(parameters[0], SemReferenceMutability::Mutable) ||
          !matches_reference(parameters[1],
                             SemReferenceMutability::ReadOnly))) ||
        (role == SemCanonicalFunctionRole::Drop &&
         (parameters.size() != 1 ||
          !matches_reference(parameters[0], SemReferenceMutability::Mutable))))
      return false;
    if (reference.local_function.hasValue()) {
      const auto &contract =
          sem_ir_->functionSemanticContract(reference.local_function);
      return contract.owner == NominalTypeId(sem_ir_->type(type).arg0) &&
             contract.role == role;
    }
    const auto *witness = sem_ir_->nominalSemanticWitness(type);
    const auto *expected = witness && role == SemCanonicalFunctionRole::Copy &&
                                   witness->copy_target
                               ? &*witness->copy_target
                           : witness &&
                                   role == SemCanonicalFunctionRole::Drop &&
                                   witness->destroy_target
                               ? &*witness->destroy_target
                               : nullptr;
    const auto *entity =
        sem_ir_->importIRs().tryGetEntity(reference.public_entity);
    return expected && entity &&
           sem_ir_->identifier(entity->package_name) ==
               expected->canonical_package &&
           sem_ir_->identifier(entity->module_name) ==
               expected->canonical_module &&
           sem_ir_->identifier(entity->name) == expected->canonical_name &&
           entity->fingerprint == expected->expected_fingerprint;
  };
  const auto matches_conversion_target = [&](FunctionRefId target, TypeId type,
                                             SemCanonicalFunctionRole role) {
    if (!target.hasValue() || target.index >= sem_ir_->functionRefCount() ||
        sem_ir_->type(type).kind != SemTypeKind::Nominal)
      return false;
    const std::span<const std::uint32_t> no_path;
    if (!target_contract_matches(
            target, type, CallableSemanticDomain::ValueRepresentation, role,
            core::AnyId::InvalidIndex, true, no_path, false, 0, 0))
      return false;
    const auto &representation = typeRepresentation(type);
    if (representation.facts.value_repr != ValueReprKind::Custom ||
        (role == SemCanonicalFunctionRole::Pack
             ? representation.pack_target != target
             : representation.init_target != target))
      return false;
    const auto &function_type =
        sem_ir_->type(sem_ir_->functionRef(target).local_type);
    if (function_type.kind != SemTypeKind::Function)
      return false;
    const auto parameters = sem_ir_->typeBlock(TypeBlockId(function_type.arg0));
    const auto owner_ref = [&](TypeId parameter,
                               SemReferenceMutability mutability) {
      return sem_ir_->type(parameter).kind == SemTypeKind::Reference &&
             sem_ir_->referencePointee(parameter) == type &&
             sem_ir_->referenceMutability(parameter) == mutability;
    };
    return role == SemCanonicalFunctionRole::Pack
               ? parameters.size() == 1 &&
                     owner_ref(parameters[0],
                               SemReferenceMutability::ReadOnly) &&
                     TypeId(function_type.arg1) == representation.value_type
               : parameters.size() == 2 &&
                     owner_ref(parameters[0],
                               SemReferenceMutability::Mutable) &&
                     parameters[1] == representation.value_type &&
                     TypeId(function_type.arg1) == sem_ir_->voidType();
  };
  std::vector<FunctionId> instruction_owners;
  std::vector<CoroutineCleanupGraphId> instruction_cleanup_graph;
  if (!verification.verifyCleanupGraphs(instruction_owners,
                                        instruction_cleanup_graph, error))
    return false;
  for (std::size_t index = 0; index < insts_.size(); ++index) {
    const auto id = LowInstId(static_cast<std::uint32_t>(index));
    const auto &value = inst(id);
    if (value.kind == LowInstKind::Invalid ||
        static_cast<std::size_t>(value.kind) >= Names.size() ||
        value.type >= sem_ir_->typeCount() ||
        origin(id).index >= sem_ir_->instCount() ||
        !containsArg(lowInstArgKind(value.kind, 0), value.arg0) ||
        !containsArg(lowInstArgKind(value.kind, 1), value.arg1)) {
      error = "low instruction has an invalid typed record at index " +
              std::to_string(index) + " (kind " +
              std::to_string(static_cast<std::uint32_t>(value.kind)) +
              ", arg0 " + std::to_string(value.arg0) + ", arg1 " +
              std::to_string(value.arg1) + ")";
      return false;
    }
    if (const auto graph_id = instruction_cleanup_graph[index];
        graph_id.hasValue()) {
      for (std::size_t argument = 0; argument != 2; ++argument) {
        const auto raw = argument == 0 ? value.arg0 : value.arg1;
        const auto kind = lowInstArgKind(value.kind, argument);
        const auto belongs_to_graph = [&](LowInstId operand) {
          return instruction_cleanup_graph[operand.index] == graph_id;
        };
        if ((kind == LowArgKind::Value && !belongs_to_graph(LowInstId(raw))) ||
            (kind == LowArgKind::ValueBlock &&
             !std::ranges::all_of(valueBlock(LowValueBlockId(raw)),
                                  belongs_to_graph))) {
          error = "LowIR coroutine cleanup graph captures external SSA";
          return false;
        }
      }
    }
    const auto instruction_type = TypeId(value.type);
    const auto value_type = [&](std::uint32_t raw) {
      return TypeId(inst(LowInstId(raw)).type);
    };
    if (!verification.verifyForeignInstruction(id, error))
      return false;
    if (!verification.verifyCoroutineInstruction(id, instruction_owners, error))
      return false;
    if (!verification.verifyObjectInstruction(id, error))
      return false;
    if (!verification.verifyScalarInstruction(id, error))
      return false;
    switch (value.kind) {
    case LowInstKind::VoidValue:
      if (instruction_type != sem_ir_->voidType()) {
        error = "void value has a non-void type";
        return false;
      }
      break;
    case LowInstKind::IntegerConstant:
      if (sem_ir_->type(instruction_type).kind != SemTypeKind::Integer &&
          sem_ir_->type(instruction_type).kind != SemTypeKind::Char &&
          (!sem_ir_->foreignRepresentationType(instruction_type).hasValue() ||
           sem_ir_->type(sem_ir_->foreignRepresentationType(instruction_type))
                   .kind != SemTypeKind::Integer)) {
        error = "integer constant has an invalid type";
        return false;
      }
      break;
    case LowInstKind::FloatConstant:
      if (sem_ir_->type(instruction_type).kind != SemTypeKind::Float) {
        error = "floating-point constant has an invalid type";
        return false;
      }
      break;
    case LowInstKind::BoolConstant:
      if (instruction_type != sem_ir_->boolType()) {
        error = "boolean constant has an invalid type";
        return false;
      }
      break;
    case LowInstKind::StringConstant:
      if (instruction_type != sem_ir_->stringType() &&
          sem_ir_->type(instruction_type).kind != SemTypeKind::RawPointer) {
        error = "string constant has an invalid type";
        return false;
      }
      break;
    case LowInstKind::TypedChannelSendPrepare:
    case LowInstKind::TypedChannelSendCommit:
    case LowInstKind::TypedChannelSendCancel:
    case LowInstKind::TypedChannelReceiveAcquire:
    case LowInstKind::TypedChannelReceiveCommit:
    case LowInstKind::TypedChannelReceiveCancel:
    case LowInstKind::TypedChannelClose: {
      if (instruction_type != sem_ir_->voidType()) {
        error = "typed channel transition must have void type";
        return false;
      }
      const auto semantic_origin = origin(id);
      if (!semantic_origin.hasValue()) {
        error = "typed channel transition has no semantic origin";
        return false;
      }
      const auto expected_kind = [&] {
        switch (value.kind) {
        case LowInstKind::TypedChannelSendPrepare:
          return SemInstKind::TypedChannelSendPrepare;
        case LowInstKind::TypedChannelSendCommit:
          return SemInstKind::TypedChannelSendCommit;
        case LowInstKind::TypedChannelSendCancel:
          return SemInstKind::TypedChannelSendCancel;
        case LowInstKind::TypedChannelReceiveAcquire:
          return SemInstKind::TypedChannelReceiveAcquire;
        case LowInstKind::TypedChannelReceiveCommit:
          return SemInstKind::TypedChannelReceiveCommit;
        case LowInstKind::TypedChannelReceiveCancel:
          return SemInstKind::TypedChannelReceiveCancel;
        case LowInstKind::TypedChannelClose:
          return SemInstKind::TypedChannelClose;
        default:
          return SemInstKind::Invalid;
        }
      }();
      const auto &semantic = sem_ir_->inst(semantic_origin);
      if (semantic.kind != expected_kind ||
          origin(LowInstId(value.arg0)) != InstId(semantic.arg0) ||
          ((value.kind == LowInstKind::TypedChannelSendPrepare ||
            value.kind == LowInstKind::TypedChannelSendCommit ||
            value.kind == LowInstKind::TypedChannelReceiveCommit) &&
           origin(LowInstId(value.arg1)) != InstId(semantic.arg1))) {
        error = "typed channel LowIR transition disagrees with SemIR";
        return false;
      }
      break;
    }
    case LowInstKind::NullPointer:
      if (sem_ir_->type(instruction_type).kind != SemTypeKind::RawPointer &&
          sem_ir_->type(instruction_type).kind !=
              SemTypeKind::CoroutineExecutor) {
        error = "null pointer has an invalid type";
        return false;
      }
      break;
    case LowInstKind::CompilerIntrinsicCall: {
      const auto target = FunctionRefId(value.arg0);
      if (!target.hasValue() || target.index >= sem_ir_->functionRefCount() ||
          sem_ir_->functionIntrinsicRole(target) ==
              CompilerIntrinsicRole::None) {
        error = "low compiler intrinsic call has an invalid target";
        return false;
      }
      const auto &function_type =
          sem_ir_->type(sem_ir_->functionRef(target).local_type);
      const auto arguments = valueBlock(LowValueBlockId(value.arg1));
      const auto parameters =
          function_type.kind == SemTypeKind::Function
              ? sem_ir_->typeBlock(TypeBlockId(function_type.arg0))
              : std::span<const TypeId>{};
      if (function_type.kind != SemTypeKind::Function ||
          instruction_type.index != function_type.arg1 ||
          arguments.size() != parameters.size()) {
        error = "low compiler intrinsic call disagrees with its signature";
        return false;
      }
      const auto semantic_origin = origin(id);
      if (!semantic_origin.hasValue() ||
          sem_ir_->inst(semantic_origin).kind !=
              SemInstKind::CompilerIntrinsicCall ||
          sem_ir_->inst(semantic_origin).arg0 != target.index) {
        error = "low compiler intrinsic call has no matching semantic origin";
        return false;
      }
      const auto semantic_arguments =
          sem_ir_->instBlock(InstBlockId(sem_ir_->inst(semantic_origin).arg1));
      if (semantic_arguments.size() != arguments.size()) {
        error = "low compiler intrinsic operands disagree with SemIR";
        return false;
      }
      for (std::size_t argument = 0; argument < arguments.size(); ++argument) {
        if (arguments[argument].index >= insts_.size() ||
            TypeId(inst(arguments[argument]).type) != parameters[argument] ||
            origin(arguments[argument]) != semantic_arguments[argument]) {
          error = "low compiler intrinsic has a mistyped or replaced operand";
          return false;
        }
      }
      const auto role = sem_ir_->functionIntrinsicRole(target);
      const auto memory_order =
          [&](LowInstId operand) -> std::optional<std::uint8_t> {
        if (!operand.hasValue() || operand.index >= insts_.size())
          return std::nullopt;
        const auto &lowered = inst(operand);
        if (lowered.kind != LowInstKind::MakeEnum ||
            lowered.arg1 >= sem_ir_->integerCount())
          return std::nullopt;
        const auto discriminant = sem_ir_->integer(IntegerId(lowered.arg1));
        if (discriminant < 0 || discriminant > 4)
          return std::nullopt;
        return static_cast<std::uint8_t>(discriminant);
      };
      std::array<std::uint8_t, 2> orderings{};
      for (std::uint8_t ordering = 0;
           ordering < compilerIntrinsicOrderingCount(role); ++ordering) {
        const auto parameter =
            compilerIntrinsicOrderingParameter(role, ordering);
        const auto order = parameter < arguments.size()
                               ? memory_order(arguments[parameter])
                               : std::nullopt;
        if (!order ||
            !isValidCompilerIntrinsicOrdering(role, ordering, *order)) {
          error = "low compiler intrinsic has an invalid memory ordering";
          return false;
        }
        orderings[ordering] = *order;
      }
      if (compilerIntrinsicOrderingCount(role) == 2 &&
          !isValidCompareExchangeOrderingPair(orderings[0], orderings[1])) {
        error = "low compare-exchange ordering pair is invalid";
        return false;
      }
      break;
    }
    case LowInstKind::Construct: {
      const auto &plan = construct_plans_.get(ConstructPlanId(value.arg0));
      const auto &target = sem_ir_->functionRef(plan.target);
      const auto &target_type = sem_ir_->type(target.local_type);
      const auto *imported =
          target.local_function.hasValue()
              ? nullptr
              : sem_ir_->importIRs().tryGetEntity(target.public_entity);
      const auto constructor =
          target.local_function.hasValue()
              ? sem_ir_->functionSemanticContract(target.local_function).role ==
                    CallableSemanticRole::Constructor
              : imported && imported->semantic_contract.role ==
                                CallableSemanticRole::Constructor;
      const auto arguments = valueBlock(plan.arguments);
      if (!target.local_type.hasValue() ||
          target_type.kind != SemTypeKind::Function || !constructor ||
          !plan.destination.hasValue() ||
          (place(plan.destination).flags & LowPlaceAddressable) == 0 ||
          sem_ir_->typeRepresentation(place(plan.destination).type).init_repr !=
              InitReprKind::InPlace) {
        error = "low construct plan has invalid destination or constructor";
        return false;
      }
      const auto return_type = TypeId(target_type.arg1);
      const auto result_shape = sem_ir_->canonicalResultShape(return_type);
      const auto expected_outcome =
          result_shape && result_shape->success == place(plan.destination).type
              ? sem_ir_->canonicalResultOutcomeType(return_type)
              : sem_ir_->voidType();
      if ((result_shape &&
           result_shape->success != place(plan.destination).type) ||
          (!result_shape && return_type != place(plan.destination).type) ||
          !expected_outcome.hasValue() ||
          instruction_type != expected_outcome) {
        error = "low construct result protocol does not match its destination";
        return false;
      }
      const auto parameters = sem_ir_->typeBlock(TypeBlockId(target_type.arg0));
      if (arguments.size() != parameters.size()) {
        error = "low construct plan has invalid argument arity";
        return false;
      }
      for (std::size_t parameter_index = 0; parameter_index < parameters.size();
           ++parameter_index)
        if (arguments[parameter_index].index >= insts_.size() ||
            TypeId(inst(arguments[parameter_index]).type) !=
                parameters[parameter_index]) {
          error = "low construct plan has a mistyped argument";
          return false;
        }
      break;
    }
    case LowInstKind::ForeignResourceWrap:
    case LowInstKind::ForeignResourceUnwrap:
    case LowInstKind::ForeignResourceValid:
    case LowInstKind::FinishForeignResource:
    case LowInstKind::FinishForeignCompletion:
      // These instructions preserve the already verified SemIR resource
      // boundary. Their operand and result facts are checked by SemIR, while
      // LowIR retains the typed operation for target lowering.
      break;
    case LowInstKind::TransferReturn:
      if (instruction_type != sem_ir_->voidType() ||
          sem_ir_->typeRepresentation(value_type(value.arg0)).init_repr !=
              InitReprKind::InPlace) {
        error = "return transfer does not target in-place storage";
        return false;
      }
      break;
    case LowInstKind::Destroy:
    case LowInstKind::LifecycleDestroy:
    case LowInstKind::EndLifetime: {
      const auto action_type = value.kind == LowInstKind::EndLifetime
                                   ? slot(SlotId(value.arg0)).type
                               : value.kind == LowInstKind::LifecycleDestroy
                                   ? place(LowPlaceId(value.arg1)).type
                                   : place(LowPlaceId(value.arg0)).type;
      const auto representation = sem_ir_->typeRepresentation(action_type);
      if (instruction_type != sem_ir_->voidType() ||
          representation.ownership != OwnershipReprKind::Owned ||
          (value.kind != LowInstKind::EndLifetime &&
           representation.destroy == DestroyReprKind::None)) {
        error = "lifecycle action targets a non-owning slot";
        return false;
      }
      if (value.kind != LowInstKind::EndLifetime) {
        const auto target = value.kind == LowInstKind::LifecycleDestroy
                                ? LowPlaceId(value.arg1)
                                : LowPlaceId(value.arg0);
        if ((place(target).flags & LowPlaceAddressable) == 0) {
          error = "place destroy targets a non-addressable logical place";
          return false;
        }
      }
      if (value.kind == LowInstKind::LifecycleDestroy &&
          (representation.destroy != DestroyReprKind::Custom ||
           !matches_lifecycle_target(FunctionRefId(value.arg0), action_type,
                                     SemCanonicalFunctionRole::Drop))) {
        error =
            "custom destroy action does not match its nominal semantic witness";
        return false;
      }
      break;
    }
    case LowInstKind::IsInitialized:
      if (instruction_type != sem_ir_->boolType()) {
        error = "logical initialization test has a non-boolean type";
        return false;
      }
      break;
    case LowInstKind::MarkInitialized:
    case LowInstKind::MarkMoved:
      if (instruction_type != sem_ir_->voidType()) {
        error = "logical initialization transition has a non-void type";
        return false;
      }
      break;
    case LowInstKind::DestroyValue:
    case LowInstKind::LifecycleDestroyValue: {
      const auto operand = value.kind == LowInstKind::DestroyValue
                               ? LowInstId(value.arg0)
                               : LowInstId(value.arg1);
      const auto action_type = value_type(operand.index);
      const auto representation = sem_ir_->typeRepresentation(action_type);
      if (instruction_type != sem_ir_->voidType() ||
          representation.ownership != OwnershipReprKind::Owned ||
          representation.destroy == DestroyReprKind::None) {
        error = "value destroy targets a non-owning value";
        return false;
      }
      if (value.kind == LowInstKind::LifecycleDestroyValue &&
          (representation.destroy != DestroyReprKind::Custom ||
           !matches_lifecycle_target(FunctionRefId(value.arg0), action_type,
                                     SemCanonicalFunctionRole::Drop))) {
        error = "value destroy does not match its lifecycle witness";
        return false;
      }
      break;
    }
    case LowInstKind::Branch:
    case LowInstKind::BranchIf:
    case LowInstKind::Return:
    case LowInstKind::ReturnInPlace:
    case LowInstKind::Unreachable:
    case LowInstKind::FatalFailure:
    case LowInstKind::CoroutineCleanupEnd:
      if (instruction_type != sem_ir_->voidType()) {
        error = "terminator has a non-void result type";
        return false;
      }
      if (value.kind == LowInstKind::BranchIf &&
          value_type(value.arg0) != sem_ir_->boolType()) {
        error = "conditional branch has a non-boolean condition";
        return false;
      }
      if (value.kind == LowInstKind::FatalFailure) {
        const auto reason = sem_ir_->integer(IntegerId(value.arg0));
        if (!isValidUnrecoverableFailureReason(reason)) {
          error = "fatal failure has an invalid reason";
          return false;
        }
      }
      break;
    case LowInstKind::Invalid:
    case LowInstKind::Count:
      error = "low instruction has an invalid kind";
      return false;
    }
  }
  if (!verification.verifyBlocks(error))
    return false;
  for (std::size_t index = 0; index < functions_.size(); ++index) {
    const auto &value =
        function(LowFunctionId(static_cast<std::uint32_t>(index)));
    if (value.semantic_function.index >= sem_ir_->functionCount() ||
        value.entry.index >= blocks_.size() ||
        value.blocks.index >= block_lists_.size() ||
        value.slots.index >= slot_blocks_.size()) {
      error = "low function has an invalid record";
      return false;
    }
    bool found_entry = false;
    std::unordered_set<std::uint32_t> function_blocks;
    for (const auto block_id : blockList(value.blocks)) {
      if (block_id.index >= blocks_.size()) {
        error = "low function contains an invalid block";
        return false;
      }
      function_blocks.insert(block_id.index);
      found_entry = found_entry || block_id == value.entry;
    }
    if (!found_entry) {
      error = "low function block list omits its entry";
      return false;
    }
    const auto &semantic_function = sem_ir_->function(value.semantic_function);
    const auto &semantic_contract =
        sem_ir_->functionSemanticContract(value.semantic_function);
    const auto semantic_role = semantic_contract.role;
    const auto expected_return =
        sem_ir_->type(semantic_function.type).kind == SemTypeKind::AsyncFunction
            ? sem_ir_->asyncSuccessType(semantic_function.type)
            : TypeId(sem_ir_->type(semantic_function.type).arg1);
    const auto semantic_parameters = sem_ir_->localBlock(
        sem_ir_->function(value.semantic_function).parameters);
    const auto entry_instructions = block(value.entry);
    if (entry_instructions.size() < semantic_parameters.size()) {
      error = "low function omits a parameter binding";
      return false;
    }
    std::unordered_set<std::uint32_t> parameter_prefix;
    std::size_t parameter_position = 0;
    for (std::size_t parameter = 0; parameter < semantic_parameters.size();
         ++parameter) {
      const auto semantic_parameter = semantic_parameters[parameter];
      const auto parameter_type = sem_ir_->local(semantic_parameter).type;
      const auto converted =
          typeRepresentation(parameter_type).facts.init_repr ==
          InitReprKind::ByConversion;
      const auto required = converted ? 2U : 1U;
      if (parameter_position + required > entry_instructions.size()) {
        error = "low function omits a converted parameter binding";
        return false;
      }
      const auto first_id = entry_instructions[parameter_position++];
      parameter_prefix.insert(first_id.index);
      const auto &first = inst(first_id);
      if (!converted) {
        if (first.kind != LowInstKind::Parameter ||
            first.arg1 != static_cast<std::uint32_t>(parameter)) {
          error = "low function parameters are not an entry-block prefix";
          return false;
        }
        const auto &parameter_slot = slot(SlotId(first.arg0));
        if (parameter_slot.semantic_local != semantic_parameter ||
            parameter_slot.type != parameter_type) {
          error = "low function has an invalid parameter binding";
          return false;
        }
      } else {
        const auto initialize_id = entry_instructions[parameter_position++];
        parameter_prefix.insert(initialize_id.index);
        const auto &initialize = inst(initialize_id);
        if (first.kind != LowInstKind::ParameterValue ||
            first.type != parameter_type.index ||
            first.arg0 != static_cast<std::uint32_t>(parameter) ||
            initialize.kind != LowInstKind::InitializeFromValue ||
            initialize.arg1 != first_id.index ||
            slot(SlotId(initialize.arg0)).semantic_local !=
                semantic_parameter ||
            slot(SlotId(initialize.arg0)).type != parameter_type) {
          error = "low function has an invalid converted parameter binding";
          return false;
        }
      }
    }
    for (const auto block_id : blockList(value.blocks)) {
      const auto instructions = block(block_id);
      for (std::size_t position = 0; position < instructions.size();
           ++position) {
        const auto &instruction = inst(instructions[position]);
        if ((instruction.kind == LowInstKind::Parameter ||
             instruction.kind == LowInstKind::ParameterValue) &&
            !parameter_prefix.contains(instructions[position].index)) {
          error = "low function has a parameter outside its entry prefix";
          return false;
        }
        if (instruction.kind == LowInstKind::DereferenceObject) {
          const auto &object_type = sem_ir_->type(TypeId(instruction.type));
          if (!(semantic_role == SemCanonicalFunctionRole::Pack ||
                semantic_role == SemCanonicalFunctionRole::Init) ||
              !semantic_contract.owner.hasValue() ||
              object_type.kind != SemTypeKind::Nominal ||
              NominalTypeId(object_type.arg0) != semantic_contract.owner) {
            error = "object dereference is outside its representation witness";
            return false;
          }
        }
        if (instruction.kind == LowInstKind::CarrierView) {
          const auto allowed =
              (semantic_role >= SemCanonicalFunctionRole::ProjectionLoad &&
               semantic_role <=
                   SemCanonicalFunctionRole::ProjectionBorrowMut) ||
              (semantic_role >= SemCanonicalFunctionRole::ObjectInit &&
               semantic_role <= SemCanonicalFunctionRole::ObjectDrop);
          const auto source = LowInstId(instruction.arg0);
          const auto &source_inst = inst(source);
          const auto input_type = TypeId(source_inst.type);
          if (!allowed || !semantic_contract.owner.hasValue()) {
            error = "carrier view is outside a canonical object role";
            return false;
          }
          if (semantic_parameters.empty() ||
              source_inst.kind != LowInstKind::Load) {
            error = "carrier view is not loaded from a canonical parameter";
            return false;
          }
          if (slot(SlotId(source_inst.arg0)).semantic_local !=
              semantic_parameters.front()) {
            error = "carrier view does not use the first canonical parameter";
            return false;
          }
          if (sem_ir_->type(input_type).kind != SemTypeKind::Reference ||
              sem_ir_->type(TypeId(instruction.type)).kind !=
                  SemTypeKind::Reference) {
            error = "carrier view does not preserve a reference ABI";
            return false;
          }
          const auto owner_type = sem_ir_->referencePointee(input_type);
          const auto &owner_sem_type = sem_ir_->type(owner_type);
          if (owner_sem_type.kind != SemTypeKind::Nominal ||
              NominalTypeId(owner_sem_type.arg0) != semantic_contract.owner ||
              sem_ir_->referencePointee(TypeId(instruction.type)) !=
                  typeRepresentation(owner_type).object_type ||
              sem_ir_->referenceMutability(input_type) !=
                  sem_ir_->referenceMutability(TypeId(instruction.type))) {
            error = "carrier view does not match its object representation";
            return false;
          }
          const auto field_count = static_cast<std::uint32_t>(
              sem_ir_->nominalType(semantic_contract.owner).fields.size());
          const auto projection =
              semantic_role >= SemCanonicalFunctionRole::ProjectionLoad &&
              semantic_role <= SemCanonicalFunctionRole::ProjectionBorrowMut;
          if ((projection &&
               (instruction.arg1 >= field_count ||
                instruction.arg1 != semantic_contract.projector_field)) ||
              (!projection && instruction.arg1 != field_count)) {
            error = "carrier view has an invalid canonical region contract";
            return false;
          }
          const auto semantic_origin =
              sem_ir_->inst(origin(instructions[position]));
          if (semantic_origin.kind != SemInstKind::CarrierView ||
              static_cast<std::uint32_t>(sem_ir_->integer(
                  IntegerId(semantic_origin.arg1))) != instruction.arg1) {
            error = "carrier view changed contract during lowering";
            return false;
          }
        }
        if (instruction.kind == LowInstKind::TransferReturn &&
            semantic_role == SemCanonicalFunctionRole::Pack) {
          error = "representation pack uses an in-place return transfer";
          return false;
        }
      }
      const auto terminator = inst(block(block_id).back());
      if (terminator.kind == LowInstKind::Branch &&
          !function_blocks.contains(terminator.arg0)) {
        error = "branch leaves its owning function";
        return false;
      }
      if (terminator.kind == LowInstKind::BranchIf) {
        const auto &pair = targets(TargetPairId(terminator.arg1));
        if (!function_blocks.contains(pair.true_block.index) ||
            !function_blocks.contains(pair.false_block.index)) {
          error = "conditional branch leaves its owning function";
          return false;
        }
      }
      if (terminator.kind == LowInstKind::Return &&
          TypeId(inst(LowInstId(terminator.arg0)).type) != expected_return) {
        error = "return value does not match its function";
        return false;
      }
      if (terminator.kind == LowInstKind::ReturnInPlace &&
          (semantic_role == SemCanonicalFunctionRole::Pack ||
           typeRepresentation(expected_return).facts.init_repr !=
               InitReprKind::InPlace)) {
        error = "in-place return does not match its function";
        return false;
      }
    }

    struct CarrierProvenance {
      LowInstId origin;
      std::vector<std::uint32_t> path;
      bool operator==(const CarrierProvenance &) const = default;
    };
    using CarrierSet = std::vector<CarrierProvenance>;
    const auto append_carrier = [](CarrierSet &target,
                                   const CarrierSet &source) {
      for (const auto &provenance : source)
        if (std::ranges::find(target, provenance) == target.end())
          target.push_back(provenance);
    };
    std::vector<LowInstId> function_instructions;
    for (const auto block_id : blockList(value.blocks)) {
      const auto instructions = block(block_id);
      function_instructions.insert(function_instructions.end(),
                                   instructions.begin(), instructions.end());
    }
    std::unordered_map<std::uint32_t, CarrierSet> carrier_values;
    std::unordered_map<std::uint32_t, CarrierSet> carrier_slots;
    bool carrier_changed = true;
    for (std::size_t iteration = 0;
         carrier_changed && iteration <= function_instructions.size() + 1;
         ++iteration) {
      carrier_changed = false;
      for (const auto instruction_id : function_instructions) {
        const auto &instruction = inst(instruction_id);
        CarrierSet next;
        if (instruction.kind == LowInstKind::CarrierView) {
          next.push_back({instruction_id, {}});
        } else if (instruction.kind == LowInstKind::Load ||
                   instruction.kind == LowInstKind::Borrow) {
          append_carrier(next, carrier_slots[instruction.arg0]);
        } else if (instruction.kind == LowInstKind::Dereference ||
                   instruction.kind == LowInstKind::CopyValue ||
                   instruction.kind == LowInstKind::MoveOut) {
          append_carrier(next, carrier_values[instruction.arg0]);
        } else if (instruction.kind == LowInstKind::StructField) {
          next = carrier_values[instruction.arg0];
          const auto field = static_cast<std::uint32_t>(
              sem_ir_->integer(IntegerId(instruction.arg1)));
          for (auto &provenance : next)
            provenance.path.push_back(field);
        } else if (instruction.kind == LowInstKind::ArrayIndex) {
          append_carrier(next, carrier_values[instruction.arg0]);
        }
        const auto before = carrier_values[instruction_id.index].size();
        append_carrier(carrier_values[instruction_id.index], next);
        carrier_changed |=
            before != carrier_values[instruction_id.index].size();
        if (instruction.kind == LowInstKind::Initialize ||
            instruction.kind == LowInstKind::Transfer) {
          const auto slot_before = carrier_slots[instruction.arg0].size();
          append_carrier(carrier_slots[instruction.arg0],
                         carrier_values[instruction.arg1]);
          carrier_changed |=
              slot_before != carrier_slots[instruction.arg0].size();
        }
      }
    }
    const auto within_region = [&](const CarrierProvenance &provenance) {
      const auto &carrier = inst(provenance.origin);
      const auto input_type = TypeId(inst(LowInstId(carrier.arg0)).type);
      const auto owner_type = sem_ir_->referencePointee(input_type);
      const auto &representation = typeRepresentation(owner_type);
      if (carrier.arg1 >= representation.field_projections.size())
        return true;
      const auto &region =
          representation.field_projections[carrier.arg1].region_indices;
      return region.empty() || (region.size() <= provenance.path.size() &&
                                std::equal(region.begin(), region.end(),
                                           provenance.path.begin()));
    };
    const auto check_region = [&](const CarrierSet &provenances) {
      return std::ranges::all_of(provenances, within_region);
    };
    for (const auto instruction_id : function_instructions) {
      const auto &instruction = inst(instruction_id);
      if (instruction.kind == LowInstKind::Call ||
          instruction.kind == LowInstKind::ForeignCall ||
          instruction.kind == LowInstKind::ForeignResultCall ||
          instruction.kind == LowInstKind::MakeArray ||
          instruction.kind == LowInstKind::MakeAggregate) {
        const auto operands = valueBlock(LowValueBlockId(
            instruction.kind == LowInstKind::Call ||
                    instruction.kind == LowInstKind::ForeignCall ||
                    instruction.kind == LowInstKind::ForeignResultCall
                ? instruction.arg1
                : instruction.arg0));
        if (std::ranges::any_of(operands, [&](LowInstId operand) {
              return !carrier_values[operand.index].empty();
            })) {
          error = "low carrier view escapes through a call or aggregate";
          return false;
        }
      } else if (instruction.kind == LowInstKind::Return) {
        const auto &provenances = carrier_values[instruction.arg0];
        if (provenances.empty())
          continue;
        const auto returned_type =
            TypeId(inst(LowInstId(instruction.arg0)).type);
        const auto borrow_role =
            semantic_role == SemCanonicalFunctionRole::ProjectionBorrow ||
            semantic_role == SemCanonicalFunctionRole::ProjectionBorrowMut;
        if ((sem_ir_->type(returned_type).kind == SemTypeKind::Reference &&
             !borrow_role) ||
            std::ranges::any_of(provenances, [](const auto &provenance) {
              return provenance.path.empty();
            })) {
          error = "low carrier view escapes through a return";
          return false;
        }
        if (!check_region(provenances)) {
          error = "low carrier return exceeds its projector region";
          return false;
        }
      } else if (instruction.kind == LowInstKind::ArrayIndex ||
                 instruction.kind == LowInstKind::Add ||
                 instruction.kind == LowInstKind::Equal) {
        if (!check_region(carrier_values[instruction.arg0]) ||
            !check_region(carrier_values[instruction.arg1])) {
          error = "low carrier read exceeds its projector region";
          return false;
        }
      }
    }
  }
  return true;
}


} // namespace chtholly::compiler
