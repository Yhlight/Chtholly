// Included by IncrementalDependenciesCodecInternal.h; this fragment keeps
// canonical package-manifest codec helpers in the private namespace.
void appendSemanticContract(std::string &out,
                            const CallableSemanticContract &contract) {
  out.push_back(static_cast<char>(contract.domain));
  out.push_back(static_cast<char>(contract.role));
  appendU32(out, contract.capability);
  appendPublicType(out, contract.owner);
  appendU32(out, contract.projector_field);
  out.push_back(contract.whole_carrier ? 1 : 0);
  appendU32(out, static_cast<std::uint32_t>(contract.carrier_path.size()));
  for (const auto field : contract.carrier_path)
    appendU32(out, field);
  out.push_back(contract.has_bit_range ? 1 : 0);
  appendU32(out, contract.bit_begin);
  appendU32(out, contract.bit_end);
}

bool readSemanticContract(StateReader &reader,
                          CallableSemanticContract &contract,
                          std::uint32_t generic_parameter_count) {
  std::uint8_t domain = 0, role = 0, whole = 0, has_bits = 0;
  std::uint32_t capability = 0, path_count = 0;
  if (!reader.readU8(domain) || !reader.readU8(role) ||
      !reader.readU32(capability) || capability > 0xffffU ||
      !readPublicType(reader, contract.owner) ||
      !reader.readU32(contract.projector_field) || !reader.readU8(whole) ||
      whole > 1 || !reader.readU32(path_count) || path_count > 256 ||
      !reader.readRecords(path_count, 4))
    return false;
  contract.domain = static_cast<CallableSemanticDomain>(domain);
  contract.role = static_cast<CallableSemanticRole>(role);
  contract.capability = static_cast<std::uint16_t>(capability);
  contract.whole_carrier = whole != 0;
  contract.carrier_path.resize(path_count);
  for (auto &field : contract.carrier_path)
    if (!reader.readU32(field))
      return false;
  if (!reader.readU8(has_bits) || has_bits > 1 ||
      !reader.readU32(contract.bit_begin) || !reader.readU32(contract.bit_end))
    return false;
  contract.has_bit_range = has_bits != 0;
  std::string error;
  return contract.verify(generic_parameter_count, error);
}

void appendTemplate(std::string &out,
                    const GenericTemplateArtifact &generic_template) {
  appendU32(out, generic_template.generic_parameter_count);
  appendU32(out, generic_template.parameter_count);
  appendU32(out,
            static_cast<std::uint32_t>(generic_template.local_types.size()));
  for (const auto type : generic_template.local_types)
    appendPublicType(out, type);
  for (const auto flags : generic_template.local_flags)
    appendU32(out, flags);
  appendU32(out, static_cast<std::uint32_t>(generic_template.integers.size()));
  for (const auto value : generic_template.integers) {
    const auto bits = static_cast<std::uint64_t>(value);
    appendU32(out, static_cast<std::uint32_t>(bits));
    appendU32(out, static_cast<std::uint32_t>(bits >> 32U));
  }
  appendU32(out, static_cast<std::uint32_t>(generic_template.strings.size()));
  for (const auto &value : generic_template.strings)
    appendField(out, value);
  appendU32(out,
            static_cast<std::uint32_t>(generic_template.type_queries.size()));
  for (const auto &query : generic_template.type_queries) {
    out.push_back(static_cast<char>(query.kind));
    appendPublicType(out, query.source);
    appendPublicType(out, query.other);
    appendField(out, query.property);
  }
  appendU32(out, static_cast<std::uint32_t>(generic_template.callees.size()));
  for (const auto &callee : generic_template.callees) {
    appendPublicEntity(out, callee);
  }
  appendU32(out, static_cast<std::uint32_t>(
                     generic_template.callee_type_arguments.size()));
  for (const auto &arguments : generic_template.callee_type_arguments) {
    appendU32(out, static_cast<std::uint32_t>(arguments.size()));
    for (const auto &argument : arguments)
      appendPublicType(out, argument);
  }
  const auto append_region =
      [&](const GenericEvaluationRegionArtifact &region) {
        appendU32(out, region.entry_block);
        appendU32(out, static_cast<std::uint32_t>(region.results.size()));
        for (const auto type : region.results)
          appendPublicType(out, type);
        appendU32(out, static_cast<std::uint32_t>(region.instructions.size()));
        for (const auto &instruction : region.instructions) {
          out.push_back(static_cast<char>(instruction.opcode));
          appendPublicType(out, instruction.type);
          appendU32(out, instruction.arg0);
          appendU32(out, instruction.arg1);
        }
        const auto append_blocks = [&](const auto &blocks) {
          appendU32(out, static_cast<std::uint32_t>(blocks.size()));
          for (const auto &block : blocks) {
            appendU32(out, static_cast<std::uint32_t>(block.size()));
            for (const auto instruction : block)
              appendU32(out, instruction);
          }
        };
        append_blocks(region.blocks);
        append_blocks(region.instruction_value_blocks);
      };
  append_region(generic_template.declaration);
  append_region(generic_template.definition);
}

