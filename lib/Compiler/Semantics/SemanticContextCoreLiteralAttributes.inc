// Included by SemanticContextCore.cpp; this fragment aliases the single
// SemanticContext state and preserves semantic phase ordering.
SemanticContext::substituteType(TypeId type, GenericId generic,
                                std::span<const CanonicalTypeId> arguments) {
  const auto canonical =
      substituteCanonicalType(sem_ir_.canonicalType(type), generic, arguments);
  return canonical.hasValue() ? sem_ir_.materializeType(canonical)
                              : TypeId::invalid();
}

[[nodiscard]] bool
SemanticContext::isCallbackTransportType(TypeId type, bool allow_void) const {
  const auto kind = sem_ir_.type(type).kind;
  return (allow_void && kind == SemTypeKind::Void) ||
         kind == SemTypeKind::Bool || kind == SemTypeKind::Integer ||
         kind == SemTypeKind::Float || kind == SemTypeKind::Reference ||
         kind == SemTypeKind::RawPointer ||
         kind == SemTypeKind::CFunctionPointer ||
         kind == SemTypeKind::CVariadicFunctionPointer ||
         (kind == SemTypeKind::Nominal && hasCRepresentation(type));
}

[[nodiscard]] bool SemanticContext::contextualizeLiteral(InstId instruction,
                                                         TypeId target) {
  const auto &inst = sem_ir_.inst(instruction);
  const auto &target_type = sem_ir_.type(target);
  if (inst.kind == SemInstKind::MakeCallbackAdapter &&
      target_type.kind == SemTypeKind::CallbackAdapter) {
    const auto values = sem_ir_.instBlock(InstBlockId(inst.arg0));
    const auto fields = sem_ir_.typeBlock(TypeBlockId(target_type.arg0));
    if (values.size() != 3 || fields.size() != 3)
      return false;
    for (std::size_t index = 0; index < values.size(); ++index)
      if (!contextualizeLiteral(values[index], fields[index]))
        return false;
    sem_ir_.setInstType(instruction, target);
    return true;
  }
  if (inst.kind == SemInstKind::MakeCallbackRegistration &&
      target_type.kind == SemTypeKind::CallbackRegistration) {
    const auto values = sem_ir_.instBlock(InstBlockId(inst.arg0));
    const auto fields = sem_ir_.typeBlock(TypeBlockId(target_type.arg0));
    const auto bindings = sem_ir_.callbackRegistrationBindings(target);
    const auto fixed_fields = fields.size() == 5 ? 4U : fields.size() - 1U;
    if ((fields.size() != 5 && fields.size() != 7 && fields.size() != 8 &&
         fields.size() != 10) ||
        values.size() != fixed_fields + bindings.size()) {
      emit(DiagnosticKind::InvalidCallbackRegistrationBinding,
           sem_ir_.location(instruction));
      return false;
    }
    constexpr std::array<std::size_t, 9> field_indices{0, 2, 3, 4, 5,
                                                       6, 7, 8, 9};
    for (std::size_t index = 0; index < fixed_fields; ++index)
      if (!contextualizeLiteral(values[index], fields[field_indices[index]]))
        return false;
    const auto register_parameters =
        sem_ir_.typeBlock(TypeBlockId(sem_ir_.type(fields[2]).arg0));
    std::vector<bool> seen(bindings.size());
    for (const auto binding_inst_id : values.subspan(fixed_fields)) {
      const auto &binding_inst = sem_ir_.inst(binding_inst_id);
      if (binding_inst.kind != SemInstKind::CallbackRegistrationBinding) {
        emit(DiagnosticKind::InvalidCallbackRegistrationBinding,
             sem_ir_.location(binding_inst_id));
        return false;
      }
      const auto label =
          values_.identifier(sem_ir_.name(NameId(binding_inst.arg0)).text);
      const auto found = std::ranges::find(bindings, label,
                                           &CallbackRegistrationBinding::name);
      if (found == bindings.end()) {
        emit(DiagnosticKind::InvalidCallbackRegistrationBinding,
             sem_ir_.location(binding_inst_id));
        return false;
      }
      const auto binding_index =
          static_cast<std::size_t>(found - bindings.begin());
      if (seen[binding_index] ||
          found->parameter_index >= register_parameters.size() ||
          !contextualizeLiteral(InstId(binding_inst.arg1),
                                register_parameters[found->parameter_index])) {
        emit(DiagnosticKind::InvalidCallbackRegistrationBinding,
             sem_ir_.location(binding_inst_id));
        return false;
      }
      seen[binding_index] = true;
      sem_ir_.setInstType(binding_inst_id,
                          register_parameters[found->parameter_index]);
    }
    if (std::ranges::any_of(seen, [](bool value) { return !value; })) {
      emit(DiagnosticKind::InvalidCallbackRegistrationBinding,
           sem_ir_.location(instruction));
      return false;
    }
    sem_ir_.setInstType(instruction, target);
    return true;
  }
  if (inst.kind == SemInstKind::NullPointer &&
      target_type.kind == SemTypeKind::RawPointer) {
    sem_ir_.setInstType(instruction, target);
    return true;
  }
  if (inst.kind == SemInstKind::IntegerLiteral &&
      target_type.kind == SemTypeKind::Integer) {
    const auto *literal = conversions_.integerLiteral(instruction);
    if (!literal ||
        !integerMagnitudeFits(literal->magnitude, target, literal->negative))
      return false;
    sem_ir_.setInstType(instruction, target);
    return true;
  }
  if (inst.kind == SemInstKind::StringLiteral &&
      sem_ir_.type(TypeId(inst.type)).kind == SemTypeKind::RawPointer &&
      target_type.kind == SemTypeKind::RawPointer &&
      sem_ir_.rawPointerPointeeConst(target)) {
    const auto &pointee = sem_ir_.type(sem_ir_.rawPointerPointee(target));
    if (pointee.kind == SemTypeKind::Integer && pointee.arg0 == 8 &&
        pointee.arg1 != 0) {
      sem_ir_.setInstType(instruction, target);
      return true;
    }
  }
  return instType(instruction) == target;
}

