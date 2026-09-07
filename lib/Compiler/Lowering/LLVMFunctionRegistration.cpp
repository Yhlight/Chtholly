#include "LLVMInternal.h"

#include "chtholly/Compiler/ProgramModel.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

#include <optional>
#include <string>

namespace chtholly::compiler {

bool LLVMFunctionRegistrationService::registerAll(
    LLVMFunctionRegistrationState &state, std::string &error) {
  for (std::uint32_t index = 0; index < state.sem_ir.functionRefCount();
       ++index) {
    const auto reference_id = FunctionRefId(index);
    const auto &reference = state.sem_ir.functionRef(reference_id);
    if (reference.local_function.hasValue() &&
        (state.sem_ir.function(reference.local_function).flags &
         SemFunctionEvaluatorArtifact) != 0 &&
        state.sem_ir.functionDeclaration(reference.local_function).kind !=
            SemCallableDeclarationKind::Forward)
      continue;
    const auto is_local = reference.local_function.hasValue();
    if (is_local && (state.sem_ir.function(reference.local_function).flags &
                     SemFunctionTemplate) != 0)
      continue;
    std::string_view name;
    std::string canonical_name;
    std::string_view package_name = state.package_name;
    IdentifierId module_name_id;
    std::uint32_t flags = SemFunctionPublic;
    bool canonical_semantic_target = false;
    bool foreign_target = false;
    bool forward_declaration = false;
    auto semantic_target_role = CallableSemanticRole::None;
    if (is_local) {
      if (reference.local_function.index >= state.sem_ir.functionCount()) {
        error = "LLVM lowering encountered an invalid local function";
        return false;
      }
      const auto &semantic = state.sem_ir.function(reference.local_function);
      foreign_target =
          state.sem_ir.functionDeclaration(reference.local_function).kind ==
          SemCallableDeclarationKind::Foreign;
      forward_declaration =
          state.sem_ir.functionDeclaration(reference.local_function).kind ==
          SemCallableDeclarationKind::Forward;
      flags = semantic.flags;
      name = state.sem_ir.identifier(state.sem_ir.name(semantic.name).text);
      const auto &contract =
          state.sem_ir.functionSemanticContract(reference.local_function);
      semantic_target_role = contract.role;
      if ((contract.domain == CallableSemanticDomain::Ordinary ||
           contract.domain == CallableSemanticDomain::NominalConstruction) &&
          semantic.semantic_owner.hasValue()) {
        const auto &owner = state.sem_ir.nominalType(semantic.semantic_owner);
        canonical_name =
            std::string(state.sem_ir.identifier(
                state.sem_ir.name(owner.name).text)) +
            "::" + std::string(name);
        name = canonical_name;
      }
      if (contract.domain != CallableSemanticDomain::Ordinary &&
          contract.domain != CallableSemanticDomain::NominalConstruction) {
        canonical_semantic_target = true;
        const auto &owner = state.sem_ir.nominalType(contract.owner);
        const auto projection =
            contract.domain == CallableSemanticDomain::ObjectProjection;
        const auto role =
            contract.role == CallableSemanticRole::Copy
                ? "copy"
            : contract.role == CallableSemanticRole::Drop
                ? "drop"
            : contract.role == CallableSemanticRole::Pack
                ? "pack"
            : contract.role == CallableSemanticRole::Init
                ? "init"
            : contract.role == CallableSemanticRole::ProjectionLoad
                ? "load"
            : contract.role == CallableSemanticRole::ProjectionStore
                ? "store"
            : contract.role == CallableSemanticRole::ProjectionTake
                ? "take"
            : contract.role == CallableSemanticRole::ProjectionInit
                ? "init"
            : contract.role == CallableSemanticRole::ProjectionBorrow
                ? "borrow"
            : contract.role == CallableSemanticRole::ProjectionBorrowMut
                ? "borrow_mut"
            : contract.role == CallableSemanticRole::ObjectInit
                ? "object_init"
            : contract.role == CallableSemanticRole::ObjectCopyInit
                ? "object_copy_init"
            : contract.role == CallableSemanticRole::ObjectMoveInit
                ? "object_move_init"
                : "object_drop";
        std::string projector_name;
        if (projection && contract.projector_field < owner.fields.size())
          projector_name = std::string(state.sem_ir.identifier(
              state.sem_ir
                  .name(owner.fields[contract.projector_field].projector_name)
                  .text));
        const auto prefix =
            contract.domain == CallableSemanticDomain::ValueRepresentation
                ? "$representation$"
            : contract.domain == CallableSemanticDomain::ObjectProjection
                ? "$projection$"
            : contract.domain == CallableSemanticDomain::ObjectShell
                ? "$object$"
                : "$lifecycle$";
        canonical_name =
            std::string(prefix) +
            std::string(state.sem_ir.identifier(
                state.sem_ir.name(owner.name).text)) +
            "$" + (projection ? projector_name + "$" : std::string{}) + role;
        name = canonical_name;
        flags |= SemFunctionPublic;
      }
      module_name_id = state.sem_ir.moduleName();
    } else {
      const auto *function =
          state.sem_ir.importIRs().tryGetEntity(reference.public_entity);
      if (!function) {
        error = "LLVM lowering encountered an invalid imported function";
        return false;
      }
      name = state.sem_ir.identifier(function->name);
      canonical_semantic_target =
          function->semantic_contract.domain !=
              CallableSemanticDomain::Ordinary &&
          function->semantic_contract.domain !=
              CallableSemanticDomain::NominalConstruction;
      semantic_target_role = function->semantic_contract.role;
      foreign_target = function->declaration_kind ==
                       PublicCallableDeclarationKind::Foreign;
      package_name = state.sem_ir.identifier(function->package_name);
      module_name_id = function->module_name;
    }
    if (canonical_semantic_target && reference.specific.hasValue()) {
      const auto &function_type = state.sem_ir.type(reference.local_type);
      const auto parameters =
          state.sem_ir.typeBlock(TypeBlockId(function_type.arg0));
      if (parameters.empty() ||
          state.sem_ir.type(parameters.front()).kind != SemTypeKind::Reference) {
        error = "canonical semantic specific has no owner parameter";
        return false;
      }
      canonical_name =
          std::string(name) + "$specific$" +
          state.concrete_type_suffix(
              state.sem_ir.referencePointee(parameters.front()));
      name = canonical_name;
    }
    const auto &semantic_type = state.sem_ir.type(reference.local_type);
    const auto abi_fingerprint = state.callable_abi_suffix(
        reference, package_name, module_name_id, name,
        canonical_semantic_target);
    std::string foreign_symbol;
    if (foreign_target) {
      if (reference.local_function.hasValue()) {
        const auto external =
            state.sem_ir.functionDeclaration(reference.local_function)
                .external_symbol;
        if (external.hasValue())
          foreign_symbol = state.sem_ir.identifier(external);
      } else if (const auto *entity =
                     state.sem_ir.importIRs().tryGetEntity(
                         reference.public_entity);
                 entity && entity->external_symbol.hasValue()) {
        foreign_symbol = state.sem_ir.identifier(entity->external_symbol);
      }
      if (foreign_symbol.empty()) {
        foreign_symbol = std::string(name);
        for (const auto &mapping : state.runtime_symbol_mappings)
          if (mapping.first == foreign_symbol) {
            foreign_symbol = mapping.second;
            break;
          }
      }
    }
    const auto symbol =
        foreign_target
            ? std::move(foreign_symbol)
            : LLVMObjectIdentityService::mangleFunction(
                  package_name, state.sem_ir.identifier(module_name_id), name,
                  abi_fingerprint);
    llvm::SmallVector<llvm::Type *, 4> parameter_types;
    const auto is_async = semantic_type.kind == SemTypeKind::AsyncFunction;
    const auto result_type =
        is_async ? state.sem_ir.asyncSuccessType(reference.local_type)
                 : TypeId(semantic_type.arg1);
    const auto representation_pack =
        canonical_semantic_target &&
        semantic_target_role == CallableSemanticRole::Pack;
    const auto fallible_constructor =
        !foreign_target &&
                semantic_target_role == CallableSemanticRole::Constructor
            ? state.sem_ir.canonicalResultShape(result_type)
            : std::optional<CanonicalResultShape>{};
    const auto in_place_result =
        !representation_pack &&
        state.low_ir.typeRepresentation(result_type).facts.init_repr ==
            InitReprKind::InPlace;
    llvm::Type *physical_result = nullptr;
    if (foreign_target) {
      const auto layout_id = state.low_ir.foreignAbiLayoutFor(reference_id);
      if (!layout_id.hasValue()) {
        error = "foreign callable has no verified LowIR ABI layout";
        return false;
      }
      const auto &layout = state.low_ir.foreignAbiLayout(layout_id);
      const auto result_indirect = layout.result.kind == ForeignPassKind::Indirect;
      if (result_indirect)
        parameter_types.push_back(llvm::PointerType::getUnqual(state.context));
      for (const auto &parameter : layout.parameters) {
        const auto physical = state.foreign_physical_types(parameter);
        parameter_types.append(physical.begin(), physical.end());
      }
      if (layout.result.kind == ForeignPassKind::Ignore || result_indirect)
        physical_result = llvm::Type::getVoidTy(state.context);
      else {
        const auto physical = state.foreign_physical_types(layout.result);
        physical_result = physical.size() == 1
                              ? physical.front()
                              : static_cast<llvm::Type *>(
                                    llvm::StructType::get(state.context, physical));
      }
    } else {
      if (fallible_constructor) {
        parameter_types.push_back(llvm::PointerType::getUnqual(state.context));
        parameter_types.push_back(llvm::PointerType::getUnqual(state.context));
      } else if (in_place_result && !is_async) {
        parameter_types.push_back(llvm::PointerType::getUnqual(state.context));
      }
      for (const auto parameter :
           state.sem_ir.typeBlock(TypeBlockId(semantic_type.arg0)))
        parameter_types.push_back(state.lower_value_type(parameter));
      physical_result =
          is_async
              ? static_cast<llvm::Type *>(llvm::PointerType::getUnqual(state.context))
              : (in_place_result || fallible_constructor)
                    ? llvm::Type::getVoidTy(state.context)
                    : state.lower_value_type(result_type);
    }
    auto *type = llvm::FunctionType::get(
        physical_result, parameter_types,
        foreign_target &&
            state.low_ir
                .foreignAbiLayout(state.low_ir.foreignAbiLayoutFor(reference_id))
                .is_variadic);
    const auto linkage =
        forward_declaration
            ? llvm::Function::ExternalLinkage
            : is_local && !foreign_target &&
                      (flags & SemFunctionPublic) == 0 &&
                      (flags & SemFunctionCoroutineScaffold) == 0 &&
                      !state.native_definition_exports.contains(
                          reference.local_function.index)
                  ? llvm::Function::InternalLinkage
                  : llvm::Function::ExternalLinkage;
    auto *llvm_function =
        llvm::Function::Create(type, linkage, symbol, state.module);
    if (foreign_target)
      state.apply_foreign_attributes(
          *llvm_function,
          state.low_ir.foreignAbiLayout(
              state.low_ir.foreignAbiLayoutFor(reference_id)));
    state.functions.emplace(reference_id.index, llvm_function);
    if (is_local)
      state.local_function_refs.emplace(reference.local_function.index,
                                        reference_id);
    if (is_local && !is_async && isV1SourceEntryName(name) &&
        state.emission_role == ModuleEmissionRole::ExecutableEntry) {
      state.source_entry = llvm_function;
      ++state.entry_candidate_count;
    }
  }
  return true;
}

} // namespace chtholly::compiler