bool readTemplateRegion(StateReader &reader,
                        GenericEvaluationRegionArtifact &region) {
  std::uint32_t result_count = 0;
  std::uint32_t instruction_count = 0;
  if (!reader.readU32(region.entry_block) || !reader.readU32(result_count) ||
      !reader.readRecords(result_count, 5))
    return false;
  region.results.resize(result_count);
  for (auto &type : region.results) {
    if (!readPublicType(reader, type))
      return false;
  }
  if (!reader.readU32(instruction_count) ||
      !reader.readRecords(instruction_count, 14))
    return false;
  region.instructions.resize(instruction_count);
  for (auto &instruction : region.instructions) {
    std::uint8_t opcode = 0;
    if (!reader.readU8(opcode) || !readPublicType(reader, instruction.type) ||
        !reader.readU32(instruction.arg0) || !reader.readU32(instruction.arg1))
      return false;
    instruction.opcode = static_cast<GenericTemplateOpcode>(opcode);
  }
  const auto read_blocks = [&](auto &blocks) {
    std::uint32_t block_count = 0;
    if (!reader.readU32(block_count) || !reader.readRecords(block_count, 4))
      return false;
    blocks.resize(block_count);
    for (auto &block : blocks) {
      std::uint32_t count = 0;
      if (!reader.readU32(count) || !reader.readRecords(count, 4))
        return false;
      block.resize(count);
      for (auto &instruction : block) {
        if (!reader.readU32(instruction))
          return false;
      }
    }
    return true;
  };
  return read_blocks(region.blocks) &&
         read_blocks(region.instruction_value_blocks);
}

