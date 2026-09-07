#include "chtholly/Compiler/CarrierView.h"

#include <algorithm>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace chtholly::compiler {
namespace {

struct Provenance {
  InstId origin;
  std::vector<std::uint32_t> path;

  friend bool operator==(const Provenance &, const Provenance &) = default;
};

using Provenances = std::vector<Provenance>;

void appendUnique(Provenances &target, const Provenances &source) {
  for (const auto &value : source)
    if (std::ranges::find(target, value) == target.end())
      target.push_back(value);
}

bool matchesCarrierPattern(const SemIR &sem_ir, TypeId actual, TypeId pattern,
                           GenericId generic,
                           std::span<const TypeId> arguments) {
  if (actual == pattern)
    return true;
  const auto &actual_type = sem_ir.type(actual);
  const auto &pattern_type = sem_ir.type(pattern);
  if (pattern_type.kind == SemTypeKind::TypeParameter &&
      pattern_type.arg0 == generic.index)
    return pattern_type.arg1 < arguments.size() &&
           sem_ir.canonicalType(actual) ==
               sem_ir.canonicalType(arguments[pattern_type.arg1]);
  if (actual_type.kind != pattern_type.kind)
    return false;
  switch (pattern_type.kind) {
  case SemTypeKind::Void:
  case SemTypeKind::Bool:
  case SemTypeKind::Char:
  case SemTypeKind::Integer:
  case SemTypeKind::Float:
  case SemTypeKind::String:
    return true;
  case SemTypeKind::Array:
    return actual_type.arg1 == pattern_type.arg1 &&
           matchesCarrierPattern(sem_ir, TypeId(actual_type.arg0),
                                 TypeId(pattern_type.arg0), generic, arguments);
  case SemTypeKind::Reference:
  case SemTypeKind::RawPointer:
    return actual_type.arg1 == pattern_type.arg1 &&
           matchesCarrierPattern(sem_ir, TypeId(actual_type.arg0),
                                 TypeId(pattern_type.arg0), generic, arguments);
  case SemTypeKind::CFunctionPointer:
  case SemTypeKind::CVariadicFunctionPointer:
  case SemTypeKind::Function: {
    const auto actual_parameters =
        sem_ir.typeBlock(TypeBlockId(actual_type.arg0));
    const auto pattern_parameters =
        sem_ir.typeBlock(TypeBlockId(pattern_type.arg0));
    if (actual_parameters.size() != pattern_parameters.size())
      return false;
    for (std::size_t index = 0; index < actual_parameters.size(); ++index)
      if (!matchesCarrierPattern(sem_ir, actual_parameters[index],
                                 pattern_parameters[index], generic, arguments))
        return false;
    return matchesCarrierPattern(sem_ir, TypeId(actual_type.arg1),
                                 TypeId(pattern_type.arg1), generic, arguments);
  }
  case SemTypeKind::AsyncFunction: {
    const auto actual_parameters =
        sem_ir.typeBlock(TypeBlockId(actual_type.arg0));
    const auto pattern_parameters =
        sem_ir.typeBlock(TypeBlockId(pattern_type.arg0));
    const auto actual_outcomes =
        sem_ir.typeBlock(TypeBlockId(actual_type.arg1));
    const auto pattern_outcomes =
        sem_ir.typeBlock(TypeBlockId(pattern_type.arg1));
    if (actual_parameters.size() != pattern_parameters.size() ||
        actual_outcomes.size() != pattern_outcomes.size())
      return false;
    for (std::size_t index = 0; index < actual_parameters.size(); ++index)
      if (!matchesCarrierPattern(sem_ir, actual_parameters[index],
                                 pattern_parameters[index], generic, arguments))
        return false;
    for (std::size_t index = 0; index < actual_outcomes.size(); ++index)
      if (!matchesCarrierPattern(sem_ir, actual_outcomes[index],
                                 pattern_outcomes[index], generic, arguments))
        return false;
    return true;
  }
  case SemTypeKind::Nominal: {
    if (actual_type.arg0 != pattern_type.arg0)
      return false;
    const auto actual_arguments =
        sem_ir.typeBlock(TypeBlockId(actual_type.arg1));
    const auto pattern_arguments =
        sem_ir.typeBlock(TypeBlockId(pattern_type.arg1));
    if (actual_arguments.size() != pattern_arguments.size())
      return false;
    for (std::size_t index = 0; index < actual_arguments.size(); ++index)
      if (!matchesCarrierPattern(sem_ir, actual_arguments[index],
                                 pattern_arguments[index], generic, arguments))
        return false;
    return true;
  }
  case SemTypeKind::TypeParameter:
  case SemTypeKind::CoroutineExecutor:
  case SemTypeKind::CoroutineScope:
  case SemTypeKind::CoroutineTask:
  case SemTypeKind::CoroutineTaskOutcome:
  case SemTypeKind::CoroutineTaskCompletion:
  case SemTypeKind::CoroutineTaskCompletionSet:
  case SemTypeKind::CoroutineTaskSelection:
  case SemTypeKind::CoroutineChecked:
  case SemTypeKind::CallbackAdapter:
  case SemTypeKind::CallbackRegistration:
  case SemTypeKind::CallbackCompletion:
  case SemTypeKind::CallbackWake:
  case SemTypeKind::Invalid:
  case SemTypeKind::Never:
  case SemTypeKind::Count:
    return false;
  }
  return false;
}

std::vector<InstId> functionInstructions(const SemIR &sem_ir,
                                         InstBlockId entry) {
  std::vector<InstId> result;
  std::unordered_set<std::uint32_t> blocks;
  const auto visit = [&](auto &&self, InstBlockId block) -> void {
    if (!blocks.insert(block.index).second)
      return;
    for (const auto instruction : sem_ir.instBlock(block)) {
      result.push_back(instruction);
      const auto &inst = sem_ir.inst(instruction);
      if (inst.kind == SemInstKind::If) {
        for (const auto arm_id : sem_ir.instBlock(InstBlockId(inst.arg1))) {
          const auto &arm = sem_ir.inst(arm_id);
          if (arm.kind == SemInstKind::IfArm)
            self(self, InstBlockId(arm.arg0));
        }
      } else if (inst.kind == SemInstKind::While) {
        self(self, InstBlockId(inst.arg0));
        self(self, InstBlockId(inst.arg1));
      } else if (inst.kind == SemInstKind::For) {
        for (const auto clause_id : sem_ir.instBlock(InstBlockId(inst.arg0))) {
          const auto &clause = sem_ir.inst(clause_id);
          if (clause.kind == SemInstKind::ForClause)
            self(self, InstBlockId(clause.arg1));
        }
        self(self, InstBlockId(inst.arg1));
      } else if (inst.kind == SemInstKind::DoWhile) {
        self(self, InstBlockId(inst.arg0));
        self(self, InstBlockId(inst.arg1));
      } else if (inst.kind == SemInstKind::Defer) {
        self(self, InstBlockId(inst.arg0));
      }
    }
  };
  visit(visit, entry);
  return result;
}

bool withinRegion(const SemCallableSemanticContract &contract,
                  const Provenance &provenance) {
  if (contract.whole_carrier || contract.carrier_path.empty())
    return true;
  return contract.carrier_path.size() <= provenance.path.size() &&
         std::equal(contract.carrier_path.begin(), contract.carrier_path.end(),
                    provenance.path.begin());
}

} // namespace

