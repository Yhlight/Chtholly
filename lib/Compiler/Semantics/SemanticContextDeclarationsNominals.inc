// Included by SemanticContextDeclarations.cpp; this fragment aliases the
// single SemanticContext and preserves declaration-phase ordering.
void SemanticContext::predeclareNominalType(NodeId node) {
  if (tree_.kind(node) != NodeKind::StructDecl &&
      tree_.kind(node) != NodeKind::UnionDecl &&
      tree_.kind(node) != NodeKind::EnumDecl)
    return;
  const auto children = tree_.children(node);
  std::size_t offset = 0;
  while (offset < children.size() &&
         (tree_.kind(children[offset]) == NodeKind::LifecycleAttribute ||
          tree_.kind(children[offset]) == NodeKind::RepresentationAttribute))
    ++offset;
  if (children.size() < offset + 2)
    return;
  const auto name = nameFor(children[offset]);
  const auto generic_nodes = tree_.children(children[offset + 1]);
  GenericId generic;
  if (!generic_nodes.empty())
    generic = values_.generics().addGeneric(
        sem_ir_.checkIRId(), sem_ir_.moduleName(), sem_ir_.name(name).text,
        static_cast<std::uint32_t>(generic_nodes.size()));
  const auto flags = hasNodeFlag(tree_.get(node).flags, NodeFlags::IsPublic)
                         ? SemNominalTypePublic
                         : 0U;
  SemNominalType nominal{name, generic, {}, node, flags};
  nominal.kind = tree_.kind(node) == NodeKind::UnionDecl  ? NominalKind::Union
                 : tree_.kind(node) == NodeKind::EnumDecl ? NominalKind::Enum
                                                          : NominalKind::Struct;
  const auto id = sem_ir_.addNominalTypeDecl(std::move(nominal));
  completion_.registerShell(
      id,
      std::string(values_.identifier(sem_ir_.moduleName())) +
          "::" + std::string(values_.identifier(sem_ir_.name(name).text)),
      node);
  nominal_nodes_.emplace(node.index, id);
  if (!nominal_names_.emplace(sem_ir_.name(name).text.index, id).second)
    emit(DiagnosticKind::DuplicateName, children[offset]);
}

void SemanticContext::completeNominalType(NodeId node) {
  if (tree_.kind(node) != NodeKind::StructDecl &&
      tree_.kind(node) != NodeKind::UnionDecl &&
      tree_.kind(node) != NodeKind::EnumDecl)
    return;
  const auto found = nominal_nodes_.find(node.index);
  if (found == nominal_nodes_.end())
    return;
  const auto result = completion_.requireComplete(
      found->second, node, [&] { return completeNominalTypeDefinition(node); });
  auto completed = sem_ir_.nominalType(found->second);
  completed.completion_state = result.succeeded()
                                   ? SemNominalCompletionState::Complete
                                   : SemNominalCompletionState::Failed;
  sem_ir_.setNominalType(found->second, std::move(completed));
}

