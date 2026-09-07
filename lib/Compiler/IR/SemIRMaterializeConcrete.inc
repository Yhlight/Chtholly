// Included by SemIRMaterialization.cpp; this fragment shares SemIR stores
// and materialization callbacks without copying graph state.
bool SemIR::materializeConcreteSpecificBody(
    FunctionId function_id, const ConcreteSpecificNodeArtifact &node,
    NameId function_name, TypeId function_type, NodeId location,
    const std::function<FunctionRefId(const ConcreteCallTargetArtifact &,
                                      std::span<const InstId>, NodeId)>
        &resolve_callee,
    std::string &error) {
  error.clear();
  const auto map_type = [&](const PublicType &type) {
    return materializePublicType(type, GenericId::invalid(), location, error);
  };
  const auto &region = node.body;
  std::vector<bool> temporary_locals(node.local_types.size());
  for (const auto &instruction : region.instructions)
    if (instruction.opcode == GenericTemplateOpcode::MaterializeTemporary &&
        instruction.arg0 < temporary_locals.size())
      temporary_locals[instruction.arg0] = true;
  std::vector<LocalId> locals;
  locals.reserve(node.local_types.size());
  for (std::uint32_t index = 0; index < node.local_types.size(); ++index) {
    const auto generated = values_->internIdentifier(
        index < node.parameter_count
            ? "$cached_parameter" + std::to_string(index)
            : "$cached_local" + std::to_string(index));
    const auto type = map_type(node.local_types[index]);
    if (!type.hasValue()) {
      error = "cached concrete specialization has a dependent local type";
      return false;
    }
    locals.push_back(
        addLocal({addName(generated), type, location,
                  node.local_flags[index] |
                      (temporary_locals[index] ? SemLocalTemporary : 0U)}));
  }
  std::vector<InstId> instructions(region.instructions.size());
  std::vector<SemInst> targets(region.instructions.size());
  std::vector<std::vector<InstId>> value_blocks(region.instructions.size());
  for (std::uint32_t index = 0; index < instructions.size(); ++index) {
    targets[index].type = map_type(region.instructions[index].type).index;
    if (!TypeId(targets[index].type).hasValue())
      return false;
    instructions[index] = addRawInst(targets[index], location);
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
      target.arg0 = addInteger(node.integers[source.arg0]).index;
      break;
    case GenericTemplateOpcode::CharLiteral:
      target.kind = SemInstKind::CharLiteral;
      target.arg0 = addInteger(node.integers[source.arg0]).index;
      break;
    case GenericTemplateOpcode::FloatLiteral:
      target.kind = SemInstKind::FloatLiteral;
      target.arg0 = addInteger(node.integers[source.arg0]).index;
      break;
    case GenericTemplateOpcode::StringLiteral:
      target.kind = SemInstKind::StringLiteral;
      target.arg0 =
          values_->internStringLiteral(node.strings[source.arg0]).index;
      break;
    case GenericTemplateOpcode::NullPointer:
      target.kind = SemInstKind::NullPointer;
      break;
    case GenericTemplateOpcode::TypeQuery:
      error = "concrete specialization contains an unresolved type query";
      return false;
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
      std::vector<InstId> arguments;
      for (const auto argument : instruction_values)
        arguments.push_back(instructions[argument]);
      const auto callee =
          resolve_callee
              ? resolve_callee(node.callees[source.arg0], arguments, location)
              : FunctionRefId::invalid();
      if (!callee.hasValue()) {
        error = "cached concrete specialization has an unresolved callee";
        return false;
      }
      if (intrinsic !=
          (functionIntrinsicRole(callee) != CompilerIntrinsicRole::None)) {
        error = "cached intrinsic call lost its verified target role";
        return false;
      }
      target.arg0 = callee.index;
      value_blocks[source_index] = std::move(arguments);
      break;
    }
    case GenericTemplateOpcode::InterfaceCall:
      error = "cached concrete specialization contains a symbolic interface "
              "call";
      return false;
    case GenericTemplateOpcode::FunctionValue: {
      target.kind = SemInstKind::FunctionValue;
      const auto callee =
          resolve_callee
              ? resolve_callee(node.callees[source.arg0], {}, location)
              : FunctionRefId::invalid();
      if (!callee.hasValue()) {
        error = "cached specialization has an unresolved function value";
        return false;
      }
      target.arg0 = callee.index;
      break;
    }
    case GenericTemplateOpcode::Closure: {
      target.kind = SemInstKind::Closure;
      if (source.arg0 >= node.callees.size() ||
          source.arg1 >= static_cast<std::uint32_t>(
                             SemCallableEnvironmentCapability::Count)) {
        error = "cached closure has invalid descriptor metadata";
        return false;
      }
      const auto environment_type = TypeId(target.type);
      if (type(environment_type).kind != SemTypeKind::Nominal) {
        error = "cached closure has a non-nominal environment";
        return false;
      }
      const auto environment_id = NominalTypeId(type(environment_type).arg0);
      auto environment = nominalType(environment_id);
      environment.flags |= SemNominalTypeClosureEnvironment;
      setNominalType(environment_id, std::move(environment));
      std::vector<InstId> captures;
      for (const auto capture : instruction_values)
        captures.push_back(instructions[capture]);
      const auto &encoded_target = node.callees[source.arg0];
      const auto reference =
          resolve_callee ? resolve_callee(encoded_target, captures, location)
                         : FunctionRefId::invalid();
      if (!reference.hasValue()) {
        error = "cached closure has an unresolved target";
        return false;
      }
      const auto target_type = type(functionRef(reference).local_type);
      const auto parameters = target_type.kind == SemTypeKind::Function
                                  ? typeBlock(TypeBlockId(target_type.arg0))
                                  : std::span<const TypeId>{};
      if (parameters.empty() ||
          type(parameters.front()).kind != SemTypeKind::Reference ||
          referencePointee(parameters.front()) != environment_type) {
        error = "cached closure target has an invalid environment";
        return false;
      }
      const auto capability = referenceMutability(parameters.front()) ==
                                      SemReferenceMutability::Mutable
                                  ? SemCallableEnvironmentCapability::Mutable
                                  : SemCallableEnvironmentCapability::ReadOnly;
      if (static_cast<std::uint32_t>(capability) != source.arg1) {
        error = "cached closure capability does not match target";
        return false;
      }
      auto identity = node.request_fingerprint;
      if (encoded_target.kind == ConcreteCallTargetKind::ExternalComponent)
        identity = encoded_target.component_fingerprint;
      else if (encoded_target.kind == ConcreteCallTargetKind::PublicEntity)
        identity = encoded_target.public_entity.expected_fingerprint;
      setCallableEnvironment({SemCallableEnvironmentKind::Closure,
                              environment_type, reference, reference,
                              capability, identity});
      target.arg0 = reference.index;
      value_blocks[source_index] = std::move(captures);
      break;
    }
    case GenericTemplateOpcode::BoundMethod: {
      target.kind = SemInstKind::BoundMethod;
      if (instruction_values.size() != 1 ||
          source.arg1 >= static_cast<std::uint32_t>(
                             SemCallableEnvironmentCapability::Count)) {
        error = "cached bound method has invalid binding metadata";
        return false;
      }
      const auto environment_type = TypeId(target.type);
      if (type(environment_type).kind != SemTypeKind::Nominal) {
        error = "cached bound method has a non-nominal environment";
        return false;
      }
      const auto environment_id = NominalTypeId(type(environment_type).arg0);
      auto environment = nominalType(environment_id);
      environment.flags |= SemNominalTypeBoundMethodEnvironment;
      setNominalType(environment_id, std::move(environment));
      std::vector<InstId> arguments{instructions[instruction_values.front()]};
      const auto callee =
          resolve_callee
              ? resolve_callee(node.callees[source.arg0], arguments, location)
              : FunctionRefId::invalid();
      if (!callee.hasValue()) {
        error = "cached bound method has an unresolved target";
        return false;
      }
      const auto target_type = type(functionRef(callee).local_type);
      if (target_type.kind != SemTypeKind::Function ||
          typeBlock(TypeBlockId(target_type.arg0)).empty()) {
        error = "cached bound method target has no receiver";
        return false;
      }
      const auto receiver_parameter =
          type(typeBlock(TypeBlockId(target_type.arg0)).front());
      const auto expected_capability =
          receiver_parameter.kind == SemTypeKind::Reference
              ? referenceMutability(
                    typeBlock(TypeBlockId(target_type.arg0)).front()) ==
                        SemReferenceMutability::Mutable
                    ? SemCallableEnvironmentCapability::Mutable
                    : SemCallableEnvironmentCapability::ReadOnly
              : SemCallableEnvironmentCapability::Consuming;
      if (static_cast<std::uint32_t>(expected_capability) != source.arg1) {
        error = "cached bound method capability does not match target";
        return false;
      }
      auto identity = node.request_fingerprint;
      const auto &encoded_target = node.callees[source.arg0];
      if (encoded_target.kind == ConcreteCallTargetKind::ExternalComponent)
        identity = encoded_target.component_fingerprint;
      else if (encoded_target.kind == ConcreteCallTargetKind::PublicEntity)
        identity = encoded_target.public_entity.expected_fingerprint;
      setCallableEnvironment({SemCallableEnvironmentKind::BoundMethod,
                              environment_type, callee, callee,
                              expected_capability, identity});
      target.arg0 = instructions[instruction_values.front()].index;
      target.arg1 = callee.index;
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
      target.arg1 = addInteger(node.integers[source.arg1]).index;
      break;
    case GenericTemplateOpcode::StructFieldAccess:
      target.kind = SemInstKind::StructFieldAccess;
      target.arg0 = inst_arg(source.arg0);
      target.arg1 = addInteger(source.arg1).index;
      break;
    case GenericTemplateOpcode::MemberAccess:
      target.kind = SemInstKind::MemberAccess;
      target.arg0 = inst_arg(source.arg0);
      target.arg1 =
          addName(values_->internIdentifier(node.strings[source.arg1])).index;
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
      error = "cached concrete specialization has an invalid opcode";
      return false;
    }
    targets[source_index] = target;
  }
  std::vector<InstBlockId> blocks;
  blocks.reserve(node.body.blocks.size());
  for (const auto &source : node.body.blocks) {
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
  std::vector<LocalId> parameters(locals.begin(),
                                  locals.begin() + node.parameter_count);
  NominalTypeId semantic_owner;
  if (node.semantic_contract.domain != CallableSemanticDomain::Ordinary &&
      !parameters.empty()) {
    const auto parameter_type = local(parameters.front()).type;
    if (type(parameter_type).kind == SemTypeKind::Reference) {
      const auto owner_type = referencePointee(parameter_type);
      if (type(owner_type).kind == SemTypeKind::Nominal)
        semantic_owner = NominalTypeId(type(owner_type).arg0);
    }
  }
  const auto previous = function(function_id);
  setFunction(function_id,
              {function_name, function_type, addLocalBlock(parameters),
               blocks[node.body.entry_block], SemFunctionSpecific,
               previous.generic, previous.specific, semantic_owner,
               static_cast<std::uint8_t>(node.semantic_contract.role),
               previous.semantic_projector});
  setFunctionSemanticContract(
      function_id,
      {node.semantic_contract.domain, node.semantic_contract.role,
       node.semantic_contract.capability, semantic_owner,
       node.semantic_contract.projector_field,
       node.semantic_contract.whole_carrier,
       node.semantic_contract.carrier_path,
       node.semantic_contract.has_bit_range, node.semantic_contract.bit_begin,
       node.semantic_contract.bit_end});
  setFunctionOwnership(function_id, node.ownership_summary);
  expectFunctionOwnership(function_id, node.ownership_summary);
  return true;
}
