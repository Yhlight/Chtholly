#include "SemIRVerificationContext.h"

#include <limits>
#include <ranges>
#include <unordered_set>

namespace chtholly::compiler::internal {

bool SemIRVerificationContext::verifyStructure(std::string &error) const {
  if (!sem_ir_.imports_.verify(error))
    return false;
  if (!sem_ir_.top_block_.hasValue() ||
      sem_ir_.top_block_.index >= sem_ir_.inst_blocks_.size()) {
    error = "semantic IR has no valid top block";
    return false;
  }
  if (!sem_ir_.place_state_query_ ||
      !sem_ir_.place_state_query_->verify(sem_ir_, error)) {
    if (error.empty())
      error = "semantic IR has no place-state query";
    return false;
  }
  if (sem_ir_.locations_.size() != sem_ir_.insts_.size()) {
    error = "instruction and location stores are not aligned";
    return false;
  }
  if (sem_ir_.function_semantic_contracts_.size() != sem_ir_.functions_.size() ||
      sem_ir_.function_ownership_summaries_.size() != sem_ir_.functions_.size() ||
      sem_ir_.expected_function_ownership_summaries_.size() !=
          sem_ir_.functions_.size() ||
      sem_ir_.function_declarations_.size() != sem_ir_.functions_.size() ||
      sem_ir_.function_constraints_.size() != sem_ir_.functions_.size()) {
    error = "function and ownership-summary stores are not aligned";
    return false;
  }
  return true;
}

bool SemIRVerificationContext::verifyEntityTables(std::string &error) const {
  const auto &sem_ir = sem_ir_;
  for (const auto &occurrence : sem_ir.symbol_occurrences_) {
    const auto target_count =
        occurrence.target_kind == SemSymbolTargetKind::Local
            ? sem_ir.locals_.size()
            : occurrence.target_kind == SemSymbolTargetKind::Function
                  ? sem_ir.function_refs_.size()
                  : sem_ir.constants_.size();
    if (!occurrence.location.hasValue() || occurrence.target >= target_count) {
      error = "semantic symbol occurrence has an invalid target";
      return false;
    }
  }
  for (const auto &context : sem_ir.member_access_contexts_) {
    if (!context.location.hasValue() || !context.owner.hasValue() ||
        context.owner.index >= sem_ir.nominal_types_.size() ||
        context.kind > SemMemberAccessKind::Associated) {
      error = "semantic member-access context has an invalid target";
      return false;
    }
  }
  for (const auto &context : sem_ir.module_access_contexts_) {
    if (!context.location.hasValue() || !context.module.hasValue() ||
        context.module.index >= sem_ir.imports_.size()) {
      error = "semantic module-access context has an invalid target";
      return false;
    }
  }
  for (std::size_t index = 0; index < sem_ir.names_.size(); ++index) {
    if (sem_ir.name(NameId(static_cast<std::uint32_t>(index)))
            .text.index >= sem_ir.values_->identifierCount()) {
      error = "semantic name has invalid interned text";
      return false;
    }
  }
  for (std::size_t index = 0; index < sem_ir.locals_.size(); ++index) {
    const auto &value =
        sem_ir.local(LocalId(static_cast<std::uint32_t>(index)));
    if (value.name.index >= sem_ir.names_.size() ||
        value.type.index >= sem_ir.types_.size()) {
      error = "local has an invalid entity record";
      return false;
    }
  }
  for (std::uint32_t index = 0; index < sem_ir.nominal_types_.size(); ++index) {
    const auto &nominal = sem_ir.nominalType(NominalTypeId(index));
    if (!nominal.name.hasValue() || nominal.name.index >= sem_ir.names_.size() ||
        !nominal.declaration.hasValue() || nominal.kind >= NominalKind::Count) {
      error = "nominal definition has an invalid identity";
      return false;
    }
    for (const auto &field : nominal.fields) {
      if (!field.name.hasValue() || field.name.index >= sem_ir.names_.size() ||
          !field.type.hasValue() || field.type.index >= sem_ir.types_.size()) {
        error = "nominal definition has an invalid field";
        return false;
      }
    }
    std::unordered_set<std::uint32_t> method_targets;
    for (const auto &method : nominal.member_functions) {
      if (!method.name.hasValue() || method.name.index >= sem_ir.names_.size() ||
          !method.target.hasValue() ||
          method.target.index >= sem_ir.function_refs_.size() ||
          (method.flags & ~(SemNominalMemberFunctionPublic |
                            SemNominalMemberFunctionAssociated)) != 0 ||
          !method_targets.insert(method.target.index).second ||
          std::ranges::any_of(nominal.fields, [&](const auto &field) {
            return field.name == method.name;
          })) {
        error = "nominal definition has an invalid inherent method";
        return false;
      }
    }
    std::unordered_set<std::uint32_t> variant_names;
    std::unordered_set<std::int64_t> variant_discriminants;
    for (std::size_t variant_index = 0; variant_index < nominal.variants.size();
         ++variant_index) {
      const auto &variant = nominal.variants[variant_index];
      if (!variant.name.hasValue() || variant.name.index >= sem_ir.names_.size() ||
          !variant.declaration.hasValue() ||
          !variant_names.insert(variant.name.index).second) {
        error = "enum definition has an invalid variant";
        return false;
      }
      if ((nominal.is_value_enum &&
           (variant.shape != SemEnumPayloadShape::Unit ||
            !variant.fields.empty() ||
            variant.discriminant < std::numeric_limits<std::int32_t>::min() ||
            variant.discriminant > std::numeric_limits<std::int32_t>::max() ||
            !variant_discriminants.insert(variant.discriminant).second)) ||
          (!nominal.is_value_enum &&
           variant.discriminant != static_cast<std::int64_t>(variant_index))) {
        error = "enum definition has invalid discriminant metadata";
        return false;
      }
      for (const auto &field : variant.fields)
        if (!field.name.hasValue() || field.name.index >= sem_ir.names_.size() ||
            !field.type.hasValue() || field.type.index >= sem_ir.types_.size()) {
          error = "enum definition has an invalid payload field";
          return false;
        }
    }
    if ((nominal.kind == NominalKind::Enum) != !nominal.variants.empty() ||
        (nominal.kind == NominalKind::Enum && !nominal.fields.empty()) ||
        (nominal.kind != NominalKind::Enum && nominal.is_value_enum)) {
      error = "nominal definition '" +
              std::string(sem_ir.identifier(sem_ir.name(nominal.name).text)) +
              "' (" + std::to_string(index) +
              ") has inconsistent enum storage: variants=" +
              std::to_string(nominal.variants.size()) +
              ", fields=" + std::to_string(nominal.fields.size());
      return false;
    }
  }
  const auto is_dependent_type = [&](auto &&self,
                                     CanonicalTypeId type_id) -> bool {
    const auto &type_value = sem_ir.values_->generics().type(type_id);
    if (type_value.kind == CanonicalTypeKind::TypeParameter ||
        type_value.kind == CanonicalTypeKind::TypeProjection)
      return true;
    if (type_value.kind == CanonicalTypeKind::Array &&
        self(self, CanonicalTypeId(type_value.arg0)))
      return true;
    return std::ranges::any_of(
        type_value.elements,
        [&](CanonicalTypeId element) { return self(self, element); });
  };
  for (const auto &query : sem_ir.type_queries_) {
    if (query.kind >= SemTypeQueryArtifact::Kind::Count ||
        !query.source.hasValue() || query.source.index >= sem_ir.types_.size()) {
      error = "semantic type query has an invalid dependent source";
      return false;
    }
    const auto type_same = query.kind == SemTypeQueryArtifact::Kind::TypeSame;
    if (type_same != query.other.hasValue() ||
        (query.other.hasValue() && query.other.index >= sem_ir.types_.size())) {
      error = "semantic type query has an invalid comparison type";
      return false;
    }
    if (!is_dependent_type(is_dependent_type, sem_ir.canonicalType(query.source)) &&
        !(type_same &&
          is_dependent_type(is_dependent_type,
                            sem_ir.canonicalType(query.other)))) {
      error = "semantic type query is not dependent";
      return false;
    }
    if (query.kind == SemTypeQueryArtifact::Kind::TypeIs) {
      if (query.property != "integer" && query.property != "floating" &&
          query.property != "raw_pointer" && query.property != "array" &&
          query.property != "nominal" && query.property != "string" &&
          query.property != "reference") {
        error = "semantic type category query has an invalid property";
        return false;
      }
    } else if (query.kind == SemTypeQueryArtifact::Kind::TypeHas) {
      if (query.property != "copy" && query.property != "move" &&
          query.property != "drop" &&
          query.property != "object_representation") {
        error = "semantic type capability query has an invalid property";
        return false;
      }
    } else if (!query.property.empty()) {
      error = "semantic type query has an unexpected property";
      return false;
    }
  }
  return true;
}

bool SemIRVerificationContext::verifyTypeRecords(std::string &error) const {
  return sem_ir_.verifyTypeRecords(error);
}

bool SemIRVerificationContext::verifyControlFlow(std::string &error) const {
  for (std::uint32_t index = 0; index < sem_ir_.functionCount(); ++index) {
    const auto function = FunctionId(index);
    const auto &value = sem_ir_.function(function);
    const auto &declaration = sem_ir_.functionDeclaration(function);
    const auto has_body =
        declaration.kind == SemCallableDeclarationKind::Definition &&
        (value.flags & SemFunctionEvaluatorArtifact) == 0;
    const auto is_template = (value.flags & SemFunctionTemplate) != 0;
    const auto is_specific =
        (is_template || declaration.kind == SemCallableDeclarationKind::Forward) &&
        value.generic.hasValue() && value.specific.hasValue() &&
        value.specific != sem_ir_.values_->generics().generic(value.generic)
                              .self_specific;
    if (has_body && !is_specific &&
        !sem_ir_.verifyCodeBlock(value.body, true,
                                 TypeId(sem_ir_.type(value.type).arg1),
                                 error)) {
      error = "function `" +
              std::string(sem_ir_.identifier(sem_ir_.name(value.name).text)) +
              "`: " + error;
      return false;
    }
  }
  return true;
}


} // namespace chtholly::compiler::internal