[[nodiscard]] bool SemanticContext::completeNominalTypeDefinition(NodeId node) {
  const auto found = nominal_nodes_.find(node.index);
  if (found == nominal_nodes_.end())
    return false;
  const auto children = tree_.children(node);
  std::size_t offset = 0;
  while (offset < children.size() &&
         (tree_.kind(children[offset]) == NodeKind::LifecycleAttribute ||
          tree_.kind(children[offset]) == NodeKind::RepresentationAttribute))
    ++offset;
  auto nominal = sem_ir_.nominalType(found->second);
  current_generic_ = nominal.generic;
  generic_bindings_.clear();
  const auto generic_nodes = tree_.children(children[offset + 1]);
  const auto is_value_enum =
      nominal.kind == NominalKind::Enum &&
      std::ranges::any_of(children, [&](NodeId child) {
        if (tree_.kind(child) != NodeKind::EnumVariant)
          return false;
        return std::ranges::any_of(tree_.children(child), [&](NodeId member) {
          return tree_.kind(member) == NodeKind::EnumDiscriminant;
        });
      });
  nominal.is_value_enum = is_value_enum;
  for (std::uint32_t index = 0; index < generic_nodes.size(); ++index) {
    const auto name = tokenString(generic_nodes[index]);
    if (!name.hasValue() ||
        !generic_bindings_.emplace(name.index, index).second)
      emit(DiagnosticKind::DuplicateName, generic_nodes[index]);
  }
  bool saw_lifecycle = false;
  bool saw_representation = false;
  for (std::size_t index = 0; index < offset; ++index) {
    if (tree_.kind(children[index]) == NodeKind::LifecycleAttribute) {
      if (std::exchange(saw_lifecycle, true))
        emit(DiagnosticKind::InvalidLifecycleAttribute, children[index]);
      else
        checkLifecycleAttribute(children[index], nominal);
    } else {
      if (std::exchange(saw_representation, true))
        emit(DiagnosticKind::InvalidRepresentationAttribute, children[index]);
      else {
        checkRepresentationAttribute(children[index], nominal);
      }
    }
  }
  std::unordered_map<std::uint32_t, NodeId> field_names;
  const auto declaration_member_start =
      offset + 2 < children.size() &&
              tree_.kind(children[offset + 2]) == NodeKind::ConstraintList
          ? offset + 3
          : offset + 2;
  const auto constraint_nodes =
      offset + 2 < children.size() &&
              tree_.kind(children[offset + 2]) == NodeKind::ConstraintList
          ? tree_.children(children[offset + 2])
          : std::span<const NodeId>{};
  if (is_value_enum &&
      (!generic_nodes.empty() || !constraint_nodes.empty() || offset != 0))
    emit(DiagnosticKind::InvalidValueEnumDeclaration, node);
  std::unordered_set<std::int64_t> value_enum_discriminants;
  for (std::size_t index = declaration_member_start; index < children.size();
       ++index) {
    if (tree_.kind(children[index]) == NodeKind::EnumVariant) {
      const auto variant_children = tree_.children(children[index]);
      if (variant_children.empty() ||
          tree_.kind(variant_children.front()) != NodeKind::Name) {
        emit(DiagnosticKind::InvalidEnumVariant, children[index]);
        continue;
      }
      SemEnumVariant variant;
      variant.name = nameFor(variant_children.front());
      variant.declaration = children[index];
      const auto duplicate =
          std::ranges::any_of(nominal.variants, [&](const auto &candidate) {
            return sem_ir_.name(candidate.name).text ==
                   sem_ir_.name(variant.name).text;
          });
      if (duplicate)
        emit(DiagnosticKind::DuplicateName, variant_children.front());
      std::optional<bool> named_payload;
      std::unordered_set<std::uint32_t> payload_names;
      std::optional<NodeId> discriminant_node;
      for (std::size_t payload_index = 1;
           payload_index < variant_children.size(); ++payload_index) {
        const auto payload_node = variant_children[payload_index];
        if (tree_.kind(payload_node) == NodeKind::EnumDiscriminant) {
          if (discriminant_node)
            emit(DiagnosticKind::ValueEnumInvalidDiscriminant, payload_node);
          else
            discriminant_node = payload_node;
          continue;
        }
        if (tree_.kind(payload_node) != NodeKind::EnumPayloadField)
          continue;
        const auto payload_children = tree_.children(payload_node);
        const auto is_named = payload_children.size() == 2;
        if ((!is_named && payload_children.size() != 1) ||
            (named_payload && *named_payload != is_named)) {
          emit(DiagnosticKind::InvalidEnumVariant, payload_node);
          continue;
        }
        named_payload = is_named;
        const auto field_name =
            is_named ? nameFor(payload_children.front())
                     : sem_ir_.addName(values_.internIdentifier(
                           std::to_string(variant.fields.size())));
        if (!payload_names.insert(sem_ir_.name(field_name).text.index).second)
          emit(DiagnosticKind::DuplicateName, payload_node);
        const auto field_type = checkType(payload_children.back());
        if (!hasObjectRepresentation(field_type))
          emit(DiagnosticKind::InvalidObjectType, payload_children.back());
        variant.fields.push_back({field_name, field_type, payload_node, 0U});
      }
      variant.shape = variant.fields.empty() ? SemEnumPayloadShape::Unit
                      : named_payload.value_or(false)
                          ? SemEnumPayloadShape::Struct
                          : SemEnumPayloadShape::Tuple;
      const auto variant_index =
          static_cast<std::uint32_t>(nominal.variants.size());
      variant.discriminant = static_cast<std::int64_t>(variant_index);
      if (is_value_enum) {
        if (!variant.fields.empty())
          emit(DiagnosticKind::InvalidValueEnumDeclaration, children[index]);
        if (!discriminant_node) {
          emit(DiagnosticKind::ValueEnumExplicitDiscriminantRequired,
               children[index]);
        } else {
          const auto discriminant_children = tree_.children(*discriminant_node);
          NodeId literal;
          bool negative = false;
          if (discriminant_children.size() == 1) {
            literal = discriminant_children.front();
            if (tree_.kind(literal) == NodeKind::UnaryExpr &&
                tree_.tokens().get(tree_.token(literal)).kind ==
                    TokenKind::Minus) {
              const auto unary_children = tree_.children(literal);
              if (unary_children.size() == 1) {
                literal = unary_children.front();
                negative = true;
              }
            }
          }
          const auto parsed = literal.hasValue() && tree_.kind(literal) ==
                                                        NodeKind::IntegerLiteral
                                  ? parseIntegerLiteral(tree_.tokens().text(
                                        tree_.token(literal)))
                                  : std::optional<ParsedIntegerLiteral>{};
          const auto magnitude_limit =
              negative ? std::uint64_t{1} << 31U
                       : static_cast<std::uint64_t>(
                             std::numeric_limits<std::int32_t>::max());
          if (!parsed ||
              (parsed->suffix != NumericSuffix::None &&
               parsed->suffix != NumericSuffix::I32) ||
              parsed->magnitude > magnitude_limit) {
            emit(DiagnosticKind::ValueEnumInvalidDiscriminant,
                 *discriminant_node);
          } else {
            variant.discriminant =
                negative ? -static_cast<std::int64_t>(parsed->magnitude)
                         : static_cast<std::int64_t>(parsed->magnitude);
            if (!value_enum_discriminants.insert(variant.discriminant).second)
              emit(DiagnosticKind::ValueEnumDuplicateDiscriminant,
                   *discriminant_node);
          }
        }
      }
      (void)sem_ir_.addInteger(static_cast<std::int64_t>(variant_index));
      for (std::uint32_t field = 0; field < variant.fields.size(); ++field) {
        const auto encoded =
            (static_cast<std::uint64_t>(variant_index) << 32U) | field;
        (void)sem_ir_.addInteger(static_cast<std::int64_t>(encoded));
      }
      nominal.variants.push_back(std::move(variant));
      continue;
    }
    if (tree_.kind(children[index]) != NodeKind::StructField &&
        tree_.kind(children[index]) != NodeKind::UnionField)
      continue;
    const auto field_children = tree_.children(children[index]);
    std::size_t field_offset = 0;
    while (field_offset < field_children.size() &&
           tree_.kind(field_children[field_offset]) ==
               NodeKind::RepresentationAttribute)
      ++field_offset;
    if (field_children.size() != field_offset + 2)
      continue;
    const auto field_name = nameFor(field_children[field_offset]);
    if (!field_names
             .emplace(sem_ir_.name(field_name).text.index,
                      field_children[field_offset])
             .second)
      emit(DiagnosticKind::DuplicateName, field_children[field_offset]);
    const auto field_type = checkType(field_children[field_offset + 1]);
    if (!hasObjectRepresentation(field_type))
      emit(DiagnosticKind::InvalidObjectType, field_children[field_offset + 1]);
    SemNominalField field{
        field_name, field_type, field_children[field_offset],
        hasNodeFlag(tree_.get(children[index]).flags, NodeFlags::IsPublic)
            ? 1U
            : 0U};
    for (std::size_t attribute = 0; attribute < field_offset; ++attribute)
      checkFieldRepresentationAttribute(field_children[attribute], field);
    nominal.fields.push_back(std::move(field));
  }
  if ((nominal.kind == NominalKind::Enum &&
       (saw_lifecycle || saw_representation || nominal.variants.empty())) ||
      (nominal.kind == NominalKind::Union &&
       nominal.representation_policy != NominalRepresentationPolicy::C) ||
      (nominal.kind == NominalKind::Union && saw_lifecycle) ||
      (nominal.representation_policy == NominalRepresentationPolicy::C &&
       (nominal.fields.empty() || nominal.value_repr_pattern.hasValue() ||
        nominal.object_repr_pattern.hasValue())))
    emit(DiagnosticKind::InvalidRepresentationAttribute, node);
  sem_ir_.setNominalType(found->second, std::move(nominal));
  return true;
}