void SemanticContext::checkLifecycleAttribute(NodeId attribute,
                                              SemNominalType &nominal) {
  const auto children = tree_.children(attribute);
  if (children.empty() ||
      values_.identifier(tokenString(children.front())) != "lifecycle") {
    emit(DiagnosticKind::InvalidLifecycleAttribute, attribute);
    return;
  }
  bool saw_copy = false;
  bool saw_move = false;
  bool saw_drop = false;
  for (std::size_t index = 1; index < children.size(); ++index) {
    const auto entry = children[index];
    const auto entry_children = tree_.children(entry);
    if (tree_.kind(entry) != NodeKind::LifecyclePolicyEntry ||
        entry_children.size() != 1) {
      emit(DiagnosticKind::InvalidLifecycleAttribute, entry);
      continue;
    }
    std::string_view key;
    const auto key_kind = tree_.tokens().get(tree_.token(entry)).kind;
    if (key_kind == TokenKind::KwCopy)
      key = "copy";
    else if (key_kind == TokenKind::KwMove)
      key = "move";
    else
      key = values_.identifier(tokenString(entry));
    const auto value = values_.identifier(tokenString(entry_children[0]));
    bool valid = true;
    if (key == "copy" && !std::exchange(saw_copy, true)) {
      if (value == "default")
        nominal.lifecycle_copy =
            static_cast<std::uint8_t>(SemLifecycleCopyPolicy::Default);
      else if (value == "delete")
        nominal.lifecycle_copy =
            static_cast<std::uint8_t>(SemLifecycleCopyPolicy::Delete);
      else if (value == "custom")
        nominal.lifecycle_copy =
            static_cast<std::uint8_t>(SemLifecycleCopyPolicy::Custom);
      else
        valid = false;
    } else if (key == "move" && !std::exchange(saw_move, true)) {
      if (value == "default")
        nominal.lifecycle_move =
            static_cast<std::uint8_t>(SemLifecycleMovePolicy::Default);
      else if (value == "delete")
        nominal.lifecycle_move =
            static_cast<std::uint8_t>(SemLifecycleMovePolicy::Delete);
      else
        valid = false;
    } else if (key == "drop" && !std::exchange(saw_drop, true)) {
      if (value == "default")
        nominal.lifecycle_drop =
            static_cast<std::uint8_t>(SemLifecycleDropPolicy::Default);
      else if (value == "custom")
        nominal.lifecycle_drop =
            static_cast<std::uint8_t>(SemLifecycleDropPolicy::Custom);
      else
        valid = false;
    } else {
      valid = false;
    }
    if (!valid)
      emit(DiagnosticKind::InvalidLifecycleAttribute, entry);
  }
  if (!saw_copy || !saw_drop)
    emit(DiagnosticKind::InvalidLifecycleAttribute, attribute);
}