std::vector<CarrierViewViolation> analyzeCarrierViews(const SemIR &sem_ir) {
  std::vector<CarrierViewViolation> violations;
  std::unordered_set<std::uint64_t> emitted;
  const auto emit = [&](InstId instruction, CarrierViewViolationKind kind) {
    const auto key = (static_cast<std::uint64_t>(instruction.index) << 8U) |
                     static_cast<std::uint8_t>(kind);
    if (emitted.insert(key).second)
      violations.push_back({instruction, kind});
  };

  for (std::uint32_t function_index = 0;
       function_index < sem_ir.functionCount(); ++function_index) {
    const auto &function = sem_ir.function(FunctionId(function_index));
    const auto &semantic_contract =
        sem_ir.functionSemanticContract(FunctionId(function_index));
    const auto role = semantic_contract.role;
    const auto instructions = functionInstructions(sem_ir, function.body);
    std::unordered_set<std::uint32_t> valid_views;

    const auto parameters = sem_ir.localBlock(function.parameters);
    for (const auto instruction : instructions) {
      const auto &view = sem_ir.inst(instruction);
      if (view.kind != SemInstKind::CarrierView)
        continue;
      const auto allowed =
          (role >= SemCanonicalFunctionRole::ProjectionLoad &&
           role <= SemCanonicalFunctionRole::ProjectionBorrowMut) ||
          (role >= SemCanonicalFunctionRole::ObjectInit &&
           role <= SemCanonicalFunctionRole::ObjectDrop);
      const auto &source = sem_ir.inst(InstId(view.arg0));
      const auto input_type = TypeId(source.type);
      const auto output_type = TypeId(view.type);
      if (!allowed || !semantic_contract.owner.hasValue() ||
          parameters.empty() || source.kind != SemInstKind::NameRef ||
          LocalId(source.arg0) != parameters.front() ||
          sem_ir.type(input_type).kind != SemTypeKind::Reference ||
          sem_ir.type(output_type).kind != SemTypeKind::Reference ||
          sem_ir.referenceMutability(input_type) !=
              sem_ir.referenceMutability(output_type)) {
        emit(instruction, CarrierViewViolationKind::Invalid);
        continue;
      }
      const auto owner_type = sem_ir.referencePointee(input_type);
      if (sem_ir.type(owner_type).kind != SemTypeKind::Nominal ||
          NominalTypeId(sem_ir.type(owner_type).arg0) !=
              semantic_contract.owner) {
        emit(instruction, CarrierViewViolationKind::Invalid);
        continue;
      }
      const auto &owner = sem_ir.nominalType(semantic_contract.owner);
      auto carrier_type = sem_ir.objectRepresentationType(owner_type);
      const auto output_carrier = sem_ir.referencePointee(output_type);
      bool carrier_matches = carrier_type.hasValue() &&
                             carrier_type != owner_type &&
                             output_carrier == carrier_type;
      if (carrier_type == owner_type && owner.object_repr_pattern.hasValue()) {
        const auto arguments =
            sem_ir.typeBlock(TypeBlockId(sem_ir.type(owner_type).arg1));
        carrier_matches = owner.generic.hasValue()
                              ? matchesCarrierPattern(sem_ir, output_carrier,
                                                      owner.object_repr_pattern,
                                                      owner.generic, arguments)
                              : output_carrier == owner.object_repr_pattern;
      }
      if (!carrier_matches) {
        emit(instruction, CarrierViewViolationKind::Invalid);
        continue;
      }
      const auto field_count = static_cast<std::uint32_t>(owner.fields.size());
      const auto contract =
          static_cast<std::uint32_t>(sem_ir.integer(IntegerId(view.arg1)));
      const auto projection =
          role >= SemCanonicalFunctionRole::ProjectionLoad &&
          role <= SemCanonicalFunctionRole::ProjectionBorrowMut;
      if ((projection && (contract >= field_count ||
                          contract != semantic_contract.projector_field)) ||
          (!projection && contract != field_count)) {
        emit(instruction, CarrierViewViolationKind::Invalid);
        continue;
      }
      valid_views.insert(instruction.index);
    }

    std::unordered_map<std::uint32_t, Provenances> values;
    std::unordered_map<std::uint32_t, Provenances> locals;
    bool changed = true;
    for (std::size_t iteration = 0;
         changed && iteration <= instructions.size() + sem_ir.localCount();
         ++iteration) {
      changed = false;
      for (const auto instruction : instructions) {
        const auto &inst = sem_ir.inst(instruction);
        Provenances next;
        if (inst.kind == SemInstKind::CarrierView &&
            valid_views.contains(instruction.index)) {
          next.push_back({instruction, {}});
        } else if (inst.kind == SemInstKind::NameRef) {
          appendUnique(next, locals[inst.arg0]);
        } else if (inst.kind == SemInstKind::Dereference ||
                   inst.kind == SemInstKind::BorrowPlace ||
                   inst.kind == SemInstKind::Move ||
                   inst.kind == SemInstKind::Copy) {
          appendUnique(next, values[inst.arg0]);
        } else if (inst.kind == SemInstKind::StructFieldAccess) {
          next = values[inst.arg0];
          const auto field =
              static_cast<std::uint32_t>(sem_ir.integer(IntegerId(inst.arg1)));
          for (auto &provenance : next)
            provenance.path.push_back(field);
        } else if (inst.kind == SemInstKind::Index) {
          appendUnique(next, values[inst.arg0]);
        }
        const auto before = values[instruction.index].size();
        appendUnique(values[instruction.index], next);
        changed |= before != values[instruction.index].size();

        if (inst.kind == SemInstKind::BindName) {
          const auto local_before = locals[inst.arg0].size();
          appendUnique(locals[inst.arg0], values[inst.arg1]);
          changed |= local_before != locals[inst.arg0].size();
        } else if (inst.kind == SemInstKind::Assign) {
          const auto &target = sem_ir.inst(InstId(inst.arg0));
          if (target.kind == SemInstKind::NameRef &&
              sem_ir.type(sem_ir.local(LocalId(target.arg0)).type).kind ==
                  SemTypeKind::Reference) {
            const auto local_before = locals[target.arg0].size();
            appendUnique(locals[target.arg0], values[inst.arg1]);
            changed |= local_before != locals[target.arg0].size();
          }
        }
      }
    }

    const auto check_region = [&](InstId sink, const Provenances &source) {
      for (const auto &provenance : source)
        if (!withinRegion(semantic_contract, provenance))
          emit(sink, CarrierViewViolationKind::Region);
    };
    for (const auto instruction : instructions) {
      const auto &inst = sem_ir.inst(instruction);
      if (inst.kind == SemInstKind::Call) {
        for (const auto argument : sem_ir.instBlock(InstBlockId(inst.arg1)))
          if (!values[argument.index].empty())
            emit(instruction, CarrierViewViolationKind::Escape);
      } else if (inst.kind == SemInstKind::AggregateInit ||
                 inst.kind == SemInstKind::ArrayLiteral) {
        for (const auto value : sem_ir.instBlock(InstBlockId(inst.arg0)))
          if (!values[value.index].empty())
            emit(instruction, CarrierViewViolationKind::Escape);
      } else if (inst.kind == SemInstKind::Return) {
        const auto &source = values[inst.arg0];
        if (source.empty())
          continue;
        const auto source_type = TypeId(sem_ir.inst(InstId(inst.arg0)).type);
        const auto borrowed_result =
            sem_ir.type(source_type).kind == SemTypeKind::Reference;
        const auto borrow_role =
            role == SemCanonicalFunctionRole::ProjectionBorrow ||
            role == SemCanonicalFunctionRole::ProjectionBorrowMut;
        if ((borrowed_result && !borrow_role) ||
            std::ranges::any_of(
                source, [](const auto &value) { return value.path.empty(); }))
          emit(instruction, CarrierViewViolationKind::Escape);
        check_region(instruction, source);
      } else if (inst.kind == SemInstKind::Move ||
                 inst.kind == SemInstKind::Copy) {
        const auto &source = values[inst.arg0];
        if (std::ranges::any_of(
                source, [](const auto &value) { return value.path.empty(); }))
          emit(instruction, CarrierViewViolationKind::Escape);
        check_region(instruction, source);
      } else if (inst.kind == SemInstKind::Assign) {
        check_region(instruction, values[inst.arg0]);
        const auto &target = sem_ir.inst(InstId(inst.arg0));
        const auto aliases_reference =
            target.kind == SemInstKind::NameRef &&
            sem_ir.type(sem_ir.local(LocalId(target.arg0)).type).kind ==
                SemTypeKind::Reference;
        if (!aliases_reference && !values[inst.arg1].empty())
          check_region(instruction, values[inst.arg1]);
      } else if (inst.kind == SemInstKind::BorrowPlace) {
        check_region(instruction, values[inst.arg0]);
      } else if (inst.kind == SemInstKind::Index ||
                 inst.kind == SemInstKind::Add ||
                 inst.kind == SemInstKind::Equal) {
        check_region(instruction, values[inst.arg0]);
        check_region(instruction, values[inst.arg1]);
      }
    }
  }
  return violations;
}

bool verifyCarrierViews(const SemIR &sem_ir, std::string &error) {
  const auto violations = analyzeCarrierViews(sem_ir);
  if (violations.empty())
    return true;
  error = violations.front().kind == CarrierViewViolationKind::Invalid
              ? "semantic carrier view has an invalid canonical contract"
          : violations.front().kind == CarrierViewViolationKind::Escape
              ? "semantic carrier view escapes its canonical function"
              : "semantic carrier access exceeds its projector region";
  return false;
}

} // namespace chtholly::compiler
