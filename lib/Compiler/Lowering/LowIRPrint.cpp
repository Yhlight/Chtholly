#include "chtholly/Compiler/LowIR.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cctype>
#include <deque>
#include <functional>
#include <limits>
#include <sstream>
#include <string_view>
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

bool isTerminator(LowInstKind kind) {
  return kind == LowInstKind::Branch || kind == LowInstKind::BranchIf ||
         kind == LowInstKind::Return || kind == LowInstKind::ReturnInPlace ||
         kind == LowInstKind::Unreachable ||
         kind == LowInstKind::FatalFailure ||
         kind == LowInstKind::CoroutineRuntimeFault ||
         kind == LowInstKind::CoroutineReturnSuccess ||
         kind == LowInstKind::CoroutineReturnError ||
         kind == LowInstKind::CoroutineReturnCancelled ||
         kind == LowInstKind::CoroutineCleanupEnd;
}

ForeignAbiTargetKind classifyForeignTarget(std::string_view triple) {
  std::string value(triple);
  std::ranges::transform(value, value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  const auto windows = value.find("windows") != std::string::npos;
  if (value.starts_with("x86_64") || value.starts_with("amd64"))
    return windows ? ForeignAbiTargetKind::WindowsX64
                   : ForeignAbiTargetKind::SysVAMD64;
  if (value.starts_with("aarch64") && !windows &&
      (value.find("linux") != std::string::npos ||
       value.find("gnu") != std::string::npos))
    return ForeignAbiTargetKind::AAPCS64;
  return ForeignAbiTargetKind::Unsupported;
}

const ForeignAbiSignature *foreignSignature(const SemIR &sem_ir,
                                            FunctionRefId target) {
  const auto &reference = sem_ir.functionRef(target);
  if (reference.local_function.hasValue()) {
    const auto &declaration =
        sem_ir.functionDeclaration(reference.local_function);
    return declaration.foreign_signature ? &*declaration.foreign_signature
                                         : nullptr;
  }
  const auto *entity = sem_ir.importIRs().tryGetEntity(reference.public_entity);
  return entity && entity->foreign_signature ? &*entity->foreign_signature
                                             : nullptr;
}

const interop::ForeignOperationArtifact *
foreignOperation(const SemIR &sem_ir, FunctionRefId target) {
  const auto &reference = sem_ir.functionRef(target);
  if (reference.local_function.hasValue()) {
    const auto &declaration =
        sem_ir.functionDeclaration(reference.local_function);
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

bool isForeign(const SemIR &sem_ir, FunctionRefId target) {
  const auto &reference = sem_ir.functionRef(target);
  if (reference.local_function.hasValue())
    return sem_ir.functionDeclaration(reference.local_function).kind ==
           SemCallableDeclarationKind::Foreign;
  const auto *entity = sem_ir.importIRs().tryGetEntity(reference.public_entity);
  return entity &&
         entity->declaration_kind == PublicCallableDeclarationKind::Foreign;
}

const CanonicalForeignResourceProtocol *verifiedForeignResourceProtocol(
    const SemIR &sem_ir, TypeId type, bool completion_projection,
    ForeignResourceProtocolId &protocol_id, std::string &error) {
  protocol_id = sem_ir.foreignResourceProtocolId(type);
  const auto &protocol = sem_ir.foreignResourceProtocol(type);
  const auto fields = sem_ir.typeBlock(TypeBlockId(sem_ir.type(type).arg0));
  if (protocol.facts.completion_projection != completion_projection ||
      protocol.types.size() != fields.size()) {
    error = "foreign resource protocol disagrees with its semantic type";
    return nullptr;
  }
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (protocol.types[index] != sem_ir.canonicalType(fields[index])) {
      error = "foreign resource protocol has stale canonical type bindings";
      return nullptr;
    }
  }
  if (!protocol.facts.verify(static_cast<std::uint32_t>(protocol.types.size()),
                             error))
    return nullptr;
  return &protocol;
}
} // namespace

std::string LowIR::print() const {
  std::ostringstream out;
  for (std::size_t index = 0; index < outcome_descriptors_.size(); ++index) {
    const auto &descriptor = outcome_descriptors_.get(
        OutcomeDescriptorId(static_cast<std::uint32_t>(index)));
    out << "outcome" << index << " cardinality="
        << outcomeCardinalityName(descriptor.cardinality)
        << " arms=" << descriptor.arms.size()
        << " transitions=" << descriptor.transitions.size()
        << " token_lanes=";
    std::vector<std::uint32_t> token_lanes;
    for (const auto &transition : descriptor.transitions)
      if (transition.consumes_token &&
          std::ranges::find(token_lanes, transition.token_lane) ==
              token_lanes.end())
        token_lanes.push_back(transition.token_lane);
    std::ranges::sort(token_lanes);
    out << token_lanes.size()
        << " fingerprint=" << descriptor.fingerprint().hex() << '\n';
  }
  for (std::size_t index = 0; index < coroutine_frame_plans_.size(); ++index) {
    const auto &plan = coroutineFramePlan(
        CoroutineFramePlanId(static_cast<std::uint32_t>(index)));
    out << "coroutine_frame" << index << " sem_fn=" << plan.function.index
        << " constructor_entity=" << plan.constructor_entity.index
        << " constructor_abi=" << plan.constructor_abi_epoch
        << " abi=" << plan.abi_version << " error="
        << (plan.error_type ? std::to_string(plan.error_type->index) : "none")
        << " states=" << plan.resume_states.size()
        << " lifted=" << plan.lifted_slots.size() << '\n';
  }
  for (std::size_t index = 0; index < coroutine_task_create_plans_.size();
       ++index) {
    const auto &plan = coroutineTaskCreatePlan(
        CoroutineTaskCreatePlanId(static_cast<std::uint32_t>(index)));
    out << "coroutine_task_create" << index << " target=" << plan.target.index
        << " scaffold=" << plan.scaffold.index
        << " constructor_entity=" << plan.constructor_entity.index
        << " constructor_abi=" << plan.constructor_abi_epoch
        << " task_type=" << plan.task_type.index << " mode="
        << (plan.mode == CoroutineTaskCreateMode::Root ? "root" : "child")
        << " parameters=" << plan.parameter_types.size() << '\n';
  }
  for (std::size_t index = 0;
       index < coroutine_task_completion_set_plans_.size(); ++index) {
    const auto &plan = coroutineTaskCompletionSetPlan(
        CoroutineTaskCompletionSetPlanId(static_cast<std::uint32_t>(index)));
    out << "coroutine_completion_set" << index
        << " scaffold=" << plan.scaffold.index
        << " operands=" << plan.operand_count
        << " bitmap_words=" << plan.bitmap_word_count
        << " abi=" << plan.abi_epoch << '\n';
  }
  for (std::size_t index = 0;
       index < coroutine_task_completion_combine_plans_.size(); ++index) {
    const auto &plan =
        coroutineTaskCompletionCombinePlan(CoroutineTaskCompletionCombinePlanId(
            static_cast<std::uint32_t>(index)));
    out << "coroutine_completion_combine" << index
        << " scaffold=" << plan.scaffold.index
        << " operation=" << static_cast<unsigned>(plan.operation)
        << " operands=" << plan.operand_count
        << " bitmap_words=" << plan.bitmap_word_count << " continuation="
        << (plan.continuation.hasValue()
                ? std::to_string(plan.continuation.index)
                : "none")
        << " abi=" << plan.abi_epoch << '\n';
  }
  for (std::size_t index = 0; index < functions_.size(); ++index) {
    const auto &fn = function(LowFunctionId(static_cast<std::uint32_t>(index)));
    out << "fn" << index << " sem_fn=" << fn.semantic_function.index
        << " entry=block" << fn.entry.index << '\n';
    for (const auto block_id : blockList(fn.blocks)) {
      out << "  block" << block_id.index << ":\n";
      for (const auto inst_id : block(block_id)) {
        const auto &value = inst(inst_id);
        out << "    low" << inst_id.index << " = "
            << lowInstKindName(value.kind) << " type=" << value.type
            << " arg0=" << value.arg0 << " arg1=" << value.arg1 << '\n';
      }
    }
  }
  return out.str();
}

void LowIR::collectMetrics(core::CompilerMetrics &metrics,
                           std::string_view label) const {
  insts_.collectMetrics(metrics,
                        core::CompilerMetrics::childLabel(label, "insts"));
  origins_.collectMetrics(metrics,
                          core::CompilerMetrics::childLabel(label, "origins"));
  slots_.collectMetrics(metrics,
                        core::CompilerMetrics::childLabel(label, "slots"));
  places_.collectMetrics(metrics,
                         core::CompilerMetrics::childLabel(label, "places"));
  targets_.collectMetrics(metrics,
                          core::CompilerMetrics::childLabel(label, "targets"));
  blocks_.collectMetrics(metrics,
                         core::CompilerMetrics::childLabel(label, "blocks"));
  functions_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "functions"));
  coroutine_frame_plans_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "coroutine_frames"));
  coroutine_task_create_plans_.collectMetrics(
      metrics,
      core::CompilerMetrics::childLabel(label, "coroutine_task_creates"));
  coroutine_task_completion_arm_plans_.collectMetrics(
      metrics,
      core::CompilerMetrics::childLabel(label, "coroutine_completion_arms"));
  coroutine_task_completion_set_plans_.collectMetrics(
      metrics,
      core::CompilerMetrics::childLabel(label, "coroutine_completion_sets"));
  coroutine_task_completion_combine_plans_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(
                   label, "coroutine_completion_combinations"));
  outcome_descriptors_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "outcomes"));
  inst_blocks_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "inst_blocks"));
  block_lists_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "block_lists"));
  slot_blocks_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "slot_blocks"));
  value_blocks_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "value_blocks"));
}

std::string_view lowInstKindName(LowInstKind kind) {
  const auto index = static_cast<std::size_t>(kind);
  return index < Names.size() ? Names[index] : "InvalidLowInstKind";
}
LowArgKind lowInstArgKind(LowInstKind kind, std::size_t index) {
  const auto kind_index = static_cast<std::size_t>(kind);
  return kind_index < Args.size() && index < 2 ? Args[kind_index][index]
                                               : LowArgKind::None;
}

} // namespace chtholly::compiler
