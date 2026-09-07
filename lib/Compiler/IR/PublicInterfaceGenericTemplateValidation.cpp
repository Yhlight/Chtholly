#include "chtholly/Compiler/PublicInterface.h"

#include "PublicInterfaceServices.h"
#include "chtholly/Compiler/BuiltinOperator.h"
#include "chtholly/Compiler/SemIR.h"

#include <algorithm>
#include <ranges>
#include <unordered_set>
#include <vector>

namespace chtholly::compiler {

namespace {
constexpr std::uint32_t UnionFieldIndexMask = ~(1U << 31U);
} // namespace

bool GenericTemplateArtifact::verify(std::string &error) const {
  error.clear();
  if (parameter_count > local_types.size() ||
      local_flags.size() != local_types.size() ||
      declaration.results.size() != parameter_count + 1 ||
      definition.entry_block >= definition.blocks.size() ||
      definition.instruction_value_blocks.size() !=
          definition.instructions.size()) {
    error = "generic template artifact has an invalid header";
    return false;
  }
  if (std::ranges::any_of(local_flags, [](std::uint32_t flags) {
        return (flags & ~0x7U) != 0;
      })) {
    error = "generic template artifact has invalid local flags";
    return false;
  }
  for (const auto type : local_types) {
    if (!internal::GenericTemplateValidationService::validType(type, generic_parameter_count, true) ||
        !internal::GenericTemplateValidationService::validReferenceType(type, parameter_count, true)) {
      error = "generic template artifact has an invalid local type";
      return false;
    }
  }
  for (const auto &type : declaration.results) {
    if (!internal::GenericTemplateValidationService::validType(type, generic_parameter_count, true) ||
        !internal::GenericTemplateValidationService::validReferenceType(type, parameter_count, true)) {
      error = "generic template declaration region has an invalid result `" +
              publicTypeName(type) + "`";
      return false;
    }
  }
  if (!declaration.instructions.empty() || !declaration.blocks.empty() ||
      !declaration.instruction_value_blocks.empty() ||
      declaration.entry_block != core::AnyId::InvalidIndex) {
    error = "generic template declaration region is not canonical";
    return false;
  }
  if (callee_type_arguments.size() != callees.size()) {
    error = "generic template artifact has inconsistent callee arguments";
    return false;
  }
  for (std::size_t index = 0; index < callees.size(); ++index) {
    const auto &callee = callees[index];
    if (callee.canonical_package.empty() || callee.canonical_module.empty() ||
        callee.canonical_name.empty() ||
        std::ranges::any_of(
            callee_type_arguments[index], [&](const auto &type) {
              return !internal::GenericTemplateValidationService::validType(type, generic_parameter_count, true);
            })) {
      error = "generic template artifact has an invalid canonical callee";
      return false;
    }
  }
  const auto dependent_type = [&](const auto &self,
                                  const PublicType &type) -> bool {
    if (type.kind == PublicTypeKind::TypeParameter ||
        type.kind == PublicTypeKind::TypeProjection)
      return true;
    return std::ranges::any_of(type.arguments, [&](const auto &argument) {
      return self(self, argument);
    });
  };
  for (const auto &query : type_queries) {
    if (query.kind >= GenericTypeQueryKind::Count ||
        !internal::GenericTemplateValidationService::validType(query.source, generic_parameter_count, true) ||
        (query.kind != GenericTypeQueryKind::TypeSame &&
         !dependent_type(dependent_type, query.source))) {
      error = "generic template artifact has an invalid type query";
      return false;
    }
    if (query.kind == GenericTypeQueryKind::TypeSame &&
        (!internal::GenericTemplateValidationService::validType(query.other, generic_parameter_count, true) ||
         !dependent_type(dependent_type, query.source) &&
             !dependent_type(dependent_type, query.other))) {
      error = "generic template artifact has an invalid type equality query";
      return false;
    }
    if (query.kind == GenericTypeQueryKind::TypeIs &&
        query.property != "integer" && query.property != "floating" &&
        query.property != "raw_pointer" && query.property != "array" &&
        query.property != "nominal" && query.property != "string" &&
        query.property != "reference") {
      error = "generic template artifact has an unknown type category query";
      return false;
    }
    if (query.kind == GenericTypeQueryKind::TypeHas &&
        query.property != "copy" && query.property != "move" &&
        query.property != "drop" && query.property != "object_representation") {
      error = "generic template artifact has an unknown type capability query";
      return false;
    }
  }
  const auto &region = definition;
  const auto valid_inst = [&](std::uint32_t id) {
    return id < region.instructions.size();
  };
  const auto valid_local = [&](std::uint32_t id) {
    return id < local_types.size();
  };
  const auto valid_block = [&](std::uint32_t id) {
    return id < region.blocks.size();
  };
  for (std::uint32_t index = 0; index < region.instructions.size(); ++index) {
    const auto &inst = region.instructions[index];
    const auto &instruction_values = region.instruction_value_blocks[index];
    if (inst.opcode >= GenericTemplateOpcode::Count ||
        !internal::GenericTemplateValidationService::validType(inst.type, generic_parameter_count, true) ||
        !internal::GenericTemplateValidationService::validReferenceType(inst.type, parameter_count, true)) {
      error = "generic template artifact has an invalid instruction";
      return false;
    }
    const auto previous_inst = [&](std::uint32_t id) {
      return valid_inst(id) && id < index;
    };
    bool valid = false;
    switch (inst.opcode) {
    case GenericTemplateOpcode::Parameter:
    case GenericTemplateOpcode::NameRef:
    case GenericTemplateOpcode::BorrowLocal:
      valid = valid_local(inst.arg0) && inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::BorrowPlace:
      valid =
          previous_inst(inst.arg0) && inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::CarrierView:
      valid =
          previous_inst(inst.arg0) && inst.arg1 != core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::IntegerLiteral:
    case GenericTemplateOpcode::FloatLiteral:
      valid =
          inst.arg0 < integers.size() && inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::CharLiteral:
      valid = inst.arg0 < integers.size() &&
              inst.arg1 == core::AnyId::InvalidIndex &&
              inst.type.kind == PublicTypeKind::Char &&
              static_cast<std::uint64_t>(integers[inst.arg0]) <= 0x10ffffU;
      break;
    case GenericTemplateOpcode::StringLiteral: {
      const auto cstring =
          inst.type.kind == PublicTypeKind::RawPointer &&
          inst.type.pointer_const && inst.type.arguments.size() == 1 &&
          inst.type.arguments.front().kind == PublicTypeKind::Integer &&
          inst.type.arguments.front().scalar_width == 8 &&
          inst.type.arguments.front().integer_signed;
      valid = inst.arg0 < strings.size() &&
              inst.arg1 == core::AnyId::InvalidIndex &&
              (inst.type.kind == PublicTypeKind::String || cstring);
      break;
    }
    case GenericTemplateOpcode::ArrayLiteral:
      valid = inst.arg0 == core::AnyId::InvalidIndex &&
              inst.arg1 == core::AnyId::InvalidIndex &&
              inst.type.kind == PublicTypeKind::Array &&
              inst.type.arguments.size() == 1 &&
              instruction_values.size() == inst.type.array_bound;
      if (valid)
        for (const auto element : instruction_values)
          valid =
              valid && previous_inst(element) &&
              region.instructions[element].type == inst.type.arguments.front();
      break;
    case GenericTemplateOpcode::NullPointer:
      valid = inst.type.kind == PublicTypeKind::RawPointer &&
              inst.arg0 == core::AnyId::InvalidIndex &&
              inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::TypeQuery: {
      valid = inst.arg0 < type_queries.size() &&
              inst.arg1 == core::AnyId::InvalidIndex;
      if (valid) {
        const auto kind = type_queries[inst.arg0].kind;
        const auto value_query = kind == GenericTypeQueryKind::TypeSame ||
                                 kind == GenericTypeQueryKind::TypeIs ||
                                 kind == GenericTypeQueryKind::TypeHas;
        valid = value_query ? inst.type.kind == PublicTypeKind::Bool
                            : inst.type.kind == PublicTypeKind::Integer;
      }
      break;
    }
    case GenericTemplateOpcode::BoolLiteral:
      valid = inst.type.kind == PublicTypeKind::Bool && inst.arg0 <= 1 &&
              inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::VoidValue:
      valid = inst.type.kind == PublicTypeKind::Void &&
              inst.arg0 == core::AnyId::InvalidIndex &&
              inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::BuiltinUnary:
      valid =
          inst.arg0 < static_cast<std::uint32_t>(BuiltinOperatorKind::Count) &&
          isBuiltinUnaryOperator(static_cast<BuiltinOperatorKind>(inst.arg0)) &&
          inst.arg1 == core::AnyId::InvalidIndex &&
          instruction_values.size() == 1 &&
          previous_inst(instruction_values[0]);
      break;
    case GenericTemplateOpcode::BuiltinBinary:
      valid =
          inst.arg0 < static_cast<std::uint32_t>(BuiltinOperatorKind::Count) &&
          !isBuiltinUnaryOperator(
              static_cast<BuiltinOperatorKind>(inst.arg0)) &&
          inst.arg1 == core::AnyId::InvalidIndex &&
          instruction_values.size() == 2 &&
          previous_inst(instruction_values[0]) &&
          previous_inst(instruction_values[1]);
      break;
    case GenericTemplateOpcode::Add:
    case GenericTemplateOpcode::Equal:
      valid = previous_inst(inst.arg0) && previous_inst(inst.arg1);
      break;
    case GenericTemplateOpcode::NumericConvert:
    case GenericTemplateOpcode::CheckedNumericCast:
      valid =
          previous_inst(inst.arg0) && inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::BindName:
      valid = valid_local(inst.arg0) && previous_inst(inst.arg1);
      break;
    case GenericTemplateOpcode::MaterializeTemporary:
      valid = valid_local(inst.arg0) && previous_inst(inst.arg1);
      break;
    case GenericTemplateOpcode::ExtendTemporary:
      valid = valid_local(inst.arg0) && previous_inst(inst.arg1) &&
              region.instructions[inst.arg1].opcode ==
                  GenericTemplateOpcode::MaterializeTemporary;
      break;
    case GenericTemplateOpcode::Assert:
      valid =
          inst.type.kind == PublicTypeKind::Void && previous_inst(inst.arg0) &&
          region.instructions[inst.arg0].type.kind == PublicTypeKind::Bool &&
          inst.arg1 ==
              static_cast<std::uint32_t>(UnrecoverableFailureReason::Assertion);
      break;
    case GenericTemplateOpcode::UnrecoverableFailure:
      valid = inst.type.kind == PublicTypeKind::Never &&
              isValidUnrecoverableFailureReason(inst.arg0) &&
              inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::EndFullExpression: {
      valid = inst.arg0 == core::AnyId::InvalidIndex &&
              inst.arg1 == core::AnyId::InvalidIndex;
      std::unordered_set<std::uint32_t> boundary_temporaries;
      for (const auto temporary : instruction_values) {
        valid = valid && previous_inst(temporary) &&
                boundary_temporaries.insert(temporary).second &&
                region.instructions[temporary].opcode ==
                    GenericTemplateOpcode::MaterializeTemporary;
      }
      break;
    }
    case GenericTemplateOpcode::Assign:
      valid = previous_inst(inst.arg0) && previous_inst(inst.arg1);
      break;
    case GenericTemplateOpcode::Return:
    case GenericTemplateOpcode::Yield:
      valid =
          previous_inst(inst.arg0) && inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::Defer:
    case GenericTemplateOpcode::ScopedBlock:
      valid = valid_block(inst.arg0) && inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::If:
      valid = previous_inst(inst.arg0) && valid_block(inst.arg1);
      break;
    case GenericTemplateOpcode::IfArm:
      valid = valid_block(inst.arg0) && inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::While:
      valid = valid_block(inst.arg0) && valid_block(inst.arg1) &&
              !region.blocks[inst.arg0].empty();
      break;
    case GenericTemplateOpcode::For:
    case GenericTemplateOpcode::DoWhile:
      valid = valid_block(inst.arg0) && valid_block(inst.arg1);
      break;
    case GenericTemplateOpcode::ForClause:
      valid = inst.arg0 <= 2 && valid_block(inst.arg1);
      break;
    case GenericTemplateOpcode::Break:
    case GenericTemplateOpcode::Continue:
      valid = inst.arg0 <= region.instructions.size() &&
              inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::Switch:
      valid = previous_inst(inst.arg0) && valid_block(inst.arg1);
      break;
    case GenericTemplateOpcode::SwitchArm:
      valid = valid_block(inst.arg1);
      break;
    case GenericTemplateOpcode::Call:
    case GenericTemplateOpcode::CompilerIntrinsicCall:
      valid =
          inst.arg0 < callees.size() && inst.arg1 == core::AnyId::InvalidIndex;
      if (valid) {
        for (const auto argument : instruction_values)
          valid = valid && previous_inst(argument);
      }
      break;
    case GenericTemplateOpcode::InterfaceCall:
      valid =
          inst.arg0 < integers.size() && inst.arg1 == core::AnyId::InvalidIndex;
      if (valid)
        for (const auto argument : instruction_values)
          valid = valid && previous_inst(argument);
      break;
    case GenericTemplateOpcode::FunctionValue:
      valid =
          inst.arg0 < callees.size() && inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::BoundMethod:
      valid = inst.arg0 < callees.size() &&
              inst.arg1 < static_cast<std::uint32_t>(
                              SemCallableEnvironmentCapability::Count) &&
              inst.type.kind == PublicTypeKind::Nominal &&
              instruction_values.size() == 1 &&
              previous_inst(instruction_values.front());
      break;
    case GenericTemplateOpcode::Closure:
      valid = inst.arg0 < callees.size() &&
              inst.arg1 < static_cast<std::uint32_t>(
                              SemCallableEnvironmentCapability::Count) &&
              inst.type.kind == PublicTypeKind::Nominal;
      if (valid)
        for (const auto capture : instruction_values)
          valid = valid && previous_inst(capture);
      break;
    case GenericTemplateOpcode::IndirectCall:
      valid =
          previous_inst(inst.arg0) && inst.arg1 == core::AnyId::InvalidIndex;
      if (valid)
        for (const auto argument : instruction_values)
          valid = valid && previous_inst(argument);
      break;
    case GenericTemplateOpcode::AggregateInit:
    case GenericTemplateOpcode::Slice:
    case GenericTemplateOpcode::TupleLiteral:
      valid = inst.arg0 == core::AnyId::InvalidIndex &&
              inst.arg1 == core::AnyId::InvalidIndex;
      if (valid)
        for (const auto field : instruction_values)
          valid = valid && previous_inst(field);
      break;
    case GenericTemplateOpcode::UnionInit:
      valid = previous_inst(inst.arg0) && inst.arg1 <= UnionFieldIndexMask;
      break;
    case GenericTemplateOpcode::EnumInit:
      valid = inst.arg0 == core::AnyId::InvalidIndex;
      if (valid)
        for (const auto field : instruction_values)
          valid = valid && previous_inst(field);
      break;
    case GenericTemplateOpcode::EnumTag:
      valid =
          previous_inst(inst.arg0) && inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::EnumPayloadAccess:
      valid = previous_inst(inst.arg0) && inst.arg1 < integers.size();
      break;
    case GenericTemplateOpcode::StructFieldAccess:
      valid = previous_inst(inst.arg0);
      break;
    case GenericTemplateOpcode::MemberAccess:
      valid = previous_inst(inst.arg0) && inst.arg1 < strings.size();
      break;
    case GenericTemplateOpcode::StaticIndex: {
      valid = previous_inst(inst.arg0) && previous_inst(inst.arg1);
      if (valid) {
        const auto &base = region.instructions[inst.arg0].type;
        const auto &index_inst = region.instructions[inst.arg1];
        valid = base.kind == PublicTypeKind::Array &&
                base.arguments.size() == 1 &&
                index_inst.opcode == GenericTemplateOpcode::IntegerLiteral &&
                index_inst.arg0 < integers.size() &&
                integers[index_inst.arg0] >= 0 &&
                static_cast<std::uint64_t>(integers[index_inst.arg0]) <
                    base.array_bound &&
                inst.type == base.arguments.front();
      }
      break;
    }
    case GenericTemplateOpcode::Index: {
      valid = previous_inst(inst.arg0) && previous_inst(inst.arg1);
      if (valid) {
        const auto &base = region.instructions[inst.arg0].type;
        valid = (base.kind == PublicTypeKind::Array ||
                 base.kind == PublicTypeKind::Slice) &&
                base.arguments.size() == 1 &&
                inst.type == base.arguments.front();
      }
      break;
    }
    case GenericTemplateOpcode::UnionFieldAccess:
      valid =
          previous_inst(inst.arg0) && inst.arg1 != core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::Dereference:
    case GenericTemplateOpcode::Move:
    case GenericTemplateOpcode::Copy:
      valid =
          previous_inst(inst.arg0) && inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::Count:
      break;
    }
    if (inst.opcode != GenericTemplateOpcode::Call &&
        inst.opcode != GenericTemplateOpcode::CompilerIntrinsicCall &&
        inst.opcode != GenericTemplateOpcode::InterfaceCall &&
        inst.opcode != GenericTemplateOpcode::IndirectCall &&
        inst.opcode != GenericTemplateOpcode::EndFullExpression &&
        inst.opcode != GenericTemplateOpcode::AggregateInit &&
        inst.opcode != GenericTemplateOpcode::Slice &&
        inst.opcode != GenericTemplateOpcode::TupleLiteral &&
        inst.opcode != GenericTemplateOpcode::BoundMethod &&
        inst.opcode != GenericTemplateOpcode::Closure &&
        inst.opcode != GenericTemplateOpcode::ArrayLiteral &&
        inst.opcode != GenericTemplateOpcode::TypeQuery &&
        inst.opcode != GenericTemplateOpcode::EnumInit &&
        inst.opcode != GenericTemplateOpcode::BuiltinUnary &&
        inst.opcode != GenericTemplateOpcode::BuiltinBinary &&
        !instruction_values.empty())
      valid = false;
    if (!valid) {
      error = "generic template artifact has invalid instruction operands";
      return false;
    }
  }
  std::vector<bool> seen(region.instructions.size());
  std::vector<std::uint32_t> instruction_blocks(region.instructions.size(),
                                                core::AnyId::InvalidIndex);
  std::vector<std::uint32_t> instruction_positions(region.instructions.size(),
                                                   core::AnyId::InvalidIndex);
  std::vector<std::vector<std::uint32_t>> successors(region.blocks.size());
  std::vector<std::uint32_t> predecessor_counts(region.blocks.size());
  std::vector<std::uint32_t> parent_blocks(region.blocks.size(),
                                           core::AnyId::InvalidIndex);
  std::vector<std::uint32_t> parent_instructions(region.blocks.size(),
                                                 core::AnyId::InvalidIndex);
  bool duplicate_parent = false;
  const auto add_successor = [&](std::uint32_t block, std::uint32_t instruction,
                                 std::uint32_t successor) {
    successors[block].push_back(successor);
    ++predecessor_counts[successor];
    if (parent_blocks[successor] != core::AnyId::InvalidIndex)
      duplicate_parent = true;
    else {
      parent_blocks[successor] = block;
      parent_instructions[successor] = instruction;
    }
  };
  for (std::uint32_t block_index = 0; block_index < region.blocks.size();
       ++block_index) {
    const auto &block = region.blocks[block_index];
    for (std::uint32_t position = 0; position < block.size(); ++position) {
      const auto inst = block[position];
      if (!valid_inst(inst) || seen[inst]) {
        error = "generic template artifact has a non-canonical block";
        return false;
      }
      seen[inst] = true;
      instruction_blocks[inst] = block_index;
      instruction_positions[inst] = position;
      const auto &instruction = region.instructions[inst];
      if (instruction.opcode == GenericTemplateOpcode::If) {
        add_successor(block_index, inst, instruction.arg1);
      } else if (instruction.opcode == GenericTemplateOpcode::IfArm) {
        add_successor(block_index, inst, instruction.arg0);
      } else if (instruction.opcode == GenericTemplateOpcode::While) {
        add_successor(block_index, inst, instruction.arg0);
        add_successor(block_index, inst, instruction.arg1);
      } else if (instruction.opcode == GenericTemplateOpcode::For ||
                 instruction.opcode == GenericTemplateOpcode::DoWhile) {
        add_successor(block_index, inst, instruction.arg0);
        add_successor(block_index, inst, instruction.arg1);
      } else if (instruction.opcode == GenericTemplateOpcode::ForClause) {
        add_successor(block_index, inst, instruction.arg1);
      } else if (instruction.opcode == GenericTemplateOpcode::Defer ||
                 instruction.opcode == GenericTemplateOpcode::ScopedBlock) {
        add_successor(block_index, inst, instruction.arg0);
      } else if (instruction.opcode == GenericTemplateOpcode::Switch ||
                 instruction.opcode == GenericTemplateOpcode::SwitchArm) {
        add_successor(block_index, inst, instruction.arg1);
      }
    }
  }
  if (std::ranges::any_of(seen, [](bool value) { return !value; })) {
    error = "generic template artifact has an unreachable instruction";
    return false;
  }
  {
    auto remaining_predecessors = predecessor_counts;
    std::vector<std::uint32_t> roots;
    for (std::uint32_t block = 0; block < region.blocks.size(); ++block)
      if (remaining_predecessors[block] == 0)
        roots.push_back(block);
    std::size_t visited_blocks = 0;
    for (std::size_t index = 0; index < roots.size(); ++index) {
      ++visited_blocks;
      for (const auto successor : successors[roots[index]])
        if (--remaining_predecessors[successor] == 0)
          roots.push_back(successor);
    }
    if (visited_blocks != region.blocks.size()) {
      error = "generic template artifact has cyclic control flow";
      return false;
    }
  }
  bool invalid_ownership = predecessor_counts[region.entry_block] != 0;
  for (std::uint32_t block = 0; block < region.blocks.size(); ++block)
    invalid_ownership |=
        block != region.entry_block && predecessor_counts[block] != 1;
  if (duplicate_parent || invalid_ownership) {
    error = "generic template artifact has non-canonical region ownership";
    return false;
  }
  for (std::uint32_t instruction = 0; instruction < region.instructions.size();
       ++instruction) {
    const auto opcode = region.instructions[instruction].opcode;
    std::uint32_t nested_loops = 0;
    auto block = instruction_blocks[instruction];
    while (block != region.entry_block) {
      const auto parent_id = parent_instructions[block];
      const auto &parent = region.instructions[parent_id];
      if (parent.opcode == GenericTemplateOpcode::Defer) {
        if (opcode == GenericTemplateOpcode::Defer ||
            opcode == GenericTemplateOpcode::Return ||
            opcode == GenericTemplateOpcode::UnrecoverableFailure ||
            ((opcode == GenericTemplateOpcode::Break ||
              opcode == GenericTemplateOpcode::Continue) &&
             region.instructions[instruction].arg0 >= nested_loops)) {
          error = "generic template defer body has invalid control flow";
          return false;
        }
        break;
      }
      if ((parent.opcode == GenericTemplateOpcode::While ||
           parent.opcode == GenericTemplateOpcode::For ||
           parent.opcode == GenericTemplateOpcode::DoWhile) &&
          parent.arg1 == block)
        ++nested_loops;
      block = parent_blocks[block];
    }
  }
  const auto dominates = [&](std::uint32_t source, std::uint32_t consumer) {
    const auto source_block = instruction_blocks[source];
    auto consumer_block = instruction_blocks[consumer];
    if (source_block == consumer_block)
      return instruction_positions[source] < instruction_positions[consumer];
    while (consumer_block != region.entry_block) {
      const auto boundary = parent_instructions[consumer_block];
      consumer_block = parent_blocks[consumer_block];
      if (source_block == consumer_block)
        return instruction_positions[source] < instruction_positions[boundary];
    }
    return false;
  };
  const auto verify_operand_dominance = [&](std::uint32_t consumer) {
    const auto &instruction = region.instructions[consumer];
    std::vector<std::uint32_t> operands;
    const auto append = [&](std::uint32_t operand) {
      operands.push_back(operand);
    };
    switch (instruction.opcode) {
    case GenericTemplateOpcode::Add:
    case GenericTemplateOpcode::Equal:
    case GenericTemplateOpcode::Assign:
      append(instruction.arg0);
      append(instruction.arg1);
      break;
    case GenericTemplateOpcode::BindName:
      append(instruction.arg1);
      break;
    case GenericTemplateOpcode::MaterializeTemporary:
    case GenericTemplateOpcode::ExtendTemporary:
      append(instruction.arg1);
      break;
    case GenericTemplateOpcode::BorrowPlace:
    case GenericTemplateOpcode::CarrierView:
    case GenericTemplateOpcode::Return:
    case GenericTemplateOpcode::Yield:
    case GenericTemplateOpcode::If:
    case GenericTemplateOpcode::Assert:
    case GenericTemplateOpcode::Switch:
    case GenericTemplateOpcode::UnionInit:
    case GenericTemplateOpcode::EnumTag:
    case GenericTemplateOpcode::EnumPayloadAccess:
    case GenericTemplateOpcode::StructFieldAccess:
    case GenericTemplateOpcode::MemberAccess:
    case GenericTemplateOpcode::UnionFieldAccess:
    case GenericTemplateOpcode::Dereference:
    case GenericTemplateOpcode::Move:
    case GenericTemplateOpcode::Copy:
    case GenericTemplateOpcode::NumericConvert:
    case GenericTemplateOpcode::CheckedNumericCast:
      append(instruction.arg0);
      break;
    case GenericTemplateOpcode::StaticIndex:
    case GenericTemplateOpcode::Index:
      append(instruction.arg0);
      append(instruction.arg1);
      break;
    default:
      break;
    }
    operands.insert(operands.end(),
                    region.instruction_value_blocks[consumer].begin(),
                    region.instruction_value_blocks[consumer].end());
    return std::ranges::all_of(operands, [&](std::uint32_t operand) {
      return dominates(operand, consumer);
    });
  };
  for (std::uint32_t instruction = 0; instruction < region.instructions.size();
       ++instruction)
    if (!verify_operand_dominance(instruction)) {
      error = "generic template operand is not dominated by its definition";
      return false;
    }
  std::vector<bool> reachable(region.blocks.size());
  std::vector<std::uint32_t> pending{region.entry_block};
  reachable[region.entry_block] = true;
  for (std::size_t index = 0; index < pending.size(); ++index) {
    for (const auto successor : successors[pending[index]]) {
      if (!reachable[successor]) {
        reachable[successor] = true;
        pending.push_back(successor);
      }
    }
  }
  if (std::ranges::any_of(reachable, [](bool value) { return !value; })) {
    error = "generic template artifact has an unreachable block";
    return false;
  }
  return true;
}
} // namespace chtholly::compiler