void SemanticContext::checkRepresentationAttribute(NodeId attribute,
                                                   SemNominalType &nominal) {
  const auto children = tree_.children(attribute);
  if (children.size() < 2 || children.size() > 3 ||
      values_.identifier(tokenString(children.front())) != "repr") {
    emit(DiagnosticKind::InvalidRepresentationAttribute, attribute);
    return;
  }
  for (std::size_t entry_index = 1; entry_index < children.size();
       ++entry_index) {
    const auto entry = children[entry_index];
    const auto entry_children = tree_.children(entry);
    const auto key = values_.identifier(tokenString(entry));
    if (key == "C" && entry_children.empty() &&
        nominal.representation_policy == NominalRepresentationPolicy::Opaque &&
        children.size() == 2) {
      nominal.representation_policy = NominalRepresentationPolicy::C;
      continue;
    }
    TypeId *pattern = nullptr;
    if (key == "value")
      pattern = &nominal.value_repr_pattern;
    else if (key == "object")
      pattern = &nominal.object_repr_pattern;
    if (tree_.kind(entry) != NodeKind::RepresentationEntry ||
        entry_children.size() != 1 || !pattern || pattern->hasValue()) {
      emit(DiagnosticKind::InvalidRepresentationAttribute, entry);
      continue;
    }
    *pattern = checkType(entry_children.front());
    if (!pattern->hasValue())
      emit(DiagnosticKind::InvalidRepresentationAttribute, entry);
  }
}

void SemanticContext::checkFieldRepresentationAttribute(
    NodeId attribute, SemNominalField &field) {
  const auto children = tree_.children(attribute);
  if (children.size() < 2 ||
      values_.identifier(tokenString(children.front())) != "repr") {
    emit(DiagnosticKind::InvalidRepresentationAttribute, attribute);
    return;
  }
  bool saw_projection = false;
  bool saw_region = false;
  for (std::size_t index = 1; index < children.size(); ++index) {
    const auto entry = children[index];
    const auto entry_children = tree_.children(entry);
    const auto key = values_.identifier(tokenString(entry));
    if (tree_.kind(entry) != NodeKind::RepresentationEntry ||
        entry_children.size() != 1) {
      emit(DiagnosticKind::InvalidRepresentationAttribute, entry);
      continue;
    }
    if (key == "region" && !std::exchange(saw_region, true) &&
        tree_.kind(entry_children.front()) == NodeKind::RepresentationPath) {
      for (const auto component : tree_.children(entry_children.front()))
        field.projection_region_path.push_back(nameFor(component));
      continue;
    }
    if (std::exchange(saw_projection, true)) {
      emit(DiagnosticKind::InvalidRepresentationAttribute, entry);
      continue;
    }
    if (key == "storage" &&
        tree_.kind(entry_children.front()) == NodeKind::RepresentationPath) {
      field.projection_kind = PublicObjectProjectionKind::StableAddress;
      for (const auto component : tree_.children(entry_children.front()))
        field.storage_path.push_back(nameFor(component));
    } else if (key == "projector" &&
               tree_.kind(entry_children.front()) == NodeKind::TypeName) {
      field.projection_kind = PublicObjectProjectionKind::Computed;
      field.projector_name = nameFor(entry_children.front());
    } else if (key == "bits" && tree_.kind(entry_children.front()) ==
                                    NodeKind::RepresentationBitRange) {
      const auto range = tree_.children(entry_children.front());
      if (range.size() != 3 ||
          tree_.kind(range.front()) != NodeKind::RepresentationPath) {
        emit(DiagnosticKind::InvalidRepresentationAttribute, entry);
        continue;
      }
      field.projection_kind = PublicObjectProjectionKind::BitPacked;
      for (const auto component : tree_.children(range.front()))
        field.storage_path.push_back(nameFor(component));
      const auto parse_bound = [&](NodeId node, std::uint32_t &value) {
        const auto text = tree_.tokens().text(tree_.token(node));
        const auto [end, ec] =
            std::from_chars(text.data(), text.data() + text.size(), value);
        return ec == std::errc{} && end == text.data() + text.size();
      };
      if (!parse_bound(range[1], field.bit_begin) ||
          !parse_bound(range[2], field.bit_end) ||
          field.bit_begin >= field.bit_end)
        emit(DiagnosticKind::InvalidRepresentationAttribute, entry);
    } else {
      emit(DiagnosticKind::InvalidRepresentationAttribute, entry);
    }
  }
  if (!saw_projection)
    emit(DiagnosticKind::InvalidRepresentationAttribute, attribute);
}