void SemanticContext::validateNominalTypeCycles() {
  std::vector<std::uint8_t> state(sem_ir_.nominalTypeCount());
  const auto visit_type = [&](auto &&self, TypeId type,
                              auto &&visit_nominal) -> bool {
    const auto &value = sem_ir_.type(type);
    if (value.kind == SemTypeKind::Array)
      return self(self, TypeId(value.arg0), visit_nominal);
    return value.kind == SemTypeKind::Nominal
               ? visit_nominal(visit_nominal, NominalTypeId(value.arg0))
               : false;
  };
  const auto visit_nominal = [&](auto &&self, NominalTypeId id) -> bool {
    if (state[id.index] == 1)
      return true;
    if (state[id.index] == 2)
      return false;
    state[id.index] = 1;
    for (const auto &field : sem_ir_.nominalType(id).fields)
      if (visit_type(visit_type, field.type, self))
        return true;
    state[id.index] = 2;
    return false;
  };
  for (std::uint32_t index = 0; index < sem_ir_.nominalTypeCount(); ++index)
    if (visit_nominal(visit_nominal, NominalTypeId(index)))
      emit(DiagnosticKind::RecursiveNominalType,
           sem_ir_.nominalType(NominalTypeId(index)).declaration);
}

[[nodiscard]] bool SemanticContext::isAtomicStorageType(TypeId type) const {
  if (!type.hasValue() || sem_ir_.type(type).kind != SemTypeKind::Nominal)
    return false;
  const auto &nominal =
      sem_ir_.nominalType(NominalTypeId(sem_ir_.type(type).arg0));
  for (const auto &member : nominal.member_functions)
    if (member.target.hasValue() &&
        isAtomicCompilerIntrinsic(sem_ir_.functionIntrinsicRole(member.target)))
      return true;
  if (!nominal.canonical_entity.hasValue())
    return false;
  const auto *entity =
      sem_ir_.importIRs().tryGetEntity(nominal.canonical_entity);
  if (!entity)
    return false;
  const auto &registry = sem_ir_.importIRs().registry();
  for (std::uint32_t index = 0; index < registry.entityCount(); ++index) {
    const auto *candidate = registry.tryGetEntity(PublicEntityId(index));
    if (candidate && candidate->kind == PublicEntityKind::Function &&
        isAtomicCompilerIntrinsic(candidate->intrinsic_role) &&
        candidate->member_owner &&
        candidate->member_owner->kind == PublicEntityKind::NominalType &&
        candidate->member_owner->canonical_package ==
            values_.identifier(entity->package_name) &&
        candidate->member_owner->canonical_module ==
            values_.identifier(entity->module_name) &&
        candidate->member_owner->canonical_name ==
            values_.identifier(entity->name) &&
        candidate->member_owner->expected_fingerprint == entity->fingerprint)
      return true;
  }
  return false;
}