bool readTemplate(StateReader &reader,
                  GenericTemplateArtifact &generic_template) {
  std::uint32_t local_count = 0;
  std::uint32_t integer_count = 0;
  std::uint32_t string_count = 0;
  std::uint32_t query_count = 0;
  std::uint32_t callee_count = 0;
  if (!reader.readU32(generic_template.generic_parameter_count) ||
      !reader.readU32(generic_template.parameter_count) ||
      !reader.readU32(local_count) || !reader.readRecords(local_count, 5))
    return false;
  generic_template.local_types.resize(local_count);
  for (auto &type : generic_template.local_types) {
    if (!readPublicType(reader, type))
      return false;
  }
  if (!reader.readRecords(local_count, 4))
    return false;
  generic_template.local_flags.resize(local_count);
  for (auto &flags : generic_template.local_flags)
    if (!reader.readU32(flags))
      return false;
  if (!reader.readU32(integer_count) || !reader.readRecords(integer_count, 8))
    return false;
  generic_template.integers.reserve(integer_count);
  for (std::uint32_t index = 0; index < integer_count; ++index) {
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    if (!reader.readU32(low) || !reader.readU32(high))
      return false;
    const auto bits = static_cast<std::uint64_t>(low) |
                      (static_cast<std::uint64_t>(high) << 32U);
    generic_template.integers.push_back(std::bit_cast<std::int64_t>(bits));
  }
  if (!reader.readU32(string_count) || !reader.readRecords(string_count, 4))
    return false;
  generic_template.strings.resize(string_count);
  for (auto &value : generic_template.strings)
    if (!reader.readString(value))
      return false;
  if (!reader.readU32(query_count) || !reader.readRecords(query_count, 15))
    return false;
  generic_template.type_queries.resize(query_count);
  for (auto &query : generic_template.type_queries) {
    std::uint8_t kind = 0;
    if (!reader.readU8(kind) ||
        kind >= static_cast<std::uint8_t>(GenericTypeQueryKind::Count) ||
        !readPublicType(reader, query.source) ||
        !readPublicType(reader, query.other) ||
        !reader.readString(query.property))
      return false;
    query.kind = static_cast<GenericTypeQueryKind>(kind);
  }
  if (!reader.readU32(callee_count) || !reader.readRecords(callee_count, 12))
    return false;
  generic_template.callees.resize(callee_count);
  for (auto &callee : generic_template.callees) {
    if (!readPublicEntity(reader, callee))
      return false;
  }
  std::uint32_t argument_block_count = 0;
  if (!reader.readU32(argument_block_count) ||
      argument_block_count != callee_count ||
      !reader.readRecords(argument_block_count, 4))
    return false;
  generic_template.callee_type_arguments.resize(argument_block_count);
  for (auto &arguments : generic_template.callee_type_arguments) {
    std::uint32_t argument_count = 0;
    if (!reader.readU32(argument_count) ||
        !reader.readRecords(argument_count, 5))
      return false;
    arguments.resize(argument_count);
    for (auto &argument : arguments)
      if (!readPublicType(reader, argument))
        return false;
  }
  if (!readTemplateRegion(reader, generic_template.declaration) ||
      !readTemplateRegion(reader, generic_template.definition))
    return false;
  std::string error;
  return generic_template.verify(error);
}

