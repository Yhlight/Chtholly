#include "LowIRVerificationContext.h"

#include <ranges>
#include <unordered_set>

namespace chtholly::compiler::internal {

namespace {
bool isTerminator(LowInstKind kind) {
  return kind == LowInstKind::Branch || kind == LowInstKind::BranchIf ||
         kind == LowInstKind::Return || kind == LowInstKind::ReturnInPlace ||
         kind == LowInstKind::Unreachable || kind == LowInstKind::FatalFailure ||
         kind == LowInstKind::CoroutineRuntimeFault ||
         kind == LowInstKind::CoroutineReturnSuccess ||
         kind == LowInstKind::CoroutineReturnError ||
         kind == LowInstKind::CoroutineReturnCancelled ||
         kind == LowInstKind::CoroutineCleanupEnd;
}
} // namespace

bool LowIRVerificationContext::verifyPreconditions(std::string &error) const {
  if (!low_ir_.nominal_layout_error_.empty()) {
    error = low_ir_.nominal_layout_error_;
    return false;
  }
  if (!low_ir_.sem_ir_->verify(error)) {
    error = "LowIR references invalid SemIR: " + error;
    return false;
  }
  if (!low_ir_.coroutine_lowering_error_.empty()) {
    error = low_ir_.coroutine_lowering_error_;
    return false;
  }
  return true;
}

bool LowIRVerificationContext::verifyStructure(std::string &error) const {
  if (low_ir_.origins_.size() != low_ir_.insts_.size()) {
    error = "low instruction and origin stores are not aligned";
    return false;
  }
  for (std::size_t index = 0; index < low_ir_.slots_.size(); ++index) {
    const auto &value = low_ir_.slot(SlotId(static_cast<std::uint32_t>(index)));
    if (value.type.index >= low_ir_.sem_ir_->typeCount() ||
        (((value.flags & LowSlotSynthetic) == 0) &&
         value.semantic_local.index >= low_ir_.sem_ir_->localCount()) ||
        ((value.flags & LowSlotSynthetic) != 0 && value.semantic_local.hasValue()) ||
        value.reserved != 0) {
      error = "low slot has an invalid semantic record";
      return false;
    }
  }
  for (std::size_t index = 0; index < low_ir_.places_.size(); ++index) {
    const auto &value = low_ir_.place(LowPlaceId(static_cast<std::uint32_t>(index)));
    if (value.root.index >= low_ir_.slots_.size() ||
        value.projections.index >= low_ir_.place_projection_blocks_.size() ||
        value.logical_projections.index >=
            low_ir_.place_projection_blocks_.size() ||
        value.type.index >= low_ir_.sem_ir_->typeCount() ||
        (value.flags & ~LowPlaceAddressable) != 0) {
      error = "low place has an invalid semantic record";
      return false;
    }
    for (const auto projection : low_ir_.placeProjections(value.projections))
      if (projection.aggregate_type.index >= low_ir_.sem_ir_->typeCount()) {
        error = "low place has an invalid frozen object projection";
        return false;
      }
    for (const auto projection : low_ir_.logicalPlaceProjections(
             LowPlaceId(static_cast<std::uint32_t>(index))))
      if (projection.aggregate_type.index >= low_ir_.sem_ir_->typeCount()) {
        error = "low place has an invalid logical projection";
        return false;
      }
  }
  for (std::size_t index = 0; index < low_ir_.targets_.size(); ++index) {
    const auto &value =
        low_ir_.targets(TargetPairId(static_cast<std::uint32_t>(index)));
    if (value.true_block.index >= low_ir_.blocks_.size() ||
        value.false_block.index >= low_ir_.blocks_.size()) {
      error = "conditional branch has an invalid target";
      return false;
    }
  }
  return true;
}

bool LowIRVerificationContext::verifyCleanupGraphs(
    std::vector<FunctionId> &instruction_owners,
    std::vector<CoroutineCleanupGraphId> &instruction_cleanup_graph,
    std::string &error) const {
  instruction_owners.assign(low_ir_.insts_.size(), FunctionId::invalid());
  instruction_cleanup_graph.assign(low_ir_.insts_.size(),
                                   CoroutineCleanupGraphId::invalid());
  std::unordered_set<std::uint32_t> ordinary_blocks;
  for (const auto &function : low_ir_.functions_.values())
    for (const auto block_id : low_ir_.blockList(function.blocks)) {
      ordinary_blocks.insert(block_id.index);
      for (const auto instruction : low_ir_.block(block_id)) {
        if (low_ir_.inst(instruction).kind == LowInstKind::CoroutineCleanupEnd) {
          error = "LowIR coroutine cleanup end appears in an ordinary block";
          return false;
        }
        instruction_owners[instruction.index] = function.semantic_function;
      }
    }
  std::unordered_set<std::uint32_t> cleanup_blocks;
  for (std::uint32_t graph_index = 0;
       graph_index < low_ir_.coroutine_cleanup_graphs_.size(); ++graph_index) {
    const auto graph_id = CoroutineCleanupGraphId(graph_index);
    const auto &graph = low_ir_.coroutineCleanupGraph(graph_id);
    if (!graph.function.hasValue() ||
        graph.function.index >= low_ir_.sem_ir_->functionCount() ||
        !graph.semantic_origin.hasValue() ||
        graph.semantic_origin.index >= low_ir_.sem_ir_->instCount() ||
        !graph.entry.hasValue() || graph.entry.index >= low_ir_.blockCount() ||
        !graph.blocks.hasValue() ||
        graph.blocks.index >= low_ir_.block_lists_.size()) {
      error = "LowIR coroutine cleanup graph has an invalid record";
      return false;
    }
    const auto graph_blocks = low_ir_.blockList(graph.blocks);
    if (graph_blocks.empty() ||
        std::ranges::find(graph_blocks, graph.entry) == graph_blocks.end()) {
      error = "LowIR coroutine cleanup graph omits its entry";
      return false;
    }
    bool has_end = false;
    std::unordered_set<std::uint32_t> graph_locals;
    for (const auto slot_id : graph.local_slots)
      if (!slot_id.hasValue() || slot_id.index >= low_ir_.slotCount() ||
          !graph_locals.insert(slot_id.index).second) {
        error = "LowIR coroutine cleanup graph has invalid local slots";
        return false;
      }
    for (const auto block_id : graph_blocks) {
      if (!block_id.hasValue() || block_id.index >= low_ir_.blockCount() ||
          ordinary_blocks.contains(block_id.index) ||
          !cleanup_blocks.insert(block_id.index).second) {
        error = "LowIR coroutine cleanup graph has invalid block ownership";
        return false;
      }
      if (low_ir_.block(block_id).empty()) {
        error = "LowIR coroutine cleanup graph contains an empty block";
        return false;
      }
      for (const auto instruction : low_ir_.block(block_id)) {
        if (!instruction.hasValue() || instruction.index >= low_ir_.instCount() ||
            instruction_cleanup_graph[instruction.index].hasValue()) {
          error = "LowIR coroutine cleanup graph has invalid instruction ownership";
          return false;
        }
        instruction_cleanup_graph[instruction.index] = graph_id;
        instruction_owners[instruction.index] = graph.function;
      }
      const auto &terminal = low_ir_.inst(low_ir_.block(block_id).back());
      has_end |= terminal.kind == LowInstKind::CoroutineCleanupEnd;
      if (terminal.kind == LowInstKind::Branch) {
        if (!low_ir_.containsArg(LowArgKind::Block, terminal.arg0) ||
            std::ranges::find(graph_blocks, LowBlockId(terminal.arg0)) ==
                graph_blocks.end()) {
          error = "LowIR coroutine cleanup branch leaves its graph";
          return false;
        }
      } else if (terminal.kind == LowInstKind::BranchIf) {
        if (!low_ir_.containsArg(LowArgKind::Targets, terminal.arg1)) {
          error = "LowIR coroutine cleanup branch has invalid targets";
          return false;
        }
        const auto &pair = low_ir_.targets(TargetPairId(terminal.arg1));
        if (std::ranges::find(graph_blocks, pair.true_block) ==
                graph_blocks.end() ||
            std::ranges::find(graph_blocks, pair.false_block) ==
                graph_blocks.end()) {
          error = "LowIR coroutine cleanup branch leaves its graph";
          return false;
        }
      } else if (terminal.kind != LowInstKind::CoroutineCleanupEnd) {
        error = "LowIR coroutine cleanup graph has an invalid terminator";
        return false;
      }
    }
    if (!has_end) {
      error = "LowIR coroutine cleanup graph has no cleanup end";
      return false;
    }
  }
  return true;
}

bool LowIRVerificationContext::verifyBlocks(std::string &error) const {
  for (std::size_t index = 0; index < low_ir_.blocks_.size(); ++index) {
    const auto instructions =
        low_ir_.block(LowBlockId(static_cast<std::uint32_t>(index)));
    if (instructions.empty()) {
      error = "low block is empty";
      return false;
    }
    for (std::size_t position = 0; position < instructions.size(); ++position) {
      if (instructions[position].index >= low_ir_.insts_.size()) {
        error = "low block contains an invalid instruction";
        return false;
      }
      if (isTerminator(low_ir_.inst(instructions[position]).kind) !=
          (position + 1 == instructions.size())) {
        error = "low block does not have exactly one final terminator";
        return false;
      }
      const auto drain_kind = low_ir_.inst(instructions[position]).kind;
      if (drain_kind != LowInstKind::CoroutineTaskGroupDrain &&
          drain_kind != LowInstKind::CoroutineTaskGroupErrorDrain &&
          drain_kind != LowInstKind::CoroutineTaskGroupCancelDrain)
        continue;
      const auto cancel_drain =
          drain_kind != LowInstKind::CoroutineTaskGroupDrain;
      const auto prefix_size = cancel_drain ? 5U : 4U;
      if (position < prefix_size) {
        error = "low coroutine task-group drain has no protocol prefix";
        return false;
      }
      const auto operands = low_ir_.valueBlock(
          drain_kind == LowInstKind::CoroutineTaskGroupDrain
              ? low_ir_.getAs<LowCoroutineTaskGroupDrain>(instructions[position])
                    .arg0
          : drain_kind == LowInstKind::CoroutineTaskGroupErrorDrain
              ? low_ir_.getAs<LowCoroutineTaskGroupErrorDrain>(
                            instructions[position])
                    .arg0
              : low_ir_.getAs<LowCoroutineTaskGroupCancelDrain>(
                            instructions[position])
                    .arg0);
      const auto load_id = instructions[position - 1];
      const auto initialize_id = instructions[position - 2];
      const auto arm_id = instructions[position - 3];
      const auto close_id = instructions[position - 4];
      const auto &load = low_ir_.inst(load_id);
      const auto &initialize = low_ir_.inst(initialize_id);
      const auto &arm = low_ir_.inst(arm_id);
      const auto &close = low_ir_.inst(close_id);
      if (operands.size() != 2 || operands[0] != load_id ||
          load.kind != LowInstKind::Load ||
          initialize.kind != LowInstKind::Initialize ||
          initialize.arg0 != load.arg0 || initialize.arg1 != arm_id.index ||
          arm.kind != LowInstKind::CoroutineTaskGroupCompletionArm ||
          arm.arg0 != operands[1].index ||
          close.kind != LowInstKind::CoroutineTaskGroupClose ||
          close.arg0 != operands[1].index) {
        error = "low coroutine task-group drain has an invalid protocol prefix";
        return false;
      }
      if (cancel_drain) {
        const auto &request_cancel = low_ir_.inst(instructions[position - 5]);
        if (request_cancel.kind !=
                LowInstKind::CoroutineTaskGroupRequestCancel ||
            request_cancel.arg0 != operands[1].index) {
          error = "low coroutine task-group cancel drain has no cancellation request";
          return false;
        }
      }
    }
  }
  return true;
}

bool LowIRVerificationContext::verifyAbiIndexes(std::string &error) const {
  if (!low_ir_.foreign_abi_error_.empty()) {
    error = low_ir_.foreign_abi_error_;
    return false;
  }
  if (low_ir_.foreign_abi_layout_by_target_.size() !=
      low_ir_.sem_ir_->functionRefCount()) {
    error = "LowIR has an incomplete foreign ABI layout index";
    return false;
  }
  if (low_ir_.foreign_abi_layout_by_callback_type_.size() !=
      low_ir_.sem_ir_->typeCount()) {
    error = "LowIR has an incomplete callback ABI layout index";
    return false;
  }
  return true;
}
} // namespace chtholly::compiler::internal