[[nodiscard]] std::string SemanticContext::typePath(NodeId node) const {
  std::string result;
  const auto append = [&](NodeId component) {
    const auto id = tokenString(component);
    if (!id.hasValue())
      return;
    if (!result.empty())
      result += "::";
    result += values_.identifier(id);
  };
  if (tree_.kind(node) == NodeKind::QualifiedType) {
    for (const auto component : tree_.children(node))
      append(component);
  } else {
    append(node);
  }
  return result;
}

[[nodiscard]] bool
SemanticContext::isCanonicalReference(TypeId type, NominalTypeId owner,
                                      SemReferenceMutability mutability) {
  const auto &reference = sem_ir_.type(type);
  if (reference.kind != SemTypeKind::Reference ||
      sem_ir_.referenceMutability(type) != mutability)
    return false;
  const auto &pointee = sem_ir_.type(TypeId(reference.arg0));
  return pointee.kind == SemTypeKind::Nominal &&
         NominalTypeId(pointee.arg0) == owner;
}

[[nodiscard]] TypeId
SemanticContext::nominalFieldType(TypeId nominal_type,
                                  const SemNominalField &field) {
  const auto &type = sem_ir_.type(nominal_type);
  const auto &definition = sem_ir_.nominalType(NominalTypeId(type.arg0));
  if (!definition.generic.hasValue())
    return field.type;
  std::vector<CanonicalTypeId> arguments;
  for (const auto argument : sem_ir_.typeBlock(TypeBlockId(type.arg1)))
    arguments.push_back(sem_ir_.canonicalType(argument));
  return substituteType(field.type, definition.generic, arguments);
}

[[nodiscard]] NominalTypeId
SemanticContext::resolveNominalComponents(std::span<const NodeId> components,
                                          NodeId location) {
  if (components.empty())
    return NominalTypeId::invalid();
  if (components.size() == 1) {
    const auto name = tokenString(components.front());
    const auto found = name.hasValue() ? nominal_names_.find(name.index)
                                       : nominal_names_.end();
    return found == nominal_names_.end() ? NominalTypeId::invalid()
                                         : found->second;
  }
  std::string module_name;
  for (std::size_t index = 0; index + 1 < components.size(); ++index) {
    if (!module_name.empty())
      module_name += "::";
    module_name += values_.identifier(tokenString(components[index]));
  }
  const auto import =
      sem_ir_.importIRs().findByModule(values_.internIdentifier(module_name));
  const auto *interface = sem_ir_.importIRs().tryGetInterface(import);
  if (!interface)
    return NominalTypeId::invalid();
  const auto binding =
      interface->findNominalType(tokenString(components.back()));
  if (!binding.hasValue())
    return NominalTypeId::invalid();
  return materializeImportedNominal(
      interface->nominalType(binding).canonical_entity, location);
}

[[nodiscard]] TypeId
SemanticContext::resolveNominalTypeComponents(std::span<const NodeId> components,
                                            NodeId location) {
  if (components.size() == 1) {
    const auto name = tokenString(components.front());
    const auto found = type_alias_names_.find(name.index);
    if (found != type_alias_names_.end()) {
      const auto &alias = sem_ir_.typeAlias(found->second);
      if (!alias.generic.hasValue() && alias.state == SemTypeAliasState::Complete &&
          sem_ir_.type(alias.target).kind == SemTypeKind::Nominal)
        return alias.target;
      return TypeId::invalid();
    }
  }
  const auto nominal = resolveNominalComponents(components, location);
  return nominal.hasValue() && !sem_ir_.nominalType(nominal).generic.hasValue()
      ? sem_ir_.addNominalType(nominal) : TypeId::invalid();
}

