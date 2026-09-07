// Included by ConcreteSpecializationSupportInternal.h; this fragment keeps
// concrete-specialization codec and verification state in the private scope.
bool readRegion(Reader &reader, GenericEvaluationRegionArtifact &region) {
  std::uint32_t count = 0;
  if (!reader.u32(region.entry_block) || !readCount(reader, count, 5))
    return false;
  region.results.resize(count);
  for (auto &type : region.results)
    if (!reader.type(type))
      return false;
  if (!readCount(reader, count, 14))
    return false;
  region.instructions.resize(count);
  for (auto &instruction : region.instructions) {
    std::uint8_t opcode = 0;
    if (!reader.u8(opcode) || !reader.type(instruction.type) ||
        !reader.u32(instruction.arg0) || !reader.u32(instruction.arg1))
      return false;
    instruction.opcode = static_cast<GenericTemplateOpcode>(opcode);
  }
  const auto read_blocks = [&](auto &blocks) {
    std::uint32_t block_count = 0;
    if (!readCount(reader, block_count, 4))
      return false;
    blocks.resize(block_count);
    for (auto &block : blocks) {
      std::uint32_t item_count = 0;
      if (!readCount(reader, item_count, 4))
        return false;
      block.resize(item_count);
      for (auto &item : block)
        if (!reader.u32(item))
          return false;
    }
    return true;
  };
  return read_blocks(region.blocks) &&
         read_blocks(region.instruction_value_blocks);
}

