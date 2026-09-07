#include "PublicInterfaceConstructionInternal.h"
#include "PublicInterfaceEncodingInternal.h"
#include "chtholly/Compiler/BuiltinOperator.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <functional>
#include <limits>
#include <numeric>
#include <ranges>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace chtholly::compiler::internal {
namespace {

constexpr std::uint32_t UnionFieldUnsafeBit = 1U << 31U;
constexpr std::uint32_t UnionFieldIndexMask = ~UnionFieldUnsafeBit;

} // namespace

std::optional<GenericTemplateArtifact>
PublicInterfaceGenericTemplateConstructionService::build(
    const SemIR &sem_ir, const SemFunction &function,
    std::string_view package_name,
    const std::function<std::optional<PublicType>(TypeId)> &map_type,
    const std::unordered_map<std::uint32_t, IdentifierId>
        &hidden_evaluator_targets,
    std::string &error) {
  GenericTemplateArtifact artifact;
  artifact.generic_parameter_count =
      function.generic.hasValue()
          ? sem_ir.genericValues().generic(function.generic).binding_count
          : 0;
  const auto &function_type = sem_ir.type(function.type);
  const auto function_parameters =
      sem_ir.typeBlock(TypeBlockId(function_type.arg0));
  const auto source_parameters = sem_ir.localBlock(function.parameters);
  artifact.parameter_count =
      function.intrinsic_role == CompilerIntrinsicRole::None &&
              !source_parameters.empty()
          ? static_cast<std::uint32_t>(source_parameters.size())
          : static_cast<std::uint32_t>(function_parameters.size());

  std::vector<InstBlockId> blocks{function.body};
  for (std::size_t block_index = 0; block_index < blocks.size();
       ++block_index) {
    for (const auto inst_id : sem_ir.instBlock(blocks[block_index])) {
      const auto &inst = sem_ir.inst(inst_id);
      if (inst.kind == SemInstKind::If || inst.kind == SemInstKind::IfArm ||
          inst.kind == SemInstKind::While || inst.kind == SemInstKind::For ||
          inst.kind == SemInstKind::ForClause ||
          inst.kind == SemInstKind::DoWhile ||
          inst.kind == SemInstKind::Defer ||
          inst.kind == SemInstKind::ScopedBlock ||
          inst.kind == SemInstKind::Switch ||
          inst.kind == SemInstKind::SwitchArm) {
        const auto add_block = [&](InstBlockId nested) {
          if (std::ranges::find(blocks, nested) == blocks.end())
            blocks.push_back(nested);
        };
        if (inst.kind == SemInstKind::While ||
            inst.kind == SemInstKind::DoWhile || inst.kind == SemInstKind::For)
          add_block(InstBlockId(inst.arg0));
        if (inst.kind == SemInstKind::Defer ||
            inst.kind == SemInstKind::ScopedBlock)
          add_block(InstBlockId(inst.arg0));
        else if (inst.kind == SemInstKind::IfArm)
          add_block(InstBlockId(inst.arg0));
        else
          add_block(InstBlockId(inst.arg1));
      }
    }
  }
  std::vector<InstId> instructions;
  for (const auto block : blocks) {
    const auto values = sem_ir.instBlock(block);
    instructions.insert(instructions.end(), values.begin(), values.end());
  }
  std::ranges::sort(instructions, {}, &InstId::index);
  instructions.erase(std::unique(instructions.begin(), instructions.end()),
                     instructions.end());
  std::unordered_map<std::uint32_t, std::uint32_t> inst_map;
  for (std::uint32_t index = 0; index < instructions.size(); ++index)
    inst_map.emplace(instructions[index].index, index);

  std::unordered_map<std::uint32_t, std::uint32_t> local_map;
  const auto add_local = [&](LocalId local) {
    if (const auto found = local_map.find(local.index);
        found != local_map.end())
      return found->second;
    const auto type = map_type(sem_ir.local(local).type);
    if (!type)
      return core::AnyId::InvalidIndex;
    const auto index = static_cast<std::uint32_t>(artifact.local_types.size());
    artifact.local_types.push_back(*type);
    artifact.local_flags.push_back(sem_ir.local(local).flags);
    local_map.emplace(local.index, index);
    return index;
  };
  if (function.intrinsic_role == CompilerIntrinsicRole::None &&
      !source_parameters.empty()) {
    for (const auto parameter : source_parameters) {
      if (add_local(parameter) == core::AnyId::InvalidIndex) {
        error = "public generic template has an unsupported local type";
        return std::nullopt;
      }
    }
  } else {
    for (const auto parameter :
         sem_ir.typeBlock(TypeBlockId(function_type.arg0))) {
      auto type = map_type(parameter);
      if (!type) {
        error = "public intrinsic template has an unsupported parameter type";
        return std::nullopt;
      }
      artifact.local_types.push_back(std::move(*type));
      artifact.local_flags.push_back(0U);
    }
  }
  for (const auto parameter :
       sem_ir.typeBlock(TypeBlockId(function_type.arg0))) {
    auto type = map_type(parameter);
    if (!type) {
      error = "public generic template has an unsupported parameter type";
      return std::nullopt;
    }
    artifact.declaration.results.push_back(std::move(*type));
  }
  auto return_type = map_type(TypeId(function_type.arg1));
  if (!return_type) {
    error = "public generic template has an unsupported return type";
    return std::nullopt;
  }
  artifact.declaration.results.push_back(std::move(*return_type));

  if (function.intrinsic_role != CompilerIntrinsicRole::None) {
    artifact.definition.entry_block = 0;
    artifact.definition.blocks.emplace_back();
    if (!artifact.verify(error))
      return std::nullopt;
    return artifact;
  }

  std::unordered_map<std::uint32_t, std::uint32_t> integer_map;
  std::unordered_map<std::string, std::uint32_t> string_map;
  std::unordered_map<std::uint32_t, std::uint32_t> query_map;
  std::unordered_map<std::string, std::uint32_t> callee_map;
  for (const auto old_id : instructions) {
    const auto &old = sem_ir.inst(old_id);
    GenericTemplateInstArtifact value;
    std::vector<std::uint32_t> instruction_values;
    auto instruction_type = map_type(TypeId(old.type));
    if (!instruction_type) {
      error = "public generic template has an unsupported instruction type";
      return std::nullopt;
    }
    value.type = std::move(*instruction_type);
    const auto inst_arg = [&](std::uint32_t raw) {
      const auto found = inst_map.find(raw);
      return found == inst_map.end() ? core::AnyId::InvalidIndex
                                     : found->second;
    };
    switch (old.kind) {
    case SemInstKind::Parameter:
      value.opcode = GenericTemplateOpcode::Parameter;
      value.arg0 = add_local(LocalId(old.arg0));
      break;
    case SemInstKind::IntegerLiteral: {
      value.opcode = GenericTemplateOpcode::IntegerLiteral;
      const auto [found, inserted] = integer_map.emplace(
          old.arg0, static_cast<std::uint32_t>(artifact.integers.size()));
      if (inserted)
        artifact.integers.push_back(sem_ir.integer(IntegerId(old.arg0)));
      value.arg0 = found->second;
      break;
    }
    case SemInstKind::FloatLiteral: {
      value.opcode = GenericTemplateOpcode::FloatLiteral;
      const auto [found, inserted] = integer_map.emplace(
          old.arg0, static_cast<std::uint32_t>(artifact.integers.size()));
      if (inserted)
        artifact.integers.push_back(sem_ir.integer(IntegerId(old.arg0)));
      value.arg0 = found->second;
      break;
    }
    case SemInstKind::CharLiteral: {
      value.opcode = GenericTemplateOpcode::CharLiteral;
      const auto [found, inserted] = integer_map.emplace(
          old.arg0, static_cast<std::uint32_t>(artifact.integers.size()));
      if (inserted)
        artifact.integers.push_back(sem_ir.integer(IntegerId(old.arg0)));
      value.arg0 = found->second;
      break;
    }
    case SemInstKind::StringLiteral: {
      value.opcode = GenericTemplateOpcode::StringLiteral;
      const auto spelling = std::string(
          sem_ir.string(StringLiteralId(static_cast<std::uint32_t>(old.arg0))));
      const auto [found, inserted] = string_map.emplace(
          spelling, static_cast<std::uint32_t>(artifact.strings.size()));
      if (inserted)
        artifact.strings.push_back(spelling);
      value.arg0 = found->second;
      break;
    }
    case SemInstKind::ArrayLiteral:
      value.opcode = GenericTemplateOpcode::ArrayLiteral;
      for (const auto element : sem_ir.instBlock(InstBlockId(old.arg0))) {
        const auto mapped = inst_arg(element.index);
        if (mapped == core::AnyId::InvalidIndex) {
          error = "public generic array elements are not canonical";
          return std::nullopt;
        }
        instruction_values.push_back(mapped);
      }
      break;
    case SemInstKind::NullPointer:
      value.opcode = GenericTemplateOpcode::NullPointer;
      break;
    case SemInstKind::TypeQuery: {
      value.opcode = GenericTemplateOpcode::TypeQuery;
      const auto query_index =
          static_cast<std::uint32_t>(sem_ir.integer(IntegerId(old.arg0)));
      const auto [found, inserted] = query_map.emplace(
          query_index,
          static_cast<std::uint32_t>(artifact.type_queries.size()));
      if (inserted) {
        if (query_index >= sem_ir.typeQueryCount()) {
          error = "public generic type query index is out of range";
          return std::nullopt;
        }
        const auto &query = sem_ir.typeQuery(query_index);
        GenericTypeQueryArtifact descriptor;
        descriptor.kind = static_cast<GenericTypeQueryKind>(query.kind);
        descriptor.source = map_type(query.source).value_or(PublicType{});
        if (query.kind == SemTypeQueryArtifact::Kind::TypeSame)
          descriptor.other = map_type(query.other).value_or(PublicType{});
        descriptor.property = query.property;
        if (descriptor.source.kind == PublicTypeKind::Count ||
            (query.kind == SemTypeQueryArtifact::Kind::TypeSame &&
             descriptor.other.kind == PublicTypeKind::Count)) {
          error = "public generic type query has an unsupported source type";
          return std::nullopt;
        }
        artifact.type_queries.push_back(std::move(descriptor));
      }
      value.arg0 = found->second;
      break;
    }
    case SemInstKind::BoolLiteral:
      value.opcode = GenericTemplateOpcode::BoolLiteral;
      value.arg0 =
          static_cast<std::uint32_t>(sem_ir.integer(IntegerId(old.arg0)));
      break;
    case SemInstKind::VoidValue:
      value.opcode = GenericTemplateOpcode::VoidValue;
      break;
    case SemInstKind::NameRef:
      value.opcode = GenericTemplateOpcode::NameRef;
      value.arg0 = add_local(LocalId(old.arg0));
      break;
    case SemInstKind::BorrowLocal:
      value.opcode = GenericTemplateOpcode::BorrowLocal;
      value.arg0 = add_local(LocalId(old.arg0));
      break;
    case SemInstKind::BorrowPlace:
      value.opcode = GenericTemplateOpcode::BorrowPlace;
      value.arg0 = inst_arg(old.arg0);
      break;
    case SemInstKind::CarrierView:
      value.opcode = GenericTemplateOpcode::CarrierView;
      value.arg0 = inst_arg(old.arg0);
      value.arg1 =
          static_cast<std::uint32_t>(sem_ir.integer(IntegerId(old.arg1)));
      break;
    case SemInstKind::Dereference:
      value.opcode = GenericTemplateOpcode::Dereference;
      value.arg0 = inst_arg(old.arg0);
      break;
    case SemInstKind::Move:
      value.opcode = GenericTemplateOpcode::Move;
      value.arg0 = inst_arg(old.arg0);
      break;
    case SemInstKind::Copy:
      value.opcode = GenericTemplateOpcode::Copy;
      value.arg0 = inst_arg(old.arg0);
      break;
    case SemInstKind::BuiltinUnary:
      value.opcode = GenericTemplateOpcode::BuiltinUnary;
      value.arg0 =
          static_cast<std::uint32_t>(sem_ir.integer(IntegerId(old.arg1)));
      instruction_values.push_back(inst_arg(old.arg0));
      break;
    case SemInstKind::BuiltinBinary:
      value.opcode = GenericTemplateOpcode::BuiltinBinary;
      value.arg0 =
          static_cast<std::uint32_t>(sem_ir.integer(IntegerId(old.arg1)));
      for (const auto operand : sem_ir.instBlock(InstBlockId(old.arg0)))
        instruction_values.push_back(inst_arg(operand.index));
      break;
    case SemInstKind::Add:
      value.opcode = GenericTemplateOpcode::Add;
      value.arg0 = inst_arg(old.arg0);
      value.arg1 = inst_arg(old.arg1);
      break;
    case SemInstKind::NumericConvert:
      value.opcode = GenericTemplateOpcode::NumericConvert;
      value.arg0 = inst_arg(old.arg0);
      break;
    case SemInstKind::CheckedNumericCast:
      value.opcode = GenericTemplateOpcode::CheckedNumericCast;
      value.arg0 = inst_arg(old.arg0);
      break;
    case SemInstKind::Equal:
      value.opcode = GenericTemplateOpcode::Equal;
      value.arg0 = inst_arg(old.arg0);
      value.arg1 = inst_arg(old.arg1);
      break;
    case SemInstKind::MaterializeTemporary:
      value.opcode = GenericTemplateOpcode::MaterializeTemporary;
      value.arg0 = add_local(LocalId(old.arg0));
      value.arg1 = inst_arg(old.arg1);
      break;
    case SemInstKind::EndFullExpression:
      value.opcode = GenericTemplateOpcode::EndFullExpression;
      for (const auto temporary : sem_ir.instBlock(InstBlockId(old.arg0)))
        instruction_values.push_back(inst_arg(temporary.index));
      break;
    case SemInstKind::ExtendTemporary:
      value.opcode = GenericTemplateOpcode::ExtendTemporary;
      value.arg0 = add_local(LocalId(old.arg0));
      value.arg1 = inst_arg(old.arg1);
      break;
    case SemInstKind::Assert:
      value.opcode = GenericTemplateOpcode::Assert;
      value.arg0 = inst_arg(old.arg0);
      value.arg1 =
          static_cast<std::uint32_t>(sem_ir.integer(IntegerId(old.arg1)));
      break;
    case SemInstKind::UnrecoverableFailure:
      value.opcode = GenericTemplateOpcode::UnrecoverableFailure;
      value.arg0 =
          static_cast<std::uint32_t>(sem_ir.integer(IntegerId(old.arg0)));
      break;
    case SemInstKind::BindName:
      value.opcode = GenericTemplateOpcode::BindName;
      value.arg0 = add_local(LocalId(old.arg0));
      value.arg1 = inst_arg(old.arg1);
      break;
    case SemInstKind::Assign:
      value.opcode = GenericTemplateOpcode::Assign;
      value.arg0 = inst_arg(old.arg0);
      value.arg1 = inst_arg(old.arg1);
      break;
    case SemInstKind::Return:
      value.opcode = GenericTemplateOpcode::Return;
      value.arg0 = inst_arg(old.arg0);
      break;
    case SemInstKind::Defer:
      value.opcode = GenericTemplateOpcode::Defer;
      value.arg0 = static_cast<std::uint32_t>(
          std::ranges::find(blocks, InstBlockId(old.arg0)) - blocks.begin());
      break;
    case SemInstKind::ScopedBlock:
      value.opcode = GenericTemplateOpcode::ScopedBlock;
      value.arg0 = static_cast<std::uint32_t>(
          std::ranges::find(blocks, InstBlockId(old.arg0)) - blocks.begin());
      break;
    case SemInstKind::If:
      value.opcode = GenericTemplateOpcode::If;
      value.arg0 = inst_arg(old.arg0);
      value.arg1 = static_cast<std::uint32_t>(
          std::ranges::find(blocks, InstBlockId(old.arg1)) - blocks.begin());
      break;
    case SemInstKind::IfArm:
      value.opcode = GenericTemplateOpcode::IfArm;
      value.arg0 = static_cast<std::uint32_t>(
          std::ranges::find(blocks, InstBlockId(old.arg0)) - blocks.begin());
      break;
    case SemInstKind::While:
      value.opcode = GenericTemplateOpcode::While;
      value.arg0 = static_cast<std::uint32_t>(
          std::ranges::find(blocks, InstBlockId(old.arg0)) - blocks.begin());
      value.arg1 = static_cast<std::uint32_t>(
          std::ranges::find(blocks, InstBlockId(old.arg1)) - blocks.begin());
      break;
    case SemInstKind::For:
      value.opcode = GenericTemplateOpcode::For;
      value.arg0 = static_cast<std::uint32_t>(
          std::ranges::find(blocks, InstBlockId(old.arg0)) - blocks.begin());
      value.arg1 = static_cast<std::uint32_t>(
          std::ranges::find(blocks, InstBlockId(old.arg1)) - blocks.begin());
      break;
    case SemInstKind::ForClause:
      value.opcode = GenericTemplateOpcode::ForClause;
      value.arg0 =
          static_cast<std::uint32_t>(sem_ir.integer(IntegerId(old.arg0)));
      value.arg1 = static_cast<std::uint32_t>(
          std::ranges::find(blocks, InstBlockId(old.arg1)) - blocks.begin());
      break;
    case SemInstKind::DoWhile:
      value.opcode = GenericTemplateOpcode::DoWhile;
      value.arg0 = static_cast<std::uint32_t>(
          std::ranges::find(blocks, InstBlockId(old.arg0)) - blocks.begin());
      value.arg1 = static_cast<std::uint32_t>(
          std::ranges::find(blocks, InstBlockId(old.arg1)) - blocks.begin());
      break;
    case SemInstKind::Break:
      value.opcode = GenericTemplateOpcode::Break;
      value.arg0 =
          static_cast<std::uint32_t>(sem_ir.integer(IntegerId(old.arg0)));
      break;
    case SemInstKind::Continue:
      value.opcode = GenericTemplateOpcode::Continue;
      value.arg0 =
          static_cast<std::uint32_t>(sem_ir.integer(IntegerId(old.arg0)));
      break;
    case SemInstKind::Yield:
      value.opcode = GenericTemplateOpcode::Yield;
      value.arg0 = inst_arg(old.arg0);
      break;
    case SemInstKind::Switch:
      value.opcode = GenericTemplateOpcode::Switch;
      value.arg0 = inst_arg(old.arg0);
      value.arg1 = static_cast<std::uint32_t>(
          std::ranges::find(blocks, InstBlockId(old.arg1)) - blocks.begin());
      break;
    case SemInstKind::SwitchArm: {
      const auto variant = sem_ir.integer(IntegerId(old.arg0));
      if (variant < -1 || static_cast<std::uint64_t>(variant) >
                              std::numeric_limits<std::uint32_t>::max()) {
        error = "public generic switch arm has an invalid variant";
        return std::nullopt;
      }
      value.opcode = GenericTemplateOpcode::SwitchArm;
      value.arg0 = variant < 0 ? core::AnyId::InvalidIndex
                               : static_cast<std::uint32_t>(variant);
      value.arg1 = static_cast<std::uint32_t>(
          std::ranges::find(blocks, InstBlockId(old.arg1)) - blocks.begin());
      break;
    }
    case SemInstKind::Call:
    case SemInstKind::CompilerIntrinsicCall: {
      value.opcode = old.kind == SemInstKind::CompilerIntrinsicCall
                         ? GenericTemplateOpcode::CompilerIntrinsicCall
                         : GenericTemplateOpcode::Call;
      const auto &reference = sem_ir.functionRef(FunctionRefId(old.arg0));
      PublicEntityReferenceArtifact callee;
      if (reference.public_entity.hasValue()) {
        const auto *entity =
            sem_ir.importIRs().tryGetEntity(reference.public_entity);
        if (!entity) {
          error = "public generic template has an unresolved public callee";
          return std::nullopt;
        }
        callee = {PublicEntityKind::Function,
                  std::string(sem_ir.identifier(entity->package_name)),
                  std::string(sem_ir.identifier(entity->module_name)),
                  std::string(sem_ir.identifier(entity->name)),
                  entity->fingerprint};
      } else if (reference.local_function.hasValue()) {
        auto canonical_id = reference.local_function;
        if (reference.generic.hasValue()) {
          for (std::uint32_t function_index = 0;
               function_index < sem_ir.functionCount(); ++function_index) {
            const auto candidate_id = FunctionId(function_index);
            const auto &candidate = sem_ir.function(FunctionId(function_index));
            if (candidate.generic == reference.generic &&
                (candidate.flags & SemFunctionTemplate) != 0) {
              canonical_id = candidate_id;
              break;
            }
          }
        }
        const auto &canonical_function = sem_ir.function(canonical_id);
        bool is_public = (canonical_function.flags & SemFunctionPublic) != 0;
        if (canonical_function.semantic_owner.hasValue()) {
          const auto &owner =
              sem_ir.nominalType(canonical_function.semantic_owner);
          const auto member = std::ranges::find_if(
              owner.member_functions, [&](const auto &candidate) {
                return candidate.target.hasValue() &&
                       sem_ir.functionRef(candidate.target).local_function ==
                           canonical_id;
              });
          is_public = member != owner.member_functions.end() &&
                      (member->flags & SemNominalMemberFunctionPublic) != 0;
        }
        const auto hidden = hidden_evaluator_targets.find(canonical_id.index);
        if (!is_public && hidden == hidden_evaluator_targets.end()) {
          error = "public generic template calls a private function";
          return std::nullopt;
        }
        auto canonical_name =
            hidden != hidden_evaluator_targets.end()
                ? std::string(sem_ir.identifier(hidden->second))
                : std::string(sem_ir.identifier(
                      sem_ir.name(canonical_function.name).text));
        if (canonical_function.semantic_owner.hasValue()) {
          const auto &owner =
              sem_ir.nominalType(canonical_function.semantic_owner);
          canonical_name =
              std::string(sem_ir.identifier(sem_ir.name(owner.name).text)) +
              "::" + canonical_name;
        }
        callee = {PublicEntityKind::Function, std::string(package_name),
                  std::string(sem_ir.identifier(sem_ir.moduleName())),
                  std::move(canonical_name)};
      } else {
        error = "public generic template has a non-canonical callee";
        return std::nullopt;
      }
      std::string key;
      appendField(key, callee.canonical_package);
      appendField(key, callee.canonical_module);
      appendField(key, callee.canonical_name);
      const auto concrete_arguments =
          sem_ir.functionRefConcreteArguments(FunctionRefId(old.arg0));
      for (const auto &argument : concrete_arguments)
        appendType(key, argument);
      const auto [found, inserted] = callee_map.emplace(
          key, static_cast<std::uint32_t>(artifact.callees.size()));
      if (inserted) {
        artifact.callees.push_back(std::move(callee));
        artifact.callee_type_arguments.emplace_back(concrete_arguments.begin(),
                                                    concrete_arguments.end());
      }
      value.arg0 = found->second;
      value.arg1 = core::AnyId::InvalidIndex;
      for (const auto argument : sem_ir.instBlock(InstBlockId(old.arg1))) {
        const auto mapped = inst_arg(argument.index);
        if (mapped == core::AnyId::InvalidIndex) {
          error = "public generic call arguments are not canonical";
          return std::nullopt;
        }
        instruction_values.push_back(mapped);
      }
      break;
    }
    case SemInstKind::InterfaceCall: {
      value.opcode = GenericTemplateOpcode::InterfaceCall;
      const auto [found, inserted] = integer_map.emplace(
          old.arg0, static_cast<std::uint32_t>(artifact.integers.size()));
      if (inserted)
        artifact.integers.push_back(sem_ir.integer(IntegerId(old.arg0)));
      value.arg0 = found->second;
      for (const auto argument : sem_ir.instBlock(InstBlockId(old.arg1)))
        instruction_values.push_back(inst_arg(argument.index));
      break;
    }
    case SemInstKind::FunctionValue: {
      value.opcode = GenericTemplateOpcode::FunctionValue;
      const auto &reference = sem_ir.functionRef(FunctionRefId(old.arg0));
      PublicEntityReferenceArtifact callee;
      if (reference.public_entity.hasValue()) {
        const auto *entity =
            sem_ir.importIRs().tryGetEntity(reference.public_entity);
        if (!entity) {
          error = "public generic function value has an unresolved target";
          return std::nullopt;
        }
        callee = {PublicEntityKind::Function,
                  std::string(sem_ir.identifier(entity->package_name)),
                  std::string(sem_ir.identifier(entity->module_name)),
                  std::string(sem_ir.identifier(entity->name)),
                  entity->fingerprint};
      } else if (reference.local_function.hasValue()) {
        auto target_id = reference.local_function;
        if (reference.generic.hasValue()) {
          for (std::uint32_t function_index = 0;
               function_index < sem_ir.functionCount(); ++function_index) {
            const auto candidate_id = FunctionId(function_index);
            const auto &candidate = sem_ir.function(candidate_id);
            if (candidate.generic == reference.generic &&
                (candidate.flags & SemFunctionTemplate) != 0) {
              target_id = candidate_id;
              break;
            }
          }
        }
        const auto &target = sem_ir.function(target_id);
        bool is_public = (target.flags & SemFunctionPublic) != 0;
        if (target.semantic_owner.hasValue()) {
          const auto &owner = sem_ir.nominalType(target.semantic_owner);
          const auto member = std::ranges::find_if(
              owner.member_functions, [&](const auto &candidate) {
                return candidate.target.hasValue() &&
                       sem_ir.functionRef(candidate.target).local_function ==
                           target_id;
              });
          is_public = member != owner.member_functions.end() &&
                      (member->flags & SemNominalMemberFunctionPublic) != 0 &&
                      (member->flags & SemNominalMemberFunctionAssociated) != 0;
        }
        if (!is_public) {
          error = "public generic function value targets a private function";
          return std::nullopt;
        }
        auto canonical_name =
            std::string(sem_ir.identifier(sem_ir.name(target.name).text));
        if (target.semantic_owner.hasValue()) {
          const auto &owner = sem_ir.nominalType(target.semantic_owner);
          canonical_name =
              std::string(sem_ir.identifier(sem_ir.name(owner.name).text)) +
              "::" + canonical_name;
        }
        callee = {PublicEntityKind::Function, std::string(package_name),
                  std::string(sem_ir.identifier(sem_ir.moduleName())),
                  std::move(canonical_name)};
      } else {
        error = "public generic function value has no canonical target";
        return std::nullopt;
      }
      std::string key;
      appendField(key, callee.canonical_package);
      appendField(key, callee.canonical_module);
      appendField(key, callee.canonical_name);
      const auto concrete_arguments =
          sem_ir.functionRefConcreteArguments(FunctionRefId(old.arg0));
      for (const auto &argument : concrete_arguments)
        appendType(key, argument);
      const auto [found, inserted] = callee_map.emplace(
          key, static_cast<std::uint32_t>(artifact.callees.size()));
      if (inserted) {
        artifact.callees.push_back(std::move(callee));
        artifact.callee_type_arguments.emplace_back(concrete_arguments.begin(),
                                                    concrete_arguments.end());
      }
      value.arg0 = found->second;
      break;
    }
    case SemInstKind::Closure: {
      value.opcode = GenericTemplateOpcode::Closure;
      const auto &reference = sem_ir.functionRef(FunctionRefId(old.arg0));
      if (!reference.local_function.hasValue()) {
        error = "public generic closure has no canonical hidden target";
        return std::nullopt;
      }
      auto target_id = reference.local_function;
      if (reference.generic.hasValue()) {
        for (std::uint32_t function_index = 0;
             function_index < sem_ir.functionCount(); ++function_index) {
          const auto candidate_id = FunctionId(function_index);
          const auto &candidate = sem_ir.function(candidate_id);
          if (candidate.generic == reference.generic &&
              (candidate.flags & SemFunctionTemplate) != 0) {
            target_id = candidate_id;
            break;
          }
        }
      }
      const auto hidden = hidden_evaluator_targets.find(target_id.index);
      const auto &target = sem_ir.function(target_id);
      if (hidden == hidden_evaluator_targets.end() &&
          (target.flags & SemFunctionPublic) == 0) {
        error = "public generic closure target is not artifact-visible";
        return std::nullopt;
      }
      const auto target_name = hidden != hidden_evaluator_targets.end()
                                   ? hidden->second
                                   : sem_ir.name(target.name).text;
      PublicEntityReferenceArtifact callee{
          PublicEntityKind::Function, std::string(package_name),
          std::string(sem_ir.identifier(sem_ir.moduleName())),
          std::string(sem_ir.identifier(target_name))};
      std::string key;
      appendField(key, callee.canonical_package);
      appendField(key, callee.canonical_module);
      appendField(key, callee.canonical_name);
      const auto concrete_arguments =
          sem_ir.functionRefConcreteArguments(FunctionRefId(old.arg0));
      for (const auto &argument : concrete_arguments)
        appendType(key, argument);
      const auto [found, inserted] = callee_map.emplace(
          key, static_cast<std::uint32_t>(artifact.callees.size()));
      if (inserted) {
        artifact.callees.push_back(std::move(callee));
        artifact.callee_type_arguments.emplace_back(concrete_arguments.begin(),
                                                    concrete_arguments.end());
      }
      const auto *info = sem_ir.tryGetCallableEnvironment(TypeId(old.type));
      if (!info || info->kind != SemCallableEnvironmentKind::Closure) {
        error = "public generic closure has no callable-environment facts";
        return std::nullopt;
      }
      value.arg0 = found->second;
      value.arg1 = static_cast<std::uint32_t>(info->capability);
      for (const auto capture : sem_ir.instBlock(InstBlockId(old.arg1))) {
        const auto mapped = inst_arg(capture.index);
        if (mapped == core::AnyId::InvalidIndex) {
          error = "public generic closure captures are not canonical";
          return std::nullopt;
        }
        instruction_values.push_back(mapped);
      }
      break;
    }
    case SemInstKind::BoundMethod: {
      value.opcode = GenericTemplateOpcode::BoundMethod;
      const auto mapped_receiver = inst_arg(old.arg0);
      if (mapped_receiver == core::AnyId::InvalidIndex) {
        error = "public generic bound-method receiver is not canonical";
        return std::nullopt;
      }
      const auto &reference = sem_ir.functionRef(FunctionRefId(old.arg1));
      PublicEntityReferenceArtifact callee;
      FunctionRefId target_ref = FunctionRefId(old.arg1);
      if (reference.public_entity.hasValue()) {
        const auto *entity =
            sem_ir.importIRs().tryGetEntity(reference.public_entity);
        if (!entity || entity->member_kind !=
                           PublicFunctionArtifact::MemberKind::Instance) {
          error = "public generic bound method has a non-instance target";
          return std::nullopt;
        }
        callee = {PublicEntityKind::Function,
                  std::string(sem_ir.identifier(entity->package_name)),
                  std::string(sem_ir.identifier(entity->module_name)),
                  std::string(sem_ir.identifier(entity->name)),
                  entity->fingerprint};
      } else if (reference.local_function.hasValue()) {
        auto target_id = reference.local_function;
        if (reference.generic.hasValue()) {
          for (std::uint32_t function_index = 0;
               function_index < sem_ir.functionCount(); ++function_index) {
            const auto candidate_id = FunctionId(function_index);
            const auto &candidate = sem_ir.function(candidate_id);
            if (candidate.generic == reference.generic &&
                (candidate.flags & SemFunctionTemplate) != 0) {
              target_id = candidate_id;
              break;
            }
          }
        }
        const auto &target = sem_ir.function(target_id);
        if (!target.semantic_owner.hasValue()) {
          error =
              "public generic bound method target is not an instance member";
          return std::nullopt;
        }
        const auto &owner = sem_ir.nominalType(target.semantic_owner);
        const auto member = std::ranges::find_if(
            owner.member_functions, [&](const auto &candidate) {
              return candidate.target.hasValue() &&
                     sem_ir.functionRef(candidate.target).local_function ==
                         target_id &&
                     (candidate.flags & SemNominalMemberFunctionAssociated) ==
                         0;
            });
        if (member == owner.member_functions.end() ||
            (member->flags & SemNominalMemberFunctionPublic) == 0) {
          error = "public generic bound method targets a private member";
          return std::nullopt;
        }
        auto canonical_name =
            std::string(sem_ir.identifier(sem_ir.name(owner.name).text));
        canonical_name += "::";
        canonical_name +=
            std::string(sem_ir.identifier(sem_ir.name(target.name).text));
        callee = {PublicEntityKind::Function, std::string(package_name),
                  std::string(sem_ir.identifier(sem_ir.moduleName())),
                  std::move(canonical_name)};
      } else {
        error = "public generic bound method has no canonical target";
        return std::nullopt;
      }
      const auto target_type = sem_ir.type(reference.local_type);
      if (target_type.kind != SemTypeKind::Function) {
        error = "public generic bound method target has an invalid type";
        return std::nullopt;
      }
      const auto parameters = sem_ir.typeBlock(TypeBlockId(target_type.arg0));
      if (parameters.empty()) {
        error = "public generic bound method target has no receiver";
        return std::nullopt;
      }
      const auto &receiver_parameter = sem_ir.type(parameters.front());
      const auto capability =
          receiver_parameter.kind == SemTypeKind::Reference
              ? sem_ir.referenceMutability(parameters.front()) ==
                        SemReferenceMutability::Mutable
                    ? static_cast<std::uint32_t>(
                          SemCallableEnvironmentCapability::Mutable)
                    : static_cast<std::uint32_t>(
                          SemCallableEnvironmentCapability::ReadOnly)
              : static_cast<std::uint32_t>(
                    SemCallableEnvironmentCapability::Consuming);
      std::string key;
      appendField(key, callee.canonical_package);
      appendField(key, callee.canonical_module);
      appendField(key, callee.canonical_name);
      appendField(key, std::to_string(capability));
      const auto concrete_arguments =
          sem_ir.functionRefConcreteArguments(target_ref);
      for (const auto &argument : concrete_arguments)
        appendType(key, argument);
      const auto [found, inserted] = callee_map.emplace(
          key, static_cast<std::uint32_t>(artifact.callees.size()));
      if (inserted) {
        artifact.callees.push_back(std::move(callee));
        artifact.callee_type_arguments.emplace_back(concrete_arguments.begin(),
                                                    concrete_arguments.end());
      }
      value.arg0 = found->second;
      value.arg1 = capability;
      instruction_values.push_back(mapped_receiver);
      break;
    }
    case SemInstKind::IndirectCall:
      value.opcode = GenericTemplateOpcode::IndirectCall;
      value.arg0 = inst_arg(old.arg0);
      for (const auto argument : sem_ir.instBlock(InstBlockId(old.arg1))) {
        const auto mapped = inst_arg(argument.index);
        if (mapped == core::AnyId::InvalidIndex) {
          error = "public generic indirect-call operands are not canonical";
          return std::nullopt;
        }
        instruction_values.push_back(mapped);
      }
      break;
    case SemInstKind::AggregateInit: {
      value.opcode = GenericTemplateOpcode::AggregateInit;
      for (const auto field : sem_ir.instBlock(InstBlockId(old.arg0))) {
        const auto mapped = inst_arg(field.index);
        if (mapped == core::AnyId::InvalidIndex) {
          error = "public generic aggregate fields are not canonical";
          return std::nullopt;
        }
        instruction_values.push_back(mapped);
      }
      break;
    }
    case SemInstKind::TupleLiteral: {
      value.opcode = GenericTemplateOpcode::TupleLiteral;
      for (const auto element : sem_ir.instBlock(InstBlockId(old.arg0))) {
        const auto mapped = inst_arg(element.index);
        if (mapped == core::AnyId::InvalidIndex) {
          error = "public generic tuple elements are not canonical";
          return std::nullopt;
        }
        instruction_values.push_back(mapped);
      }
      break;
    }
    case SemInstKind::Slice: {
      value.opcode = GenericTemplateOpcode::Slice;
      for (const auto operand : sem_ir.instBlock(InstBlockId(old.arg0))) {
        const auto mapped = inst_arg(operand.index);
        if (mapped == core::AnyId::InvalidIndex) {
          error = "public generic slice operands are not canonical";
          return std::nullopt;
        }
        instruction_values.push_back(mapped);
      }
      break;
    }
    case SemInstKind::UnionInit: {
      const auto member = sem_ir.integer(IntegerId(old.arg1));
      if (member < 0 ||
          static_cast<std::uint64_t>(member) > UnionFieldIndexMask) {
        error = "public generic union initializer has an invalid member";
        return std::nullopt;
      }
      value.opcode = GenericTemplateOpcode::UnionInit;
      value.arg0 = inst_arg(old.arg0);
      value.arg1 = static_cast<std::uint32_t>(member);
      break;
    }
    case SemInstKind::EnumInit: {
      const auto variant = sem_ir.integer(IntegerId(old.arg1));
      if (variant < 0 || static_cast<std::uint64_t>(variant) >
                             std::numeric_limits<std::uint32_t>::max()) {
        error = "public generic enum initializer has an invalid variant";
        return std::nullopt;
      }
      value.opcode = GenericTemplateOpcode::EnumInit;
      for (const auto field : sem_ir.instBlock(InstBlockId(old.arg0))) {
        const auto mapped = inst_arg(field.index);
        if (mapped == core::AnyId::InvalidIndex) {
          error = "public generic enum payload is not canonical";
          return std::nullopt;
        }
        instruction_values.push_back(mapped);
      }
      value.arg1 = static_cast<std::uint32_t>(variant);
      break;
    }
    case SemInstKind::EnumTag:
      value.opcode = GenericTemplateOpcode::EnumTag;
      value.arg0 = inst_arg(old.arg0);
      break;
    case SemInstKind::EnumPayloadAccess: {
      value.opcode = GenericTemplateOpcode::EnumPayloadAccess;
      value.arg0 = inst_arg(old.arg0);
      const auto [found, inserted] = integer_map.emplace(
          old.arg1, static_cast<std::uint32_t>(artifact.integers.size()));
      if (inserted)
        artifact.integers.push_back(sem_ir.integer(IntegerId(old.arg1)));
      value.arg1 = found->second;
      break;
    }
    case SemInstKind::StructFieldAccess: {
      const auto field = sem_ir.integer(IntegerId(old.arg1));
      if (field < 0 || static_cast<std::uint64_t>(field) >
                           std::numeric_limits<std::uint32_t>::max()) {
        error = "public generic field projection has an invalid index";
        return std::nullopt;
      }
      value.opcode = GenericTemplateOpcode::StructFieldAccess;
      value.arg0 = inst_arg(old.arg0);
      value.arg1 = static_cast<std::uint32_t>(field);
      break;
    }
    case SemInstKind::MemberAccess: {
      value.opcode = GenericTemplateOpcode::MemberAccess;
      value.arg0 = inst_arg(old.arg0);
      const auto spelling =
          std::string(sem_ir.identifier(sem_ir.name(NameId(old.arg1)).text));
      const auto [found, inserted] = string_map.emplace(
          spelling, static_cast<std::uint32_t>(artifact.strings.size()));
      if (inserted)
        artifact.strings.push_back(spelling);
      value.arg1 = found->second;
      break;
    }
    case SemInstKind::Index:
      value.arg0 = inst_arg(old.arg0);
      value.arg1 = inst_arg(old.arg1);
      if (value.arg0 == core::AnyId::InvalidIndex ||
          value.arg1 == core::AnyId::InvalidIndex) {
        error = "public generic index projection is not canonical";
        return std::nullopt;
      }
      value.opcode =
          sem_ir.inst(InstId(old.arg1)).kind == SemInstKind::IntegerLiteral
              ? GenericTemplateOpcode::StaticIndex
              : GenericTemplateOpcode::Index;
      break;
    case SemInstKind::UnionFieldAccess: {
      const auto encoded_field = sem_ir.integer(IntegerId(old.arg1));
      const auto unsafe = encoded_field < 0;
      const auto field = unsafe ? -encoded_field - 1 : encoded_field;
      if (field < 0 ||
          static_cast<std::uint64_t>(field) >= UnionFieldIndexMask) {
        error = "public generic union projection has an invalid index";
        return std::nullopt;
      }
      value.opcode = GenericTemplateOpcode::UnionFieldAccess;
      value.arg0 = inst_arg(old.arg0);
      value.arg1 = static_cast<std::uint32_t>(field) |
                   (unsafe ? UnionFieldUnsafeBit : 0U);
      break;
    }
    default:
      error = "public generic template contains unsupported instruction " +
              std::string(semInstKindName(old.kind));
      return std::nullopt;
    }
    artifact.definition.instructions.push_back(value);
    artifact.definition.instruction_value_blocks.push_back(
        std::move(instruction_values));
  }
  for (const auto block : blocks) {
    std::vector<std::uint32_t> result;
    for (const auto inst : sem_ir.instBlock(block))
      result.push_back(inst_map.at(inst.index));
    artifact.definition.blocks.push_back(std::move(result));
  }
  artifact.definition.entry_block = 0;
  if (!artifact.verify(error))
    return std::nullopt;
  return artifact;
}

} // namespace chtholly::compiler::internal