bool readFunction(StateReader &reader, PublicFunctionArtifact &function) {
  std::uint32_t parameter_count = 0;
  std::uint8_t has_member_owner = 0;
  if (!reader.readString(function.name) || !reader.readU8(has_member_owner) ||
      has_member_owner > 1)
    return false;
  if (has_member_owner != 0) {
    function.member_owner.emplace();
    if (!readPublicEntity(reader, *function.member_owner))
      return false;
  }
  std::uint8_t member_kind = 0;
  if (!reader.readU8(member_kind) ||
      member_kind >=
          static_cast<std::uint8_t>(PublicFunctionArtifact::MemberKind::Count))
    return false;
  function.member_kind =
      static_cast<PublicFunctionArtifact::MemberKind>(member_kind);
  if (!reader.readString(function.canonical_package) ||
      !reader.readString(function.canonical_module) ||
      !reader.readString(function.canonical_name) ||
      !reader.readU32(function.generic_parameter_count) ||
      !reader.readU32(parameter_count) ||
      !reader.readRecords(parameter_count, 1))
    return false;
  function.parameters.reserve(parameter_count);
  for (std::uint32_t index = 0; index < parameter_count; ++index) {
    PublicType type;
    if (!readPublicType(reader, type))
      return false;
    function.parameters.push_back(type);
  }
  std::uint32_t parameter_name_count = 0;
  if (!reader.readU32(parameter_name_count) ||
      parameter_name_count != parameter_count ||
      !reader.readRecords(parameter_name_count, 4))
    return false;
  function.parameter_names.resize(parameter_name_count);
  for (auto &name : function.parameter_names)
    if (!reader.readString(name))
      return false;
  std::uint32_t default_argument_count = 0;
  if (!reader.readU32(default_argument_count) ||
      default_argument_count != parameter_count ||
      !reader.readRecords(default_argument_count, 1))
    return false;
  function.default_arguments.resize(default_argument_count);
  for (auto &argument : function.default_arguments) {
    std::uint8_t has_argument = 0;
    if (!reader.readU8(has_argument) || has_argument > 1)
      return false;
    if (has_argument != 0) {
      argument.emplace();
      if (!readConstantValue(reader, *argument))
        return false;
    }
  }
  std::uint8_t has_template = 0;
  std::uint8_t execution_kind = 0;
  std::uint8_t constructor_flags = 0;
  std::uint8_t nominal_constructor_kind = 0;
  std::uint8_t has_error = 0;
  std::uint8_t declaration_kind = 0;
  std::uint8_t intrinsic_role = 0;
  std::uint8_t is_unsafe = 0;
  std::uint8_t is_const = 0;
  if (!readPublicType(reader, function.return_type) ||
      !reader.readU8(execution_kind) ||
      execution_kind >=
          static_cast<std::uint8_t>(PublicFunctionExecutionKind::Count) ||
      !reader.readU32(function.coroutine_constructor.epoch) ||
      !reader.readU8(constructor_flags) || (constructor_flags & ~0x0fU) != 0 ||
      !reader.readU32(function.nominal_constructor.epoch) ||
      !reader.readU8(nominal_constructor_kind) ||
      nominal_constructor_kind >=
          static_cast<std::uint8_t>(
              PublicNominalConstructorResultKind::Count) ||
      !reader.readU8(has_error) || has_error > 1)
    return false;
  function.coroutine_constructor.eager_start = (constructor_flags & 0x01U) != 0;
  function.coroutine_constructor.left_to_right_exactly_once =
      (constructor_flags & 0x02U) != 0;
  function.coroutine_constructor.supports_root =
      (constructor_flags & 0x04U) != 0;
  function.coroutine_constructor.supports_child =
      (constructor_flags & 0x08U) != 0;
  function.nominal_constructor.result_kind =
      static_cast<PublicNominalConstructorResultKind>(nominal_constructor_kind);
  if (has_error != 0) {
    function.error_type = PublicType{};
    if (!readPublicType(reader, *function.error_type))
      return false;
  }
  if (!readSemanticContract(reader, function.semantic_contract,
                            function.generic_parameter_count) ||
      !reader.readU8(intrinsic_role) ||
      intrinsic_role >=
          static_cast<std::uint8_t>(CompilerIntrinsicRole::Count) ||
      !readOwnershipSummary(reader, function.ownership_summary,
                            parameter_count) ||
      !reader.readU8(declaration_kind) || !reader.readU8(is_unsafe) ||
      !reader.readU8(is_const) ||
      declaration_kind >=
          static_cast<std::uint8_t>(PublicCallableDeclarationKind::Count) ||
      is_unsafe > 1 || is_const > 1 ||
      !reader.readString(function.foreign_abi) ||
      !reader.readString(function.external_symbol) ||
      !readForeignSignature(reader, function.foreign_signature) ||
      !readInteropArtifactReference(reader, function.interop_artifact) ||
      !reader.readU8(has_template) || has_template > 1)
    return false;
  function.execution_kind =
      static_cast<PublicFunctionExecutionKind>(execution_kind);
  function.intrinsic_role = static_cast<CompilerIntrinsicRole>(intrinsic_role);
  function.declaration_kind =
      static_cast<PublicCallableDeclarationKind>(declaration_kind);
  function.is_unsafe = is_unsafe != 0;
  function.is_const = is_const != 0;
  if (has_template != 0) {
    function.generic_template.emplace();
    if (!readTemplate(reader, *function.generic_template))
      return false;
  }
  std::uint32_t constraint_count = 0;
  if (!reader.readU32(constraint_count) ||
      !reader.readRecords(constraint_count, 10))
    return false;
  function.constraints.resize(constraint_count);
  for (auto &constraint : function.constraints)
    if (!readInterfaceConstraint(reader, constraint))
      return false;
  if (!reader.readFingerprint(function.entity_fingerprint))
    return false;
  return true;
}

