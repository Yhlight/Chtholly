// Included by SemanticContextSpecialization.cpp; this fragment aliases the
// single SemanticContext specialization state and preserves queue ordering.
SemanticContext::buildConcreteSpecificNode(
    SpecificId specific_id,
    const std::unordered_map<std::uint32_t, std::uint32_t> &component_nodes,
    std::string &error) {
  const auto cacheable = cacheable_specifics_.find(specific_id.index);
  const auto reference = specific_function_refs_.find(specific_id.index);
  if (cacheable == cacheable_specifics_.end() ||
      reference == specific_function_refs_.end()) {
    error = "concrete specialization is missing its canonical identity";
    return std::nullopt;
  }
  const auto &function_ref = sem_ir_.functionRef(reference->second);
  if (!function_ref.local_function.hasValue()) {
    error = "concrete specialization is missing its SemIR function";
    return std::nullopt;
  }
  const auto &function = sem_ir_.function(function_ref.local_function);
  const auto &function_type = sem_ir_.type(function.type);
  ConcreteSpecificNodeArtifact artifact;
  artifact.request_fingerprint = cacheable->second.request_fingerprint;
  artifact.template_entity = cacheable->second.entity;
  artifact.arguments = cacheable->second.arguments;
  artifact.constraint_witnesses = cacheable->second.constraint_witnesses;
  artifact.parameter_count = static_cast<std::uint32_t>(
      sem_ir_.localBlock(function.parameters).size());
  artifact.ownership_summary =
      sem_ir_.functionOwnership(function_ref.local_function);

  const auto map_type = [&](TypeId id) -> PublicType {
    const auto mapped = concretePublicType(sem_ir_.canonicalType(id));
    return mapped ? std::move(*mapped) : PublicType{};
  };
  for (const auto &descriptor : sem_ir_.typedChannelDescriptors()) {
    if (descriptor.owner_function != function_ref.local_function)
      continue;
    auto payload = map_type(descriptor.payload_type);
    if (payload.kind == PublicTypeKind::Count) {
      error = "typed channel descriptor payload is not public";
      return std::nullopt;
    }
    ConcreteTypedChannelDescriptor concrete;
    concrete.payload_type = std::move(payload);
    concrete.payload_type_fingerprint = descriptor.payload_type_fingerprint;
    concrete.layout_fingerprint = descriptor.layout_fingerprint;
    concrete.lifecycle_fingerprint = descriptor.lifecycle_fingerprint;
    concrete.representation = descriptor.representation;
    concrete.concurrency = descriptor.concurrency;
    concrete.runtime_abi_epoch = descriptor.runtime_abi_epoch;
    concrete.move_target = descriptor.move_target;
    concrete.drop_target = descriptor.drop_target;
    concrete.component_identity = descriptor.component_identity;
    concrete.operation_identity = descriptor.operation_identity;
    concrete.operation_kind = descriptor.operation_kind;
    concrete.lease_policy = descriptor.lease_policy;
    concrete.ownership_flags = descriptor.ownership_flags;
    concrete.component_descriptor_digest = descriptor.component_descriptor_digest;
    if (!verifyConcreteTypedChannelDescriptor(concrete, error))
      return std::nullopt;
    artifact.typed_channels.push_back(std::move(concrete));
  }
  const auto &local_contract =
      sem_ir_.functionSemanticContract(function_ref.local_function);
  artifact.semantic_contract.domain = local_contract.domain;
  artifact.semantic_contract.role = local_contract.role;
  artifact.semantic_contract.capability = local_contract.capability;
  artifact.semantic_contract.projector_field = local_contract.projector_field;
  artifact.semantic_contract.whole_carrier = local_contract.whole_carrier;
  artifact.semantic_contract.carrier_path = local_contract.carrier_path;
  artifact.semantic_contract.has_bit_range = local_contract.has_bit_range;
  artifact.semantic_contract.bit_begin = local_contract.bit_begin;
  artifact.semantic_contract.bit_end = local_contract.bit_end;
  if (local_contract.owner.hasValue()) {
    const auto parameters = sem_ir_.typeBlock(TypeBlockId(function_type.arg0));
    if (parameters.empty() ||
        sem_ir_.type(parameters.front()).kind != SemTypeKind::Reference) {
      error = "representation specific has no canonical owner parameter";
      return std::nullopt;
    }
    artifact.semantic_contract.owner =
        map_type(sem_ir_.referencePointee(parameters.front()));
  }

  std::vector<InstBlockId> blocks{function.body};
  for (std::size_t block_index = 0; block_index < blocks.size();
       ++block_index) {
    for (const auto inst_id : sem_ir_.instBlock(blocks[block_index])) {
      const auto &inst = sem_ir_.inst(inst_id);
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
    const auto values = sem_ir_.instBlock(block);
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
    const auto type = map_type(sem_ir_.local(local).type);
    if (type.kind == PublicTypeKind::Count)
      return core::AnyId::InvalidIndex;
    const auto index = static_cast<std::uint32_t>(artifact.local_types.size());
    artifact.local_types.push_back(type);
    artifact.local_flags.push_back(sem_ir_.local(local).flags);
    local_map.emplace(local.index, index);
    return index;
  };
  for (const auto parameter : sem_ir_.localBlock(function.parameters))
    if (add_local(parameter) == core::AnyId::InvalidIndex) {
      error = "concrete specialization has an unsupported parameter type";
      return std::nullopt;
    }
  for (const auto parameter :
       sem_ir_.typeBlock(TypeBlockId(function_type.arg0)))
    artifact.body.results.push_back(map_type(parameter));
  artifact.body.results.push_back(map_type(TypeId(function_type.arg1)));
  if (std::ranges::any_of(artifact.body.results, [](PublicType type) {
        return type.kind == PublicTypeKind::Count;
      })) {
    error = "concrete specialization has an unsupported result type";
    return std::nullopt;
  }

  std::unordered_map<std::uint32_t, std::uint32_t> integer_map;
  std::unordered_map<std::string, std::uint32_t> string_map;
  for (const auto old_id : instructions) {
    const auto &old = sem_ir_.inst(old_id);
    GenericTemplateInstArtifact value;
    std::vector<std::uint32_t> instruction_values;
    value.type = map_type(TypeId(old.type));
    if (value.type.kind == PublicTypeKind::Count) {
      error = "concrete specialization has an unsupported instruction type";
      return std::nullopt;
    }
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
        artifact.integers.push_back(sem_ir_.integer(IntegerId(old.arg0)));
      value.arg0 = found->second;
      break;
    }
    case SemInstKind::FloatLiteral: {
      value.opcode = GenericTemplateOpcode::FloatLiteral;
      const auto [found, inserted] = integer_map.emplace(
          old.arg0, static_cast<std::uint32_t>(artifact.integers.size()));
      if (inserted)
        artifact.integers.push_back(sem_ir_.integer(IntegerId(old.arg0)));
      value.arg0 = found->second;
      break;
    }
    case SemInstKind::CharLiteral: {
      value.opcode = GenericTemplateOpcode::CharLiteral;
      const auto [found, inserted] = integer_map.emplace(
          old.arg0, static_cast<std::uint32_t>(artifact.integers.size()));
      if (inserted)
        artifact.integers.push_back(sem_ir_.integer(IntegerId(old.arg0)));
      value.arg0 = found->second;
      break;
    }
    case SemInstKind::StringLiteral: {
      value.opcode = GenericTemplateOpcode::StringLiteral;
      const auto spelling = std::string(sem_ir_.string(
          StringLiteralId(static_cast<std::uint32_t>(old.arg0))));
      const auto [found, inserted] = string_map.emplace(
          spelling, static_cast<std::uint32_t>(artifact.strings.size()));
      if (inserted)
        artifact.strings.push_back(spelling);
      value.arg0 = found->second;
      break;
    }
    case SemInstKind::ArrayLiteral:
      value.opcode = GenericTemplateOpcode::ArrayLiteral;
      for (const auto element : sem_ir_.instBlock(InstBlockId(old.arg0)))
        instruction_values.push_back(inst_arg(element.index));
      break;
    case SemInstKind::NullPointer:
      value.opcode = GenericTemplateOpcode::NullPointer;
      break;
    case SemInstKind::TypeQuery:
      error = "concrete specialization contains an unresolved type query";
      return std::nullopt;
    case SemInstKind::BoolLiteral:
      value.opcode = GenericTemplateOpcode::BoolLiteral;
      value.arg0 =
          static_cast<std::uint32_t>(sem_ir_.integer(IntegerId(old.arg0)));
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
          static_cast<std::uint32_t>(sem_ir_.integer(IntegerId(old.arg1)));
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
          static_cast<std::uint32_t>(sem_ir_.integer(IntegerId(old.arg1)));
      instruction_values.push_back(inst_arg(old.arg0));
      break;
    case SemInstKind::BuiltinBinary:
      value.opcode = GenericTemplateOpcode::BuiltinBinary;
      value.arg0 =
          static_cast<std::uint32_t>(sem_ir_.integer(IntegerId(old.arg1)));
      for (const auto operand : sem_ir_.instBlock(InstBlockId(old.arg0)))
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
      for (const auto temporary : sem_ir_.instBlock(InstBlockId(old.arg0)))
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
          static_cast<std::uint32_t>(sem_ir_.integer(IntegerId(old.arg1)));
      break;
    case SemInstKind::UnrecoverableFailure:
      value.opcode = GenericTemplateOpcode::UnrecoverableFailure;
      value.arg0 =
          static_cast<std::uint32_t>(sem_ir_.integer(IntegerId(old.arg0)));
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
          static_cast<std::uint32_t>(sem_ir_.integer(IntegerId(old.arg0)));
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
          static_cast<std::uint32_t>(sem_ir_.integer(IntegerId(old.arg0)));
      break;
    case SemInstKind::Continue:
      value.opcode = GenericTemplateOpcode::Continue;
      value.arg0 =
          static_cast<std::uint32_t>(sem_ir_.integer(IntegerId(old.arg0)));
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
      const auto variant = sem_ir_.integer(IntegerId(old.arg0));
      if (variant < -1 || static_cast<std::uint64_t>(variant) >
                              std::numeric_limits<std::uint32_t>::max()) {
        error = "concrete specialization has an invalid switch arm";
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
      const auto &callee_ref = sem_ir_.functionRef(FunctionRefId(old.arg0));
      ConcreteCallTargetArtifact target;
      target.callable_type = map_type(callee_ref.local_type);
      if (target.callable_type.kind != PublicTypeKind::Function) {
        error = "concrete specialization call has no concrete callable type";
        return std::nullopt;
      }
      if (callee_ref.specific.hasValue()) {
        if (const auto internal =
                component_nodes.find(callee_ref.specific.index);
            internal != component_nodes.end()) {
          target.kind = ConcreteCallTargetKind::ComponentNode;
          target.node_index = internal->second;
          target.request_fingerprint =
              cacheable_specifics_.at(callee_ref.specific.index)
                  .request_fingerprint;
        } else {
          const auto component =
              specific_component_fingerprints_.find(callee_ref.specific.index);
          const auto requested =
              cacheable_specifics_.find(callee_ref.specific.index);
          if (component == specific_component_fingerprints_.end() ||
              requested == cacheable_specifics_.end()) {
            error = "concrete specialization calls a non-cacheable specific";
            return std::nullopt;
          }
          target.kind = ConcreteCallTargetKind::ExternalComponent;
          target.request_fingerprint = requested->second.request_fingerprint;
          target.component_fingerprint = component->second;
        }
      } else if (callee_ref.public_entity.hasValue()) {
        const auto entity = publicEntityReference(callee_ref.public_entity);
        if (!entity) {
          error = "concrete specialization has an unresolved public callee";
          return std::nullopt;
        }
        target.kind = ConcreteCallTargetKind::PublicEntity;
        target.public_entity = *entity;
        const auto concrete_arguments =
            sem_ir_.functionRefConcreteArguments(FunctionRefId(old.arg0));
        target.type_arguments.assign(concrete_arguments.begin(),
                                     concrete_arguments.end());
      } else {
        error = "concrete specialization calls a private entity";
        return std::nullopt;
      }
      value.arg0 = static_cast<std::uint32_t>(artifact.callees.size());
      artifact.callees.push_back(std::move(target));
      value.arg1 = core::AnyId::InvalidIndex;
      for (const auto argument : sem_ir_.instBlock(InstBlockId(old.arg1))) {
        const auto mapped = inst_arg(argument.index);
        if (mapped == core::AnyId::InvalidIndex) {
          error = "concrete specialization call operands are not canonical";
          return std::nullopt;
        }
        instruction_values.push_back(mapped);
      }
      break;
    }
    case SemInstKind::FunctionValue: {
      value.opcode = GenericTemplateOpcode::FunctionValue;
      const auto &callee_ref = sem_ir_.functionRef(FunctionRefId(old.arg0));
      std::optional<PublicEntityReferenceArtifact> entity;
      const auto callable_type = map_type(callee_ref.local_type);
      if (callable_type.kind != PublicTypeKind::Function) {
        error = "concrete function value has no concrete callable type";
        return std::nullopt;
      }
      if (callee_ref.public_entity.hasValue()) {
        entity = publicEntityReference(callee_ref.public_entity);
      } else if (callee_ref.local_function.hasValue()) {
        auto local_id = callee_ref.local_function;
        if (callee_ref.generic.hasValue()) {
          for (std::uint32_t function_index = 0;
               function_index < sem_ir_.functionCount(); ++function_index) {
            const auto candidate_id = FunctionId(function_index);
            const auto &candidate = sem_ir_.function(candidate_id);
            if (candidate.generic == callee_ref.generic &&
                (candidate.flags & SemFunctionTemplate) != 0) {
              local_id = candidate_id;
              break;
            }
          }
        }
        const auto &local = sem_ir_.function(local_id);
        bool is_public = (local.flags & SemFunctionPublic) != 0;
        const SemNominalMemberFunction *member = nullptr;
        if (local.semantic_owner.hasValue()) {
          const auto &owner = sem_ir_.nominalType(local.semantic_owner);
          const auto found = std::ranges::find_if(
              owner.member_functions, [&](const auto &candidate) {
                return candidate.target.hasValue() &&
                       sem_ir_.functionRef(candidate.target).local_function ==
                           local_id;
              });
          if (found != owner.member_functions.end())
            member = &*found;
          is_public = member &&
                      (member->flags & SemNominalMemberFunctionPublic) != 0 &&
                      (member->flags & SemNominalMemberFunctionAssociated) != 0;
        }
        if (!is_public) {
          error = "concrete specialization has a non-public function value";
          return std::nullopt;
        }
        auto canonical_name =
            std::string(values_.identifier(sem_ir_.name(local.name).text));
        if (local.semantic_owner.hasValue()) {
          const auto &owner = sem_ir_.nominalType(local.semantic_owner);
          canonical_name =
              std::string(values_.identifier(sem_ir_.name(owner.name).text)) +
              "::" + canonical_name;
        }
        const auto canonical = sem_ir_.importIRs().registry().findEntity(
            artifact.template_entity.canonical_package,
            artifact.template_entity.canonical_module, canonical_name,
            PublicEntityKind::Function);
        if (canonical.hasValue())
          entity = publicEntityReference(canonical);
      }
      if (!entity) {
        error = "concrete specialization has an unresolved function value";
        return std::nullopt;
      }
      ConcreteCallTargetArtifact target;
      target.kind = ConcreteCallTargetKind::PublicEntity;
      target.public_entity = *entity;
      target.callable_type = callable_type;
      target.type_arguments = std::vector<PublicType>(
          sem_ir_.functionRefConcreteArguments(FunctionRefId(old.arg0)).begin(),
          sem_ir_.functionRefConcreteArguments(FunctionRefId(old.arg0)).end());
      value.arg0 = static_cast<std::uint32_t>(artifact.callees.size());
      artifact.callees.push_back(std::move(target));
      break;
    }
    case SemInstKind::Closure: {
      value.opcode = GenericTemplateOpcode::Closure;
      const auto &callee_ref = sem_ir_.functionRef(FunctionRefId(old.arg0));
      ConcreteCallTargetArtifact target;
      target.callable_type = map_type(callee_ref.local_type);
      if (target.callable_type.kind != PublicTypeKind::Function) {
        error = "concrete closure target has no concrete callable type";
        return std::nullopt;
      }
      if (callee_ref.specific.hasValue()) {
        if (const auto internal =
                component_nodes.find(callee_ref.specific.index);
            internal != component_nodes.end()) {
          target.kind = ConcreteCallTargetKind::ComponentNode;
          target.node_index = internal->second;
          target.request_fingerprint =
              cacheable_specifics_.at(callee_ref.specific.index)
                  .request_fingerprint;
        } else {
          const auto component =
              specific_component_fingerprints_.find(callee_ref.specific.index);
          const auto requested =
              cacheable_specifics_.find(callee_ref.specific.index);
          if (component == specific_component_fingerprints_.end() ||
              requested == cacheable_specifics_.end()) {
            error = "concrete closure targets a non-cacheable specific";
            return std::nullopt;
          }
          target.kind = ConcreteCallTargetKind::ExternalComponent;
          target.request_fingerprint = requested->second.request_fingerprint;
          target.component_fingerprint = component->second;
        }
      } else if (callee_ref.public_entity.hasValue()) {
        const auto entity = publicEntityReference(callee_ref.public_entity);
        if (!entity) {
          error = "concrete closure has an unresolved target";
          return std::nullopt;
        }
        target.kind = ConcreteCallTargetKind::PublicEntity;
        target.public_entity = *entity;
      } else {
        error = "concrete closure has no canonical target";
        return std::nullopt;
      }
      const auto *info = sem_ir_.tryGetCallableEnvironment(TypeId(old.type));
      if (!info || info->kind != SemCallableEnvironmentKind::Closure) {
        error = "concrete closure has no callable-environment facts";
        return std::nullopt;
      }
      value.arg0 = static_cast<std::uint32_t>(artifact.callees.size());
      value.arg1 = static_cast<std::uint32_t>(info->capability);
      artifact.callees.push_back(std::move(target));
      for (const auto capture : sem_ir_.instBlock(InstBlockId(old.arg1))) {
        const auto mapped = inst_arg(capture.index);
        if (mapped == core::AnyId::InvalidIndex) {
          error = "concrete closure captures are not canonical";
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
        error = "concrete specialization bound-method receiver is not "
                "canonical";
        return std::nullopt;
      }
      const auto &callee_ref = sem_ir_.functionRef(FunctionRefId(old.arg1));
      const auto callable_type = map_type(callee_ref.local_type);
      if (callable_type.kind != PublicTypeKind::Function) {
        error = "concrete bound method has no concrete callable type";
        return std::nullopt;
      }
      const auto target_type = sem_ir_.type(callee_ref.local_type);
      const auto parameters =
          target_type.kind == SemTypeKind::Function
              ? sem_ir_.typeBlock(TypeBlockId(target_type.arg0))
              : std::span<const TypeId>{};
      if (parameters.empty()) {
        error = "concrete specialization bound method has no receiver";
        return std::nullopt;
      }
      const auto receiver_parameter = sem_ir_.type(parameters.front());
      value.arg1 = receiver_parameter.kind == SemTypeKind::Reference
                       ? static_cast<std::uint32_t>(
                             sem_ir_.referenceMutability(parameters.front()) ==
                                     SemReferenceMutability::Mutable
                                 ? SemCallableEnvironmentCapability::Mutable
                                 : SemCallableEnvironmentCapability::ReadOnly)
                       : static_cast<std::uint32_t>(
                             SemCallableEnvironmentCapability::Consuming);
      ConcreteCallTargetArtifact target;
      if (callee_ref.specific.hasValue()) {
        if (const auto internal =
                component_nodes.find(callee_ref.specific.index);
            internal != component_nodes.end()) {
          target.kind = ConcreteCallTargetKind::ComponentNode;
          target.node_index = internal->second;
          target.request_fingerprint =
              cacheable_specifics_.at(callee_ref.specific.index)
                  .request_fingerprint;
        } else {
          const auto component =
              specific_component_fingerprints_.find(callee_ref.specific.index);
          const auto requested =
              cacheable_specifics_.find(callee_ref.specific.index);
          if (component == specific_component_fingerprints_.end() ||
              requested == cacheable_specifics_.end()) {
            error = "concrete specialization bound method targets a non-"
                    "cacheable specific";
            return std::nullopt;
          }
          target.kind = ConcreteCallTargetKind::ExternalComponent;
          target.request_fingerprint = requested->second.request_fingerprint;
          target.component_fingerprint = component->second;
        }
      } else if (callee_ref.public_entity.hasValue()) {
        const auto entity = publicEntityReference(callee_ref.public_entity);
        const auto *registry_entity =
            sem_ir_.importIRs().registry().tryGetEntity(
                callee_ref.public_entity);
        if (!entity || !registry_entity ||
            registry_entity->member_kind !=
                PublicFunctionArtifact::MemberKind::Instance) {
          error = "concrete specialization bound method target is not an "
                  "instance member";
          return std::nullopt;
        }
        target.kind = ConcreteCallTargetKind::PublicEntity;
        target.public_entity = *entity;
      } else {
        error = "concrete specialization bound method has no canonical "
                "target";
        return std::nullopt;
      }
      target.type_arguments = std::vector<PublicType>(
          sem_ir_.functionRefConcreteArguments(FunctionRefId(old.arg1)).begin(),
          sem_ir_.functionRefConcreteArguments(FunctionRefId(old.arg1)).end());
      target.callable_type = callable_type;
      value.arg0 = static_cast<std::uint32_t>(artifact.callees.size());
      artifact.callees.push_back(std::move(target));
      instruction_values.push_back(mapped_receiver);
      break;
    }
    case SemInstKind::IndirectCall:
      value.opcode = GenericTemplateOpcode::IndirectCall;
      value.arg0 = inst_arg(old.arg0);
      for (const auto argument : sem_ir_.instBlock(InstBlockId(old.arg1))) {
        const auto mapped = inst_arg(argument.index);
        if (mapped == core::AnyId::InvalidIndex) {
          error = "concrete specialization indirect-call operands are not "
                  "canonical";
          return std::nullopt;
        }
        instruction_values.push_back(mapped);
      }
      break;
    case SemInstKind::AggregateInit: {
      value.opcode = GenericTemplateOpcode::AggregateInit;
      for (const auto field : sem_ir_.instBlock(InstBlockId(old.arg0))) {
        const auto mapped = inst_arg(field.index);
        if (mapped == core::AnyId::InvalidIndex) {
          error = "concrete specialization aggregate fields are not canonical";
          return std::nullopt;
        }
        instruction_values.push_back(mapped);
      }
      break;
    }
    case SemInstKind::TupleLiteral: {
      value.opcode = GenericTemplateOpcode::TupleLiteral;
      for (const auto element : sem_ir_.instBlock(InstBlockId(old.arg0))) {
        const auto mapped = inst_arg(element.index);
        if (mapped == core::AnyId::InvalidIndex) {
          error = "concrete specialization tuple elements are not canonical";
          return std::nullopt;
        }
        instruction_values.push_back(mapped);
      }
      break;
    }
    case SemInstKind::Slice: {
      value.opcode = GenericTemplateOpcode::Slice;
      for (const auto operand : sem_ir_.instBlock(InstBlockId(old.arg0))) {
        const auto mapped = inst_arg(operand.index);
        if (mapped == core::AnyId::InvalidIndex) {
          error = "concrete specialization slice operands are not canonical";
          return std::nullopt;
        }
        instruction_values.push_back(mapped);
      }
      break;
    }
    case SemInstKind::UnionInit: {
      const auto member = sem_ir_.integer(IntegerId(old.arg1));
      if (member < 0 ||
          static_cast<std::uint64_t>(member) > UnionFieldIndexMask) {
        error = "concrete specialization union initializer has an invalid "
                "member";
        return std::nullopt;
      }
      value.opcode = GenericTemplateOpcode::UnionInit;
      value.arg0 = inst_arg(old.arg0);
      value.arg1 = static_cast<std::uint32_t>(member);
      break;
    }
    case SemInstKind::EnumInit: {
      const auto variant = sem_ir_.integer(IntegerId(old.arg1));
      if (variant < 0 || static_cast<std::uint64_t>(variant) >
                             std::numeric_limits<std::uint32_t>::max()) {
        error = "concrete specialization has an invalid enum variant";
        return std::nullopt;
      }
      value.opcode = GenericTemplateOpcode::EnumInit;
      for (const auto field : sem_ir_.instBlock(InstBlockId(old.arg0))) {
        const auto mapped = inst_arg(field.index);
        if (mapped == core::AnyId::InvalidIndex) {
          error = "concrete enum payload is not canonical";
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
        artifact.integers.push_back(sem_ir_.integer(IntegerId(old.arg1)));
      value.arg1 = found->second;
      break;
    }
    case SemInstKind::StructFieldAccess: {
      const auto field = sem_ir_.integer(IntegerId(old.arg1));
      if (field < 0 || static_cast<std::uint64_t>(field) >
                           std::numeric_limits<std::uint32_t>::max()) {
        error = "concrete specialization field projection has an invalid index";
        return std::nullopt;
      }
      value.opcode = GenericTemplateOpcode::StructFieldAccess;
      value.arg0 = inst_arg(old.arg0);
      value.arg1 = static_cast<std::uint32_t>(field);
      break;
    }
    case SemInstKind::Index:
      value.opcode = GenericTemplateOpcode::StaticIndex;
      value.arg0 = inst_arg(old.arg0);
      value.arg1 = inst_arg(old.arg1);
      if (value.arg0 == core::AnyId::InvalidIndex ||
          value.arg1 == core::AnyId::InvalidIndex ||
          sem_ir_.inst(InstId(old.arg1)).kind != SemInstKind::IntegerLiteral) {
        error = "concrete specialization index projection is not static";
        return std::nullopt;
      }
      break;
    case SemInstKind::UnionFieldAccess: {
      const auto encoded_field = sem_ir_.integer(IntegerId(old.arg1));
      if (encoded_field == std::numeric_limits<std::int64_t>::min()) {
        error = "concrete specialization union projection has an invalid "
                "index";
        return std::nullopt;
      }
      const auto unsafe = encoded_field < 0;
      const auto field = unsafe ? -encoded_field - 1 : encoded_field;
      if (field < 0 ||
          static_cast<std::uint64_t>(field) >= UnionFieldIndexMask) {
        error = "concrete specialization union projection has an invalid "
                "index";
        return std::nullopt;
      }
      value.opcode = GenericTemplateOpcode::UnionFieldAccess;
      value.arg0 = inst_arg(old.arg0);
      value.arg1 = static_cast<std::uint32_t>(field) |
                   (unsafe ? UnionFieldUnsafeBit : 0U);
      break;
    }
    default:
      error = "concrete specialization contains an unsupported instruction";
      return std::nullopt;
    }
    artifact.body.instructions.push_back(value);
    artifact.body.instruction_value_blocks.push_back(
        std::move(instruction_values));
  }
  for (const auto block : blocks) {
    std::vector<std::uint32_t> result;
    for (const auto inst : sem_ir_.instBlock(block)) {
      const auto found = inst_map.find(inst.index);
      if (found == inst_map.end()) {
        error = "concrete specialization block references an instruction "
                "outside its canonical body";
        return std::nullopt;
      }
      result.push_back(found->second);
    }
    artifact.body.blocks.push_back(std::move(result));
  }
  artifact.body.entry_block = 0;
  return artifact;
}
