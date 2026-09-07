// Included by SemIRMaterialization.cpp; this fragment shares SemIR stores
// and materialization callbacks without copying graph state.
bool SemIR::materializeTemplateBody(
    FunctionId function, const GenericTemplateArtifact &generic_template,
    GenericId generic, NameId function_name, TypeId function_type,
    NodeId template_location,
    const std::function<TypeId(const PublicType &)> &materialize_type,
    const std::function<FunctionRefId(PublicEntityId, std::span<const InstId>,
                                      std::span<const PublicType>, NodeId)>
        &resolve_callee,
    std::string &error) {
  if (!generic_template.verify(error))
    return false;
  const auto map_type = [&](const PublicType &type_value) {
    const auto result = materialize_type(type_value);
    if (!result.hasValue() && error.empty())
      error = "cannot materialize a generic template type";
    return result;
  };
  const auto &region = generic_template.definition;
  std::vector<bool> temporary_locals(generic_template.local_types.size());
  for (const auto &instruction : region.instructions)
    if (instruction.opcode == GenericTemplateOpcode::MaterializeTemporary &&
        instruction.arg0 < temporary_locals.size())
      temporary_locals[instruction.arg0] = true;
  std::vector<LocalId> locals;
  locals.reserve(generic_template.local_types.size());
  for (std::uint32_t index = 0; index < generic_template.local_types.size();
       ++index) {
    const auto generated =
        values_->internIdentifier(index < generic_template.parameter_count
                                      ? "$parameter" + std::to_string(index)
                                      : "$local" + std::to_string(index));
    locals.push_back(addLocal(
        {addName(generated), map_type(generic_template.local_types[index]),
         template_location,
         generic_template.local_flags[index] |
             (temporary_locals[index] ? SemLocalTemporary : 0U)}));
  }
  std::vector<InstId> instructions(region.instructions.size());
  std::vector<SemInst> targets(region.instructions.size());
  std::vector<std::vector<InstId>> value_blocks(region.instructions.size());
  for (std::uint32_t index = 0; index < instructions.size(); ++index) {
    targets[index].type = map_type(region.instructions[index].type).index;
    if (!TypeId(targets[index].type).hasValue())
      return false;
    instructions[index] = addRawInst(targets[index], template_location);
  }
  for (std::uint32_t source_index = 0;
       source_index < region.instructions.size(); ++source_index) {
    const auto &source = region.instructions[source_index];
    const auto &instruction_values =
        region.instruction_value_blocks[source_index];
    SemInst target;
    target.type = targets[source_index].type;
    const auto inst_arg = [&](std::uint32_t id) {
      return id < instructions.size() ? instructions[id].index
                                      : core::AnyId::InvalidIndex;
    };
    switch (source.opcode) {
    case GenericTemplateOpcode::Parameter:
      target.kind = SemInstKind::Parameter;
      target.arg0 = locals[source.arg0].index;
      break;
    case GenericTemplateOpcode::IntegerLiteral:
      target.kind = SemInstKind::IntegerLiteral;
      target.arg0 = addInteger(generic_template.integers[source.arg0]).index;
      break;
    case GenericTemplateOpcode::CharLiteral:
      target.kind = SemInstKind::CharLiteral;
      target.arg0 = addInteger(generic_template.integers[source.arg0]).index;
      break;
    case GenericTemplateOpcode::FloatLiteral:
      target.kind = SemInstKind::FloatLiteral;
      target.arg0 = addInteger(generic_template.integers[source.arg0]).index;
      break;
    case GenericTemplateOpcode::StringLiteral:
      target.kind = SemInstKind::StringLiteral;
      target.arg0 =
          values_->internStringLiteral(generic_template.strings[source.arg0])
              .index;
      break;
    case GenericTemplateOpcode::NullPointer:
      target.kind = SemInstKind::NullPointer;
      break;
    case GenericTemplateOpcode::TypeQuery: {
      if (source.arg0 >= generic_template.type_queries.size()) {
        error = "generic template type query index is out of range";
        return false;
      }
      const auto &query = generic_template.type_queries[source.arg0];
      SemTypeQueryArtifact descriptor;
      descriptor.kind = static_cast<SemTypeQueryArtifact::Kind>(query.kind);
      descriptor.source = map_type(query.source);
      if (!descriptor.source.hasValue())
        return false;
      if (query.kind == GenericTypeQueryKind::TypeSame) {
        descriptor.other = map_type(query.other);
        if (!descriptor.other.hasValue())
          return false;
      }
      descriptor.property = query.property;
      target.kind = SemInstKind::TypeQuery;
      target.arg0 = addInteger(addTypeQuery(std::move(descriptor))).index;
      break;
    }
    case GenericTemplateOpcode::BoolLiteral:
      target.kind = SemInstKind::BoolLiteral;
      target.arg0 = addInteger(source.arg0).index;
      break;
    case GenericTemplateOpcode::VoidValue:
      target.kind = SemInstKind::VoidValue;
      break;
    case GenericTemplateOpcode::NameRef:
      target.kind = SemInstKind::NameRef;
      target.arg0 = locals[source.arg0].index;
      break;
    case GenericTemplateOpcode::BorrowLocal:
      target.kind = SemInstKind::BorrowLocal;
      target.arg0 = locals[source.arg0].index;
      break;
    case GenericTemplateOpcode::BorrowPlace:
      target.kind = SemInstKind::BorrowPlace;
      target.arg0 = inst_arg(source.arg0);
      break;
    case GenericTemplateOpcode::CarrierView:
      target.kind = SemInstKind::CarrierView;
      target.arg0 = inst_arg(source.arg0);
      target.arg1 = addInteger(source.arg1).index;
      break;
    case GenericTemplateOpcode::Dereference:
      target.kind = SemInstKind::Dereference;
      target.arg0 = inst_arg(source.arg0);
      break;
    case GenericTemplateOpcode::Move:
      target.kind = SemInstKind::Move;
      target.arg0 = inst_arg(source.arg0);
      break;
    case GenericTemplateOpcode::Copy:
      target.kind = SemInstKind::Copy;
      target.arg0 = inst_arg(source.arg0);
      break;
    case GenericTemplateOpcode::BuiltinUnary:
    case GenericTemplateOpcode::BuiltinBinary: {
      std::vector<InstId> operands;
      for (const auto operand : instruction_values)
        operands.push_back(InstId(inst_arg(operand)));
      target.kind = source.opcode == GenericTemplateOpcode::BuiltinUnary
                        ? SemInstKind::BuiltinUnary
                        : SemInstKind::BuiltinBinary;
      target.arg0 = source.opcode == GenericTemplateOpcode::BuiltinUnary
                        ? operands.front().index
                        : addInstBlock(operands, true).index;
      target.arg1 = addInteger(source.arg0).index;
      break;
    }
    case GenericTemplateOpcode::Add:
      target.kind = SemInstKind::Add;
      target.arg0 = inst_arg(source.arg0);
      target.arg1 = inst_arg(source.arg1);
      break;
    case GenericTemplateOpcode::NumericConvert:
      target.kind = SemInstKind::NumericConvert;
      target.arg0 = inst_arg(source.arg0);
      break;
    case GenericTemplateOpcode::CheckedNumericCast:
      target.kind = SemInstKind::CheckedNumericCast;
      target.arg0 = inst_arg(source.arg0);
      break;
    case GenericTemplateOpcode::Equal:
      target.kind = SemInstKind::Equal;
      target.arg0 = inst_arg(source.arg0);
      target.arg1 = inst_arg(source.arg1);
      break;
    case GenericTemplateOpcode::MaterializeTemporary:
      target.kind = SemInstKind::MaterializeTemporary;
      target.arg0 = locals[source.arg0].index;
      target.arg1 = inst_arg(source.arg1);
      break;
    case GenericTemplateOpcode::EndFullExpression: {
      std::vector<InstId> temporaries;
      for (const auto temporary : instruction_values)
        temporaries.push_back(InstId(inst_arg(temporary)));
      target.kind = SemInstKind::EndFullExpression;
      target.arg0 = addInstBlock(temporaries, true).index;
      break;
    }
    case GenericTemplateOpcode::ExtendTemporary:
      target.kind = SemInstKind::ExtendTemporary;
      target.arg0 = locals[source.arg0].index;
      target.arg1 = inst_arg(source.arg1);
      break;
    case GenericTemplateOpcode::Assert:
      target.kind = SemInstKind::Assert;
      target.arg0 = inst_arg(source.arg0);
      target.arg1 = addInteger(source.arg1).index;
      break;
    case GenericTemplateOpcode::UnrecoverableFailure:
      target.kind = SemInstKind::UnrecoverableFailure;
      target.arg0 = addInteger(source.arg0).index;
      break;
    case GenericTemplateOpcode::BindName:
      target.kind = SemInstKind::BindName;
      target.arg0 = locals[source.arg0].index;
      target.arg1 = inst_arg(source.arg1);
      break;
    case GenericTemplateOpcode::Assign:
      target.kind = SemInstKind::Assign;
      target.arg0 = inst_arg(source.arg0);
      target.arg1 = inst_arg(source.arg1);
      break;
    case GenericTemplateOpcode::Return:
      target.kind = SemInstKind::Return;
      target.arg0 = inst_arg(source.arg0);
      break;
    case GenericTemplateOpcode::Defer:
      target.kind = SemInstKind::Defer;
      target.arg0 = source.arg0;
      break;
    case GenericTemplateOpcode::ScopedBlock:
      target.kind = SemInstKind::ScopedBlock;
      target.arg0 = source.arg0;
      break;
    case GenericTemplateOpcode::If:
      target.kind = SemInstKind::If;
      target.arg0 = inst_arg(source.arg0);
      target.arg1 = source.arg1;
      break;
    case GenericTemplateOpcode::IfArm:
      target.kind = SemInstKind::IfArm;
      target.arg0 = source.arg0;
      break;
    case GenericTemplateOpcode::While:
      target.kind = SemInstKind::While;
      target.arg0 = source.arg0;
      target.arg1 = source.arg1;
      break;
    case GenericTemplateOpcode::For:
      target.kind = SemInstKind::For;
      target.arg0 = source.arg0;
      target.arg1 = source.arg1;
      break;
    case GenericTemplateOpcode::ForClause:
      target.kind = SemInstKind::ForClause;
      target.arg0 = addInteger(source.arg0).index;
      target.arg1 = source.arg1;
      break;
    case GenericTemplateOpcode::DoWhile:
      target.kind = SemInstKind::DoWhile;
      target.arg0 = source.arg0;
      target.arg1 = source.arg1;
      break;
    case GenericTemplateOpcode::Break:
      target.kind = SemInstKind::Break;
      target.arg0 = addInteger(source.arg0).index;
      break;
    case GenericTemplateOpcode::Continue:
      target.kind = SemInstKind::Continue;
      target.arg0 = addInteger(source.arg0).index;
      break;
    case GenericTemplateOpcode::Yield:
      target.kind = SemInstKind::Yield;
      target.arg0 = inst_arg(source.arg0);
      break;
    case GenericTemplateOpcode::Switch:
      target.kind = SemInstKind::Switch;
      target.arg0 = inst_arg(source.arg0);
      target.arg1 = source.arg1;
      break;
    case GenericTemplateOpcode::SwitchArm:
      target.kind = SemInstKind::SwitchArm;
      target.arg0 = addInteger(source.arg0 == core::AnyId::InvalidIndex
                                   ? -1
                                   : static_cast<std::int64_t>(source.arg0))
                        .index;
      target.arg1 = source.arg1;
      break;
    case GenericTemplateOpcode::Call:
    case GenericTemplateOpcode::CompilerIntrinsicCall: {
      const auto intrinsic =
          source.opcode == GenericTemplateOpcode::CompilerIntrinsicCall;
      target.kind =
          intrinsic ? SemInstKind::CompilerIntrinsicCall : SemInstKind::Call;
      const auto &callee = generic_template.callees[source.arg0];
      const auto entity = imports_.registry().findEntity(
          callee.canonical_package, callee.canonical_module,
          callee.canonical_name);
      const auto *target_entity = imports_.registry().tryGetEntity(entity);
      if (!target_entity ||
          (callee.expected_fingerprint.hasValue() &&
           target_entity->fingerprint != callee.expected_fingerprint)) {
        error = "generic template callee fingerprint is stale";
        return false;
      }
      const auto &concrete_arguments =
          generic_template.callee_type_arguments[source.arg0];
      if ((!concrete_arguments.empty() &&
           concrete_arguments.size() !=
               target_entity->generic_parameter_count) ||
          (target_entity->generic_parameter_count == 0 &&
           !concrete_arguments.empty())) {
        error = "generic template callee has invalid concrete type arguments";
        return false;
      }
      std::vector<InstId> arguments;
      for (const auto argument : instruction_values)
        arguments.push_back(instructions[argument]);
      const auto reference =
          entity.hasValue() && resolve_callee
              ? resolve_callee(entity, arguments, concrete_arguments,
                               template_location)
              : FunctionRefId::invalid();
      if (!reference.hasValue()) {
        error = "cannot resolve a canonical generic template callee";
        return false;
      }
      if (intrinsic !=
          (functionIntrinsicRole(reference) != CompilerIntrinsicRole::None)) {
        error = "generic template intrinsic callee lost its verified role";
        return false;
      }
      target.arg0 = reference.index;
      value_blocks[source_index] = std::move(arguments);
      break;
    }
    case GenericTemplateOpcode::InterfaceCall:
      target.kind = SemInstKind::InterfaceCall;
      target.arg0 = addInteger(generic_template.integers[source.arg0]).index;
      for (const auto argument : instruction_values)
        value_blocks[source_index].push_back(instructions[argument]);
      break;
    case GenericTemplateOpcode::FunctionValue: {
      target.kind = SemInstKind::FunctionValue;
      const auto &callee = generic_template.callees[source.arg0];
      const auto entity = imports_.registry().findEntity(
          callee.canonical_package, callee.canonical_module,
          callee.canonical_name);
      const auto *target_entity = imports_.registry().tryGetEntity(entity);
      if (!target_entity ||
          (callee.expected_fingerprint.hasValue() &&
           target_entity->fingerprint != callee.expected_fingerprint)) {
        error = "generic template function-value fingerprint is stale";
        return false;
      }
      const auto &concrete_arguments =
          generic_template.callee_type_arguments[source.arg0];
      if (concrete_arguments.size() != target_entity->generic_parameter_count) {
        error = "generic template function value has invalid concrete type "
                "arguments";
        return false;
      }
      const auto reference =
          entity.hasValue() && resolve_callee
              ? resolve_callee(entity, {}, concrete_arguments,
                               template_location)
              : FunctionRefId::invalid();
      if (!reference.hasValue()) {
        error = "cannot resolve a canonical generic function value";
        return false;
      }
      target.arg0 = reference.index;
      break;
    }
    case GenericTemplateOpcode::Closure: {
      target.kind = SemInstKind::Closure;
      if (source.arg0 >= generic_template.callees.size() ||
          source.arg1 >= static_cast<std::uint32_t>(
                             SemCallableEnvironmentCapability::Count)) {
        error = "generic template closure has invalid descriptor metadata";
        return false;
      }
      const auto environment_type = TypeId(target.type);
      if (type(environment_type).kind != SemTypeKind::Nominal) {
        error = "generic template closure has a non-nominal environment";
        return false;
      }
      const auto environment_id = NominalTypeId(type(environment_type).arg0);
      auto environment = nominalType(environment_id);
      environment.flags |= SemNominalTypeClosureEnvironment;
      setNominalType(environment_id, std::move(environment));
      const auto &callee = generic_template.callees[source.arg0];
      const auto entity = imports_.registry().findEntity(
          callee.canonical_package, callee.canonical_module,
          callee.canonical_name, PublicEntityKind::Function,
          callee.expected_fingerprint);
      const auto *target_entity = imports_.registry().tryGetEntity(entity);
      if (!target_entity ||
          (callee.expected_fingerprint.hasValue() &&
           target_entity->fingerprint != callee.expected_fingerprint)) {
        error = "generic template closure target fingerprint is stale";
        return false;
      }
      std::vector<InstId> captures;
      for (const auto capture : instruction_values)
        captures.push_back(instructions[capture]);
      const auto &concrete_arguments =
          generic_template.callee_type_arguments[source.arg0];
      const auto reference =
          resolve_callee ? resolve_callee(entity, {}, concrete_arguments,
                                          template_location)
                         : FunctionRefId::invalid();
      if (!reference.hasValue()) {
        error = "cannot resolve a canonical generic closure target";
        return false;
      }
      const auto target_type = type(functionRef(reference).local_type);
      const auto parameters = target_type.kind == SemTypeKind::Function
                                  ? typeBlock(TypeBlockId(target_type.arg0))
                                  : std::span<const TypeId>{};
      if (parameters.empty() ||
          type(parameters.front()).kind != SemTypeKind::Reference ||
          referencePointee(parameters.front()) != environment_type) {
        error = "generic template closure target has an invalid environment";
        return false;
      }
      const auto capability = referenceMutability(parameters.front()) ==
                                      SemReferenceMutability::Mutable
                                  ? SemCallableEnvironmentCapability::Mutable
                                  : SemCallableEnvironmentCapability::ReadOnly;
      if (static_cast<std::uint32_t>(capability) != source.arg1) {
        error = "generic template closure capability does not match target";
        return false;
      }
      setCallableEnvironment({SemCallableEnvironmentKind::Closure,
                              environment_type, reference, reference,
                              capability, target_entity->fingerprint});
      target.arg0 = reference.index;
      value_blocks[source_index] = std::move(captures);
      break;
    }
    case GenericTemplateOpcode::BoundMethod: {
      target.kind = SemInstKind::BoundMethod;
      if (instruction_values.size() != 1 ||
          source.arg1 >= static_cast<std::uint32_t>(
                             SemCallableEnvironmentCapability::Count)) {
        error = "generic template bound method has invalid binding metadata";
        return false;
      }
      const auto environment_type = TypeId(target.type);
      if (type(environment_type).kind != SemTypeKind::Nominal) {
        error = "generic template bound method has a non-nominal environment";
        return false;
      }
      const auto environment_id = NominalTypeId(type(environment_type).arg0);
      auto environment = nominalType(environment_id);
      environment.flags |= SemNominalTypeBoundMethodEnvironment;
      setNominalType(environment_id, std::move(environment));
      const auto &callee = generic_template.callees[source.arg0];
      const auto entity = imports_.registry().findEntity(
          callee.canonical_package, callee.canonical_module,
          callee.canonical_name);
      const auto *target_entity = imports_.registry().tryGetEntity(entity);
      if (!target_entity ||
          target_entity->member_kind !=
              PublicFunctionArtifact::MemberKind::Instance ||
          (callee.expected_fingerprint.hasValue() &&
           target_entity->fingerprint != callee.expected_fingerprint)) {
        error = "generic template bound method target fingerprint is stale";
        return false;
      }
      const auto &concrete_arguments =
          generic_template.callee_type_arguments[source.arg0];
      if (concrete_arguments.size() != target_entity->generic_parameter_count) {
        error = "generic template bound method has invalid concrete type "
                "arguments";
        return false;
      }
      std::vector<InstId> arguments{instructions[instruction_values.front()]};
      const auto reference =
          entity.hasValue() && resolve_callee
              ? resolve_callee(entity, arguments, concrete_arguments,
                               template_location)
              : FunctionRefId::invalid();
      if (!reference.hasValue()) {
        error = "cannot resolve a canonical generic bound method target";
        return false;
      }
      const auto target_type = type(functionRef(reference).local_type);
      if (target_type.kind != SemTypeKind::Function ||
          typeBlock(TypeBlockId(target_type.arg0)).empty()) {
        error = "generic bound method target has no receiver";
        return false;
      }
      const auto receiver_parameter_id =
          typeBlock(TypeBlockId(target_type.arg0)).front();
      const auto &receiver_parameter = type(receiver_parameter_id);
      const auto capability = receiver_parameter.kind == SemTypeKind::Reference
                                  ? referenceMutability(receiver_parameter_id)
                                  : SemReferenceMutability::ReadOnly;
      const auto expected_capability =
          receiver_parameter.kind == SemTypeKind::Reference
              ? capability == SemReferenceMutability::Mutable
                    ? SemCallableEnvironmentCapability::Mutable
                    : SemCallableEnvironmentCapability::ReadOnly
              : SemCallableEnvironmentCapability::Consuming;
      if (static_cast<std::uint32_t>(expected_capability) != source.arg1) {
        error = "generic bound method capability does not match target";
        return false;
      }
      setCallableEnvironment({SemCallableEnvironmentKind::BoundMethod,
                              environment_type, reference, reference,
                              expected_capability, target_entity->fingerprint});
      target.arg0 = instructions[instruction_values.front()].index;
      target.arg1 = reference.index;
      break;
    }
    case GenericTemplateOpcode::IndirectCall:
      target.kind = SemInstKind::IndirectCall;
      target.arg0 = inst_arg(source.arg0);
      for (const auto argument : instruction_values)
        value_blocks[source_index].push_back(instructions[argument]);
      break;
    case GenericTemplateOpcode::AggregateInit: {
      target.kind = SemInstKind::AggregateInit;
      std::vector<InstId> fields;
      for (const auto field : instruction_values)
        fields.push_back(instructions[field]);
      value_blocks[source_index] = std::move(fields);
      break;
    }
    case GenericTemplateOpcode::ArrayLiteral: {
      target.kind = SemInstKind::ArrayLiteral;
      std::vector<InstId> elements;
      for (const auto element : instruction_values)
        elements.push_back(instructions[element]);
      value_blocks[source_index] = std::move(elements);
      break;
    }
    case GenericTemplateOpcode::TupleLiteral: {
      target.kind = SemInstKind::TupleLiteral;
      std::vector<InstId> elements;
      for (const auto element : instruction_values)
        elements.push_back(instructions[element]);
      value_blocks[source_index] = std::move(elements);
      break;
    }
    case GenericTemplateOpcode::Slice: {
      target.kind = SemInstKind::Slice;
      std::vector<InstId> operands;
      for (const auto operand : instruction_values)
        operands.push_back(instructions[operand]);
      value_blocks[source_index] = std::move(operands);
      break;
    }
    case GenericTemplateOpcode::UnionInit:
      target.kind = SemInstKind::UnionInit;
      target.arg0 = inst_arg(source.arg0);
      target.arg1 = addInteger(source.arg1).index;
      break;
    case GenericTemplateOpcode::EnumInit: {
      target.kind = SemInstKind::EnumInit;
      std::vector<InstId> payload;
      for (const auto field : instruction_values)
        payload.push_back(instructions[field]);
      value_blocks[source_index] = std::move(payload);
      target.arg1 = addInteger(source.arg1).index;
      break;
    }
    case GenericTemplateOpcode::EnumTag:
      target.kind = SemInstKind::EnumTag;
      target.arg0 = inst_arg(source.arg0);
      break;
    case GenericTemplateOpcode::EnumPayloadAccess:
      target.kind = SemInstKind::EnumPayloadAccess;
      target.arg0 = inst_arg(source.arg0);
      target.arg1 = addInteger(generic_template.integers[source.arg1]).index;
      break;
    case GenericTemplateOpcode::StructFieldAccess:
      target.kind = SemInstKind::StructFieldAccess;
      target.arg0 = inst_arg(source.arg0);
      target.arg1 = addInteger(source.arg1).index;
      break;
    case GenericTemplateOpcode::MemberAccess:
      target.kind = SemInstKind::MemberAccess;
      target.arg0 = inst_arg(source.arg0);
      target.arg1 = addName(values_->internIdentifier(
                                generic_template.strings[source.arg1]))
                        .index;
      break;
    case GenericTemplateOpcode::StaticIndex:
    case GenericTemplateOpcode::Index:
      target.kind = SemInstKind::Index;
      target.arg0 = inst_arg(source.arg0);
      target.arg1 = inst_arg(source.arg1);
      break;
    case GenericTemplateOpcode::UnionFieldAccess: {
      target.kind = SemInstKind::UnionFieldAccess;
      target.arg0 = inst_arg(source.arg0);
      const auto field = source.arg1 & UnionFieldIndexMask;
      const auto encoded = (source.arg1 & UnionFieldUnsafeBit) != 0
                               ? -static_cast<std::int64_t>(field) - 1
                               : static_cast<std::int64_t>(field);
      target.arg1 = addInteger(encoded).index;
      break;
    }
    case GenericTemplateOpcode::Count:
      error = "cannot materialize an invalid generic template opcode";
      return false;
    }
    targets[source_index] = target;
  }
  std::vector<InstBlockId> blocks;
  blocks.reserve(region.blocks.size());
  for (const auto &source : region.blocks) {
    std::vector<InstId> values;
    values.reserve(source.size());
    for (const auto instruction : source)
      values.push_back(instructions[instruction]);
    blocks.push_back(addInstBlock(values));
  }
  for (std::uint32_t index = 0; index < targets.size(); ++index) {
    auto &target = targets[index];
    const auto source_kind = region.instructions[index].opcode;
    if (source_kind == GenericTemplateOpcode::If)
      target.arg1 = blocks[target.arg1].index;
    else if (source_kind == GenericTemplateOpcode::IfArm)
      target.arg0 = blocks[target.arg0].index;
    else if (source_kind == GenericTemplateOpcode::While) {
      target.arg0 = blocks[target.arg0].index;
      target.arg1 = blocks[target.arg1].index;
    } else if (source_kind == GenericTemplateOpcode::For ||
               source_kind == GenericTemplateOpcode::DoWhile) {
      target.arg0 = blocks[target.arg0].index;
      target.arg1 = blocks[target.arg1].index;
    } else if (source_kind == GenericTemplateOpcode::ForClause) {
      target.arg1 = blocks[target.arg1].index;
    } else if (source_kind == GenericTemplateOpcode::Defer ||
               source_kind == GenericTemplateOpcode::ScopedBlock) {
      target.arg0 = blocks[target.arg0].index;
    } else if (source_kind == GenericTemplateOpcode::Switch ||
               source_kind == GenericTemplateOpcode::SwitchArm)
      target.arg1 = blocks[target.arg1].index;
    else if (source_kind == GenericTemplateOpcode::Call ||
             source_kind == GenericTemplateOpcode::CompilerIntrinsicCall ||
             source_kind == GenericTemplateOpcode::InterfaceCall ||
             source_kind == GenericTemplateOpcode::IndirectCall)
      target.arg1 = addInstBlock(value_blocks[index]).index;
    else if (source_kind == GenericTemplateOpcode::Closure)
      target.arg1 = addInstBlock(value_blocks[index], true).index;
    else if (source_kind == GenericTemplateOpcode::AggregateInit ||
             source_kind == GenericTemplateOpcode::ArrayLiteral ||
             source_kind == GenericTemplateOpcode::TupleLiteral ||
             source_kind == GenericTemplateOpcode::Slice ||
             source_kind == GenericTemplateOpcode::EnumInit)
      target.arg0 = addInstBlock(value_blocks[index], true).index;
    insts_.get(instructions[index]) = target;
  }
  std::vector<LocalId> parameters(
      locals.begin(), locals.begin() + generic_template.parameter_count);
  const auto previous = this->function(function);
  const auto imported_flags =
      (generic.hasValue() ? SemFunctionTemplate : SemFunctionConst) |
      SemFunctionEvaluatorArtifact;
  setFunction(function,
              {function_name, function_type, addLocalBlock(parameters),
               blocks[region.entry_block], imported_flags, generic,
               generic.hasValue()
                   ? values_->generics().generic(generic).self_specific
                   : SpecificId::invalid(),
               previous.semantic_owner, previous.semantic_role,
               previous.semantic_projector});
  if (!generic.hasValue())
    setFunctionDeclaration(
        function,
        {.kind = SemCallableDeclarationKind::Definition, .is_const = true});
  return true;
}