bool readInterface(StateReader &reader, PublicInterfaceArtifact &interface) {
  std::string package_name;
  std::string module_name;
  StableFingerprint fingerprint;
  std::uint32_t function_count = 0;
  if (!reader.readString(package_name) || !reader.readString(module_name) ||
      !reader.readFingerprint(fingerprint) || !reader.readU32(function_count) ||
      !reader.readRecords(function_count, MinimumFunctionEncodingSize))
    return false;
  std::vector<PublicFunctionArtifact> functions(function_count);
  for (auto &function : functions) {
    if (!readFunction(reader, function))
      return false;
  }
  std::uint32_t nominal_count = 0;
  if (!reader.readU32(nominal_count) || !reader.readRecords(nominal_count, 4))
    return false;
  std::vector<PublicNominalTypeArtifact> nominal_types;
  nominal_types.reserve(nominal_count);
  for (std::uint32_t index = 0; index < nominal_count; ++index) {
    std::string encoded;
    if (!reader.readString(encoded))
      return false;
    std::string nominal_error;
    auto nominal = internal::decodePublicNominalTypeArtifact(
        encoded, nominal_error, reader.context());
    if (!nominal)
      return false;
    nominal_types.push_back(std::move(*nominal));
  }
  std::uint32_t value_count = 0;
  if (!reader.readU32(value_count) || !reader.readRecords(value_count, 22))
    return false;
  std::vector<PublicValueArtifact> values(value_count);
  for (auto &value : values) {
    std::uint8_t kind = 0;
    if (!reader.readU8(kind) ||
        kind >= static_cast<std::uint8_t>(PublicValueKind::Count) ||
        !reader.readString(value.name) ||
        !reader.readString(value.canonical_package) ||
        !reader.readString(value.canonical_module) ||
        !reader.readString(value.canonical_name) ||
        !readPublicType(reader, value.type) ||
        !readConstantValue(reader, value.value) ||
        !reader.readFingerprint(value.entity_fingerprint))
      return false;
    value.kind = static_cast<PublicValueKind>(kind);
  }
  std::uint32_t interface_count = 0;
  if (!reader.readU32(interface_count) ||
      !reader.readRecords(interface_count, 16))
    return false;
  std::vector<PublicInterfaceDeclarationArtifact> interfaces(interface_count);
  for (auto &declaration : interfaces)
    if (!readInterfaceDeclaration(reader, declaration))
      return false;
  std::uint32_t alias_count = 0;
  if (!reader.readU32(alias_count) || !reader.readRecords(alias_count, 14))
    return false;
  std::vector<PublicTypeAliasArtifact> aliases(alias_count);
  for (auto &alias : aliases)
    if (!readTypeAlias(reader, alias))
      return false;
  std::uint32_t witness_count = 0;
  if (!reader.readU32(witness_count) ||
      !reader.readRecords(witness_count, StableFingerprint::ByteCount + 10))
    return false;
  std::vector<PublicInterfaceWitnessArtifact> witnesses(witness_count);
  for (auto &witness : witnesses)
    if (!readInterfaceWitness(reader, witness))
      return false;
  interface = PublicInterfaceArtifact(
      std::move(package_name), std::move(module_name), fingerprint,
      std::move(functions), std::move(nominal_types), std::move(values),
      std::move(interfaces), std::move(aliases), std::move(witnesses));
  return true;
}

bool readObservation(StateReader &reader, DependencyObservation &observation) {
  std::uint8_t kind = 0;
  std::uint8_t role = 0;
  if (!reader.readU8(kind) ||
      !reader.readString(observation.provider.package_name) ||
      !reader.readString(observation.provider.module_name) ||
      !reader.readString(observation.binding_name) ||
      !reader.readFingerprint(observation.expected_fingerprint) ||
      !reader.readString(observation.canonical_provider.package_name) ||
      !reader.readString(observation.canonical_provider.module_name) ||
      !reader.readString(observation.canonical_name) || !reader.readU8(role) ||
      !reader.readFingerprint(observation.witness_fingerprint) ||
      !reader.readFingerprint(observation.specific_closure_fingerprint))
    return false;
  observation.kind = static_cast<DependencyObservationKind>(kind);
  observation.lifecycle_role = static_cast<LifecycleObservationRole>(role);
  return true;
}