[[nodiscard]] bool SemanticContext::hasCRepresentation(TypeId type) const {
  std::unordered_set<std::uint32_t> active;
  const auto visit_type = [&](auto &&self, TypeId current) -> bool {
    const auto &value = sem_ir_.type(current);
    switch (value.kind) {
    case SemTypeKind::Bool:
    case SemTypeKind::Char:
    case SemTypeKind::Integer:
    case SemTypeKind::Float:
    case SemTypeKind::RawPointer:
    case SemTypeKind::Reference:
    case SemTypeKind::CFunctionPointer:
    case SemTypeKind::CVariadicFunctionPointer:
      return true;
    case SemTypeKind::Array:
      return value.arg1 != 0 && self(self, TypeId(value.arg0));
    case SemTypeKind::Nominal: {
      if (isAtomicStorageType(current))
        return false;
      const auto id = NominalTypeId(value.arg0);
      const auto &nominal = sem_ir_.nominalType(id);
      if (nominal.representation_policy != NominalRepresentationPolicy::C ||
          nominal.generic.hasValue() || nominal.fields.empty() ||
          nominal.value_repr_pattern.hasValue() ||
          nominal.object_repr_pattern.hasValue() ||
          !active.insert(id.index).second)
        return false;
      const auto eligible =
          std::ranges::all_of(nominal.fields, [&](const auto &field) {
            return field.projection_kind ==
                       PublicObjectProjectionKind::StableAddress &&
                   field.storage_path.empty() && self(self, field.type);
          });
      active.erase(id.index);
      return eligible;
    }
    case SemTypeKind::Invalid:
    case SemTypeKind::Void:
    case SemTypeKind::Never:
    case SemTypeKind::String:
    case SemTypeKind::Function:
    case SemTypeKind::AsyncFunction:
    case SemTypeKind::TypeParameter:
    case SemTypeKind::CallbackAdapter:
    case SemTypeKind::Count:
      return false;
    }
    return false;
  };
  return visit_type(visit_type, type);
}