bool verifyBody(const ConcreteSpecificNodeArtifact &node, std::string &error) {
  const auto &region = node.body;
  if (node.parameter_count > node.local_types.size() ||
      node.local_flags.size() != node.local_types.size() ||
      !node.semantic_contract.verify(0, error) ||
      region.results.size() != node.parameter_count + 1 ||
      region.entry_block >= region.blocks.size() ||
      region.instruction_value_blocks.size() != region.instructions.size() ||
      !std::equal(region.results.begin(),
                  region.results.begin() + node.parameter_count,
                  node.local_types.begin()) ||
      !semanticContractMatchesSignature(
          std::span(node.local_types).first(node.parameter_count),
          region.results.back(), node.semantic_contract) ||
      !semanticContractMatchesEffects(node.semantic_contract,
                                      node.ownership_summary)) {
    error = "concrete specialization node has an invalid signature";
    return false;
  }
  if (std::ranges::any_of(node.local_flags, [](std::uint32_t flags) {
        return (flags & ~0x7U) != 0;
      })) {
    error = "concrete specialization node has invalid local flags";
    return false;
  }
  if (!node.ownership_summary.verify(node.parameter_count, error))
    return false;
  const auto independently_valid_region = [&](const OwnershipRegion &region,
                                              bool *slice_element = nullptr) {
    if (slice_element)
      *slice_element = false;
    PublicType current = node.local_types[region.parameter_index];
    if (current.kind == PublicTypeKind::Reference) {
      auto pointee = current.arguments.front();
      current = std::move(pointee);
    }
    for (const auto &step : region.path) {
      if (step.kind == OwnershipRegionStepKind::Dereference) {
        if (step.index != 0 ||
            (current.kind != PublicTypeKind::Reference &&
             current.kind != PublicTypeKind::RawPointer) ||
            current.arguments.size() != 1)
          return false;
        auto pointee = current.arguments.front();
        current = std::move(pointee);
        continue;
      } else if (step.kind == OwnershipRegionStepKind::Field) {
        // Nominal field definitions live in the public registry. The loader
        // performs the complete check once that closure is available.
        return current.kind == PublicTypeKind::Nominal;
      }
      if ((current.kind != PublicTypeKind::Array &&
           current.kind != PublicTypeKind::Tuple &&
           current.kind != PublicTypeKind::Slice) ||
          (current.kind == PublicTypeKind::Tuple &&
           step.kind == OwnershipRegionStepKind::AnyElement) ||
          (current.kind == PublicTypeKind::Slice &&
           step.kind != OwnershipRegionStepKind::AnyElement) ||
          (current.kind == PublicTypeKind::Array &&
           step.kind == OwnershipRegionStepKind::StaticElement &&
           step.index >= current.array_bound) ||
          (current.kind == PublicTypeKind::Tuple &&
           step.index >= current.arguments.size()))
        return false;
      const bool is_slice_element = current.kind == PublicTypeKind::Slice;
      auto element = current.kind == PublicTypeKind::Array
                         ? current.arguments.front()
                         : current.arguments[step.index];
      current = std::move(element);
      if (slice_element)
        *slice_element = is_slice_element;
    }
    return !region.has_bit_range;
  };
  if (std::ranges::any_of(
          node.ownership_summary.effects,
          [&](const auto &effect) {
            bool slice_element = false;
            return !independently_valid_region(effect.region, &slice_element) ||
                   (slice_element &&
                    effect.kind == CallableEffectKind::Initialize);
          }) ||
      std::ranges::any_of(
          node.ownership_summary.postconditions,
          [&](const auto &postcondition) {
            bool slice_element = false;
            return !independently_valid_region(postcondition.region,
                                               &slice_element) ||
                   (slice_element &&
                    (postcondition.outcomes & CallableOutcomeInitialize));
          }) ||
      std::ranges::any_of(node.ownership_summary.return_provenance,
                          [&](const auto &source) {
                            return !independently_valid_region(source.region);
                          })) {
    error = "concrete specialization ownership summary has a type-invalid "
            "region path";
    return false;
  }
  if (containsBorrowedView(region.results.back()) ==
      node.ownership_summary.returns_owned) {
    error = "concrete specialization ownership summary disagrees with its "
            "return type";
    return false;
  }
  if (!node.ownership_summary.returns_owned &&
      std::ranges::any_of(
          node.ownership_summary.return_provenance,
          [&](const CallableReturnSource &source) {
            if (source.region.parameter_index >= node.parameter_count)
              return true;
            const auto &parameter =
                node.local_types[source.region.parameter_index];
            const auto &return_type = region.results.back();
            const auto return_mutable =
                return_type.kind == PublicTypeKind::Reference
                    ? return_type.reference_mutability ==
                          PublicReferenceMutability::Mutable
                    : return_type.kind == PublicTypeKind::Slice &&
                          return_type.slice_mutable;
            const auto parameter_mutable =
                parameter.kind == PublicTypeKind::Reference
                    ? parameter.reference_mutability ==
                          PublicReferenceMutability::Mutable
                    : parameter.kind == PublicTypeKind::Slice &&
                          parameter.slice_mutable;
            return (parameter.kind != PublicTypeKind::Reference &&
                    parameter.kind != PublicTypeKind::Slice) ||
                   (return_mutable && !parameter_mutable) ||
                   std::ranges::any_of(
                       source.condition.clauses, [&](const auto &clause) {
                         return std::ranges::any_of(
                             clause.atoms, [&](const auto &atom) {
                               return atom.parameter_index >=
                                          node.parameter_count ||
                                      node.local_types[atom.parameter_index]
                                              .kind != (atom.variant == core::AnyId::InvalidIndex ? PublicTypeKind::Bool : PublicTypeKind::Nominal);
                             });
                       });
          })) {
    error = "concrete specialization has invalid borrowed return sources";
    return false;
  }
  for (const auto type : node.local_types)
    if (!concreteType(type, true)) {
      error = "concrete specialization node has a dependent local type";
      return false;
    }
  for (const auto type : region.results)
    if (!concreteType(type, true)) {
      error = "concrete specialization node has a dependent result type";
      return false;
    }
  std::vector<bool> seen(region.instructions.size());
  std::vector<std::uint32_t> instruction_blocks(region.instructions.size(),
                                                core::AnyId::InvalidIndex);
  std::vector<std::uint32_t> instruction_positions(region.instructions.size(),
                                                   core::AnyId::InvalidIndex);
  std::vector<std::vector<std::uint32_t>> successors(region.blocks.size());
  std::vector<std::uint32_t> predecessors(region.blocks.size());
  std::vector<std::uint32_t> parent_blocks(region.blocks.size(),
                                           core::AnyId::InvalidIndex);
  std::vector<std::uint32_t> parent_instructions(region.blocks.size(),
                                                 core::AnyId::InvalidIndex);
  bool duplicate_parent = false;
  const auto add_successor = [&](std::uint32_t block, std::uint32_t instruction,
                                 std::uint32_t successor) {
    successors[block].push_back(successor);
    ++predecessors[successor];
    if (parent_blocks[successor] != core::AnyId::InvalidIndex)
      duplicate_parent = true;
    else {
      parent_blocks[successor] = block;
      parent_instructions[successor] = instruction;
    }
  };
  for (std::uint32_t index = 0; index < region.instructions.size(); ++index) {
    const auto &inst = region.instructions[index];
    const auto &instruction_values = region.instruction_value_blocks[index];
    if (inst.opcode >= GenericTemplateOpcode::Count ||
        !concreteType(inst.type, true)) {
      error = "concrete specialization node has an invalid instruction";
      return false;
    }
    const auto previous = [&](std::uint32_t id) { return id < index; };
    bool valid = false;
    switch (inst.opcode) {
    case GenericTemplateOpcode::Parameter:
      valid = inst.arg0 < node.parameter_count &&
              inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::BorrowPlace:
      valid = previous(inst.arg0) && inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::CarrierView:
      valid = previous(inst.arg0) && inst.arg1 != core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::NameRef:
    case GenericTemplateOpcode::BorrowLocal:
      valid = inst.arg0 < node.local_types.size() &&
              inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::IntegerLiteral:
    case GenericTemplateOpcode::FloatLiteral:
      valid = inst.arg0 < node.integers.size() &&
              inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::CharLiteral:
      valid = inst.arg0 < node.integers.size() &&
              inst.arg1 == core::AnyId::InvalidIndex &&
              inst.type.kind == PublicTypeKind::Char &&
              static_cast<std::uint64_t>(node.integers[inst.arg0]) <=
                  0x10ffffU;
      break;
    case GenericTemplateOpcode::StringLiteral: {
      const auto cstring =
          inst.type.kind == PublicTypeKind::RawPointer &&
          inst.type.pointer_const && inst.type.arguments.size() == 1 &&
          inst.type.arguments.front().kind == PublicTypeKind::Integer &&
          inst.type.arguments.front().scalar_width == 8 &&
          inst.type.arguments.front().integer_signed;
      valid = inst.arg0 < node.strings.size() &&
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
              valid && previous(element) &&
              region.instructions[element].type == inst.type.arguments.front();
      break;
    case GenericTemplateOpcode::NullPointer:
      valid = inst.type.kind == PublicTypeKind::RawPointer &&
              inst.arg0 == core::AnyId::InvalidIndex &&
              inst.arg1 == core::AnyId::InvalidIndex;
      break;
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
          instruction_values.size() == 1 && previous(instruction_values[0]);
      break;
    case GenericTemplateOpcode::BuiltinBinary:
      valid =
          inst.arg0 < static_cast<std::uint32_t>(BuiltinOperatorKind::Count) &&
          !isBuiltinUnaryOperator(
              static_cast<BuiltinOperatorKind>(inst.arg0)) &&
          inst.arg1 == core::AnyId::InvalidIndex &&
          instruction_values.size() == 2 && previous(instruction_values[0]) &&
          previous(instruction_values[1]);
      break;
    case GenericTemplateOpcode::Add:
    case GenericTemplateOpcode::Equal:
      valid = previous(inst.arg0) && previous(inst.arg1);
      break;
    case GenericTemplateOpcode::NumericConvert:
    case GenericTemplateOpcode::CheckedNumericCast:
      valid = previous(inst.arg0) && inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::BindName:
      valid =
          inst.arg0 < node.local_types.size() && previous(inst.arg1) &&
          node.local_types[inst.arg0] == region.instructions[inst.arg1].type;
      break;
    case GenericTemplateOpcode::MaterializeTemporary:
      valid = inst.arg0 < node.local_types.size() && previous(inst.arg1);
      break;
    case GenericTemplateOpcode::ExtendTemporary:
      valid = inst.arg0 < node.local_types.size() && previous(inst.arg1) &&
              region.instructions[inst.arg1].opcode ==
                  GenericTemplateOpcode::MaterializeTemporary;
      break;
    case GenericTemplateOpcode::Assert:
      valid =
          inst.type.kind == PublicTypeKind::Void && previous(inst.arg0) &&
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
        valid = valid && previous(temporary) &&
                boundary_temporaries.insert(temporary).second &&
                region.instructions[temporary].opcode ==
                    GenericTemplateOpcode::MaterializeTemporary;
      }
      break;
    }
    case GenericTemplateOpcode::Assign:
      valid = previous(inst.arg0) && previous(inst.arg1);
      break;
    case GenericTemplateOpcode::Return:
    case GenericTemplateOpcode::Yield:
      valid = previous(inst.arg0) && inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::Defer:
    case GenericTemplateOpcode::ScopedBlock:
      valid = inst.arg0 < region.blocks.size() &&
              inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::If:
      valid = previous(inst.arg0) && inst.arg1 < region.blocks.size();
      break;
    case GenericTemplateOpcode::IfArm:
      valid = inst.arg0 < region.blocks.size() &&
              inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::While:
      valid = inst.arg0 < region.blocks.size() &&
              inst.arg1 < region.blocks.size() &&
              !region.blocks[inst.arg0].empty();
      break;
    case GenericTemplateOpcode::For:
    case GenericTemplateOpcode::DoWhile:
      valid =
          inst.arg0 < region.blocks.size() && inst.arg1 < region.blocks.size();
      break;
    case GenericTemplateOpcode::ForClause:
      valid = inst.arg0 <= 2 && inst.arg1 < region.blocks.size();
      break;
    case GenericTemplateOpcode::Break:
    case GenericTemplateOpcode::Continue:
      valid = inst.arg0 == core::AnyId::InvalidIndex &&
              inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::Switch:
      valid = previous(inst.arg0) && inst.arg1 < region.blocks.size();
      break;
    case GenericTemplateOpcode::SwitchArm:
      valid = inst.arg1 < region.blocks.size();
      break;
    case GenericTemplateOpcode::Call:
    case GenericTemplateOpcode::CompilerIntrinsicCall:
      valid = inst.arg0 < node.callees.size() &&
              inst.arg1 == core::AnyId::InvalidIndex;
      if (valid) {
        const auto &callable = node.callees[inst.arg0].callable_type;
        valid = callable.kind == PublicTypeKind::Function &&
                callable.arguments.size() == instruction_values.size() + 1 &&
                inst.type == callable.arguments.back();
        for (std::size_t argument = 0;
             valid && argument < instruction_values.size(); ++argument)
          valid = previous(instruction_values[argument]) &&
                  region.instructions[instruction_values[argument]].type ==
                      callable.arguments[argument];
      }
      break;
    case GenericTemplateOpcode::InterfaceCall:
      valid = false;
      break;
    case GenericTemplateOpcode::FunctionValue:
      valid = inst.arg0 < node.callees.size() &&
              inst.arg1 == core::AnyId::InvalidIndex &&
              inst.type == node.callees[inst.arg0].callable_type;
      break;
    case GenericTemplateOpcode::BoundMethod:
      valid = inst.arg0 < node.callees.size() && inst.arg1 < 3U &&
              inst.type.kind == PublicTypeKind::Nominal &&
              instruction_values.size() == 1 &&
              previous(instruction_values.front());
      break;
    case GenericTemplateOpcode::Closure:
      valid = inst.arg0 < node.callees.size() &&
              inst.arg1 < static_cast<std::uint32_t>(
                              SemCallableEnvironmentCapability::Count) &&
              inst.type.kind == PublicTypeKind::Nominal;
      if (valid)
        for (const auto capture : instruction_values)
          valid = valid && previous(capture);
      break;
    case GenericTemplateOpcode::IndirectCall:
      valid = previous(inst.arg0) && inst.arg1 == core::AnyId::InvalidIndex;
      if (valid)
        for (const auto argument : instruction_values)
          valid = valid && previous(argument);
      break;
    case GenericTemplateOpcode::AggregateInit:
    case GenericTemplateOpcode::Slice:
    case GenericTemplateOpcode::TupleLiteral:
      valid = inst.arg0 == core::AnyId::InvalidIndex &&
              inst.arg1 == core::AnyId::InvalidIndex;
      if (valid)
        for (const auto field : instruction_values)
          valid = valid && previous(field);
      break;
    case GenericTemplateOpcode::UnionInit:
      valid = previous(inst.arg0) && inst.arg1 < UnionFieldUnsafeBit;
      break;
    case GenericTemplateOpcode::EnumInit:
      valid = inst.arg0 == core::AnyId::InvalidIndex;
      if (valid)
        for (const auto field : instruction_values)
          valid = valid && previous(field);
      break;
    case GenericTemplateOpcode::EnumTag:
      valid = previous(inst.arg0) && inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::EnumPayloadAccess:
      valid = previous(inst.arg0) && inst.arg1 < node.integers.size();
      break;
    case GenericTemplateOpcode::StructFieldAccess:
      valid = previous(inst.arg0);
      break;
    case GenericTemplateOpcode::MemberAccess:
      valid = previous(inst.arg0) && inst.arg1 < node.strings.size();
      break;
    case GenericTemplateOpcode::StaticIndex: {
      valid = previous(inst.arg0) && previous(inst.arg1);
      if (valid) {
        const auto &base = region.instructions[inst.arg0].type;
        const auto &index_inst = region.instructions[inst.arg1];
        valid = base.kind == PublicTypeKind::Array &&
                base.arguments.size() == 1 &&
                index_inst.opcode == GenericTemplateOpcode::IntegerLiteral &&
                index_inst.arg0 < node.integers.size() &&
                node.integers[index_inst.arg0] >= 0 &&
                static_cast<std::uint64_t>(node.integers[index_inst.arg0]) <
                    base.array_bound &&
                inst.type == base.arguments.front();
      }
      break;
    }
    case GenericTemplateOpcode::Index: {
      valid = previous(inst.arg0) && previous(inst.arg1);
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
      valid = previous(inst.arg0) && inst.arg1 != core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::Dereference:
    case GenericTemplateOpcode::Move:
    case GenericTemplateOpcode::Copy:
      valid = previous(inst.arg0) && inst.arg1 == core::AnyId::InvalidIndex;
      break;
    case GenericTemplateOpcode::Count:
      break;
    }
    if (inst.opcode != GenericTemplateOpcode::Call &&
        inst.opcode != GenericTemplateOpcode::CompilerIntrinsicCall &&
        inst.opcode != GenericTemplateOpcode::IndirectCall &&
        inst.opcode != GenericTemplateOpcode::EndFullExpression &&
        inst.opcode != GenericTemplateOpcode::AggregateInit &&
        inst.opcode != GenericTemplateOpcode::Slice &&
        inst.opcode != GenericTemplateOpcode::TupleLiteral &&
        inst.opcode != GenericTemplateOpcode::BoundMethod &&
        inst.opcode != GenericTemplateOpcode::Closure &&
        inst.opcode != GenericTemplateOpcode::ArrayLiteral &&
        inst.opcode != GenericTemplateOpcode::EnumInit &&
        inst.opcode != GenericTemplateOpcode::BuiltinUnary &&
        inst.opcode != GenericTemplateOpcode::BuiltinBinary &&
        !instruction_values.empty())
      valid = false;
    if (!valid) {
      error = "concrete specialization node has invalid instruction operands";
      return false;
    }
  }
  for (std::uint32_t block = 0; block < region.blocks.size(); ++block) {
    for (std::uint32_t position = 0; position < region.blocks[block].size();
         ++position) {
      const auto id = region.blocks[block][position];
      if (id >= region.instructions.size() || seen[id]) {
        error = "concrete specialization node has a non-canonical block";
        return false;
      }
      seen[id] = true;
      instruction_blocks[id] = block;
      instruction_positions[id] = position;
      const auto &inst = region.instructions[id];
      if (inst.opcode == GenericTemplateOpcode::If) {
        add_successor(block, id, inst.arg1);
      } else if (inst.opcode == GenericTemplateOpcode::IfArm) {
        add_successor(block, id, inst.arg0);
      } else if (inst.opcode == GenericTemplateOpcode::While) {
        add_successor(block, id, inst.arg0);
        add_successor(block, id, inst.arg1);
      } else if (inst.opcode == GenericTemplateOpcode::For ||
                 inst.opcode == GenericTemplateOpcode::DoWhile) {
        add_successor(block, id, inst.arg0);
        add_successor(block, id, inst.arg1);
      } else if (inst.opcode == GenericTemplateOpcode::ForClause) {
        add_successor(block, id, inst.arg1);
      } else if (inst.opcode == GenericTemplateOpcode::Defer ||
                 inst.opcode == GenericTemplateOpcode::ScopedBlock) {
        add_successor(block, id, inst.arg0);
      } else if (inst.opcode == GenericTemplateOpcode::Switch ||
                 inst.opcode == GenericTemplateOpcode::SwitchArm) {
        add_successor(block, id, inst.arg1);
      }
    }
  }
  if (std::ranges::any_of(seen, [](bool value) { return !value; })) {
    error = "concrete specialization node has unreachable instructions";
    return false;
  }
  {
    auto remaining_predecessors = predecessors;
    std::vector<std::uint32_t> roots;
    for (std::uint32_t block = 0; block < region.blocks.size(); ++block)
      if (remaining_predecessors[block] == 0)
        roots.push_back(block);
    std::size_t visited = 0;
    for (std::size_t index = 0; index < roots.size(); ++index) {
      ++visited;
      for (const auto successor : successors[roots[index]])
        if (--remaining_predecessors[successor] == 0)
          roots.push_back(successor);
    }
    if (visited != region.blocks.size()) {
      error = "concrete specialization node has cyclic control flow";
      return false;
    }
  }
  bool invalid_ownership = predecessors[region.entry_block] != 0;
  for (std::uint32_t block = 0; block < region.blocks.size(); ++block)
    invalid_ownership |=
        block != region.entry_block && predecessors[block] != 1;
  if (duplicate_parent || invalid_ownership) {
    error = "concrete specialization has non-canonical region ownership";
    return false;
  }
  for (std::uint32_t instruction = 0; instruction < region.instructions.size();
       ++instruction) {
    const auto opcode = region.instructions[instruction].opcode;
    bool nested_loop = false;
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
             !nested_loop)) {
          error = "concrete specialization defer body has invalid control flow";
          return false;
        }
        break;
      }
      if ((parent.opcode == GenericTemplateOpcode::While ||
           parent.opcode == GenericTemplateOpcode::For ||
           parent.opcode == GenericTemplateOpcode::DoWhile) &&
          parent.arg1 == block)
        nested_loop = true;
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
    switch (instruction.opcode) {
    case GenericTemplateOpcode::Add:
    case GenericTemplateOpcode::Equal:
    case GenericTemplateOpcode::Assign:
      operands.push_back(instruction.arg0);
      operands.push_back(instruction.arg1);
      break;
    case GenericTemplateOpcode::BindName:
      operands.push_back(instruction.arg1);
      break;
    case GenericTemplateOpcode::MaterializeTemporary:
    case GenericTemplateOpcode::ExtendTemporary:
      operands.push_back(instruction.arg1);
      break;
    case GenericTemplateOpcode::IndirectCall:
      operands.push_back(instruction.arg0);
      operands.insert(operands.end(),
                      region.instruction_value_blocks[consumer].begin(),
                      region.instruction_value_blocks[consumer].end());
      break;
    case GenericTemplateOpcode::FunctionValue:
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
      operands.push_back(instruction.arg0);
      break;
    case GenericTemplateOpcode::StaticIndex:
    case GenericTemplateOpcode::Index:
      operands.push_back(instruction.arg0);
      operands.push_back(instruction.arg1);
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
      error = "concrete specialization operand is not dominated by its "
              "definition";
      return false;
    }
  std::vector<bool> reachable(region.blocks.size());
  std::vector<std::uint32_t> pending{region.entry_block};
  reachable[region.entry_block] = true;
  for (std::size_t index = 0; index < pending.size(); ++index)
    for (const auto successor : successors[pending[index]])
      if (!reachable[successor]) {
        reachable[successor] = true;
        pending.push_back(successor);
      }
  if (std::ranges::any_of(reachable, [](bool value) { return !value; })) {
    error = "concrete specialization node has unreachable blocks";
    return false;
  }
  return true;
}