[[nodiscard]] std::optional<SemanticContext::ResolvedEnumVariant>
SemanticContext::resolveQualifiedEnumVariant(NodeId path) {
  const auto components = tree_.children(path);
  if (components.size() < 2)
    return std::nullopt;
  const auto owner_type = resolveNominalTypeComponents(
      std::span<const NodeId>(components).first(components.size() - 1), path);
  if (!owner_type.hasValue())
    return std::nullopt;
  const auto nominal_id = NominalTypeId(sem_ir_.type(owner_type).arg0);
  const auto &nominal = sem_ir_.nominalType(nominal_id);
  if (nominal.kind != NominalKind::Enum)
    return std::nullopt;
  const auto variant_name = tokenString(components.back());
  for (std::uint32_t index = 0; index < nominal.variants.size(); ++index)
    if (sem_ir_.name(nominal.variants[index].name).text == variant_name)
      return ResolvedEnumVariant{owner_type, index,
                                 components.back()};
  return std::nullopt;
}

[[nodiscard]] std::optional<SemanticContext::ResolvedEnumVariant>
SemanticContext::resolveEnumLiteral(NodeId node) {
  const auto children = tree_.children(node);
  if (children.empty())
    return std::nullopt;
  if (tree_.kind(children.front()) == NodeKind::QualifiedName ||
      tree_.kind(children.front()) == NodeKind::QualifiedType)
    return resolveQualifiedEnumVariant(children.front());
  if (tree_.kind(children.front()) != NodeKind::GenericType ||
      children.size() < 2 || tree_.kind(children[1]) != NodeKind::Name)
    return std::nullopt;
  const auto type = checkType(children.front());
  const auto &semantic_type = sem_ir_.type(type);
  if (semantic_type.kind != SemTypeKind::Nominal)
    return std::nullopt;
  const auto &nominal = sem_ir_.nominalType(NominalTypeId(semantic_type.arg0));
  if (nominal.kind != NominalKind::Enum)
    return std::nullopt;
  const auto variant_name = tokenString(children[1]);
  for (std::uint32_t index = 0; index < nominal.variants.size(); ++index)
    if (sem_ir_.name(nominal.variants[index].name).text == variant_name)
      return ResolvedEnumVariant{type, index, children[1]};
  return std::nullopt;
}

[[nodiscard]] TypeId
SemanticContext::foreignResourceTargetPointer(FunctionRefId target) {
  const auto &reference = sem_ir_.functionRef(target);
  const auto &source = sem_ir_.type(reference.local_type);
  if (source.kind != SemTypeKind::Function)
    return TypeId::invalid();
  const auto physical = [&](TypeId type) {
    const auto representation = sem_ir_.foreignRepresentationType(type);
    return representation.hasValue() ? representation : type;
  };
  std::vector<TypeId> parameters;
  for (const auto parameter : sem_ir_.typeBlock(TypeBlockId(source.arg0)))
    parameters.push_back(physical(parameter));
  CallableOwnershipSummary ownership;
  bool variadic = false;
  if (reference.local_function.hasValue()) {
    const auto &declaration =
        sem_ir_.functionDeclaration(reference.local_function);
    ownership =
        declaration.declared_contract
            ? effectiveCallableOwnershipSummary(*declaration.declared_contract)
            : sem_ir_.functionOwnership(reference.local_function);
    variadic = declaration.foreign_signature &&
               declaration.foreign_signature->is_variadic;
  } else if (const auto *entity =
                 sem_ir_.importIRs().tryGetEntity(reference.public_entity)) {
    ownership = entity->ownership_summary;
    variadic =
        entity->foreign_signature && entity->foreign_signature->is_variadic;
  }
  const auto source_result = TypeId(source.arg1);
  const auto physical_result = physical(source_result);
  if (physical_result != source_result)
    ownership.returns_owned = true;
  return sem_ir_.addCFunctionPointerType(parameters, physical_result, variadic,
                                         std::move(ownership));
}