void SemanticContext::validateCRepresentationPolicies() {
  const auto is_trivial_c_object =
      [&](const auto &self, TypeId type,
          std::unordered_set<std::uint32_t> &active) -> bool {
    const auto &value = sem_ir_.type(type);
    switch (value.kind) {
    case SemTypeKind::Bool:
    case SemTypeKind::Char:
    case SemTypeKind::Integer:
    case SemTypeKind::Float:
    case SemTypeKind::RawPointer:
    case SemTypeKind::Reference:
    case SemTypeKind::CFunctionPointer:
    case SemTypeKind::CVariadicFunctionPointer:
      return true;
    case SemTypeKind::Array:
      return value.arg1 != 0 && self(self, TypeId(value.arg0), active);
    case SemTypeKind::Nominal: {
      if (isAtomicStorageType(type))
        return false;
      const auto id = NominalTypeId(value.arg0);
      const auto &nested = sem_ir_.nominalType(id);
      if (nested.representation_policy != NominalRepresentationPolicy::C ||
          nested.generic.hasValue() || nested.fields.empty() ||
          nested.value_repr_pattern.hasValue() ||
          nested.object_repr_pattern.hasValue() ||
          static_cast<SemLifecycleCopyPolicy>(nested.lifecycle_copy) !=
              SemLifecycleCopyPolicy::Default ||
          static_cast<SemLifecycleMovePolicy>(nested.lifecycle_move) !=
              SemLifecycleMovePolicy::Default ||
          static_cast<SemLifecycleDropPolicy>(nested.lifecycle_drop) !=
              SemLifecycleDropPolicy::Default ||
          !active.insert(id.index).second)
        return false;
      const auto trivial =
          std::ranges::all_of(nested.fields, [&](const auto &field) {
            return field.projection_kind ==
                       PublicObjectProjectionKind::StableAddress &&
                   field.storage_path.empty() && self(self, field.type, active);
          });
      active.erase(id.index);
      return trivial;
    }
    case SemTypeKind::Invalid:
    case SemTypeKind::Void:
    case SemTypeKind::Never:
    case SemTypeKind::String:
    case SemTypeKind::Function:
    case SemTypeKind::AsyncFunction:
    case SemTypeKind::TypeParameter:
    case SemTypeKind::CallbackAdapter:
    case SemTypeKind::Count:
      return false;
    }
    return false;
  };
  for (std::uint32_t index = 0; index < sem_ir_.nominalTypeCount(); ++index) {
    const auto id = NominalTypeId(index);
    const auto &nominal = sem_ir_.nominalType(id);
    if (!nominal.declaration.hasValue() ||
        nominal.representation_policy != NominalRepresentationPolicy::C)
      continue;
    const auto eligible =
        !nominal.fields.empty() && !nominal.value_repr_pattern.hasValue() &&
        !nominal.object_repr_pattern.hasValue() &&
        std::ranges::all_of(nominal.fields, [&](const auto &field) {
          if (field.projection_kind !=
                  PublicObjectProjectionKind::StableAddress ||
              !field.storage_path.empty())
            return false;
          if (nominal.generic.hasValue())
            return true;
          if (!hasCRepresentation(field.type))
            return false;
          if (nominal.kind != NominalKind::Union)
            return true;
          std::unordered_set<std::uint32_t> active;
          active.insert(id.index);
          return is_trivial_c_object(is_trivial_c_object, field.type, active);
        });
    if (!eligible)
      emit(DiagnosticKind::InvalidRepresentationAttribute, nominal.declaration);
  }
}

[[nodiscard]] StableFingerprint
