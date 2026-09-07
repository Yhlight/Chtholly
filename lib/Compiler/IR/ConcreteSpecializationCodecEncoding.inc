// Included by ConcreteSpecializationSupportInternal.h; this fragment keeps
// concrete-specialization codec and verification state in the private scope.
// Conditional ownership postconditions add a predicate to each postcondition
// record. Persisted components from the previous layout must fail closed.
constexpr std::uint32_t ComponentVersion =
    ConcreteSpecializationComponentFormat;
constexpr std::uint32_t MaxComponentNodes = 4096;
constexpr std::uint32_t MaxTypedChannelDescriptors = 256;
constexpr std::uint32_t UnionFieldUnsafeBit = 1U << 31U;
constexpr std::size_t MinimumCalleeEncodingSize =
    1 + 4 + StableFingerprint::ByteCount * 3 + 4 * 4 + 1;

void appendU32(std::string &out, std::uint32_t value) {
  for (std::uint32_t shift = 0; shift != 32; shift += 8)
    out.push_back(static_cast<char>((value >> shift) & 0xffU));
}

void appendField(std::string &out, std::string_view value) {
  assert(value.size() <= std::numeric_limits<std::uint32_t>::max());
  appendU32(out, static_cast<std::uint32_t>(value.size()));
  out.append(value);
}

void appendFingerprint(std::string &out, const StableFingerprint &value) {
  for (const auto byte : value.bytes())
    out.push_back(static_cast<char>(byte));
}

void appendEntity(std::string &out,
                  const PublicEntityReferenceArtifact &entity);
void appendOwnershipSummary(std::string &out,
                            const CallableOwnershipSummary &summary);
class Reader;
bool readOwnershipSummary(Reader &reader, CallableOwnershipSummary &summary,
                          std::uint32_t parameter_count);

void appendType(std::string &out, const PublicType &type) {
  out.push_back(static_cast<char>(type.kind));
  appendU32(out, type.binding_index);
  if (type.kind == PublicTypeKind::Integer) {
    appendU32(out, type.scalar_width);
    out.push_back(type.integer_signed ? 1 : 0);
  } else if (type.kind == PublicTypeKind::Float) {
    appendU32(out, type.scalar_width);
  } else if (type.kind == PublicTypeKind::Nominal) {
    appendEntity(out, type.nominal_entity);
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
  } else if (type.kind == PublicTypeKind::Reference) {
    out.push_back(static_cast<char>(type.reference_mutability));
    out.push_back(static_cast<char>(type.reference_provenance.kind));
    appendU32(out, type.reference_provenance.index);
    assert(type.arguments.size() == 1);
    appendType(out, type.arguments.front());
  } else if (type.kind == PublicTypeKind::Array) {
    appendU32(out, type.array_bound);
    assert(type.arguments.size() == 1);
    appendType(out, type.arguments.front());
  } else if (type.kind == PublicTypeKind::Tuple) {
    out.push_back(type.abi_union ? 1 : 0);
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &element : type.arguments)
      appendType(out, element);
  } else if (type.kind == PublicTypeKind::Slice) {
    out.push_back(type.slice_mutable ? 1 : 0);
    assert(type.arguments.size() == 1);
    appendType(out, type.arguments.front());
  } else if (type.kind == PublicTypeKind::TypeProjection) {
    out.push_back(static_cast<char>(type.projection_kind));
    appendU32(out, type.projection_index);
    if (type.projection_kind == PublicTypeProjectionKind::Associated)
      appendEntity(out, type.nominal_entity);
    assert(type.arguments.size() == 1);
    appendType(out, type.arguments.front());
  } else if (type.kind == PublicTypeKind::RawPointer) {
    out.push_back(type.pointer_const ? 1 : 0);
    assert(type.arguments.size() == 1);
    appendType(out, type.arguments.front());
  } else if (type.kind == PublicTypeKind::Function) {
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
  } else if (type.kind == PublicTypeKind::CFunctionPointer) {
    out.push_back(type.callable_variadic ? 1 : 0);
    out.push_back(static_cast<char>(type.foreign_calling_convention));
    appendU32(out, type.callable_context_parameter);
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
    appendOwnershipSummary(out, type.callable_contract);
  } else if (type.kind == PublicTypeKind::CallbackAdapter) {
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
  } else if (type.kind == PublicTypeKind::CallbackRegistration) {
    out.push_back(static_cast<char>(type.registration_authority));
    appendU32(out, type.registration_entry_parameter);
    appendU32(out, type.registration_userdata_parameter);
    appendU32(out, type.registration_release_parameter);
    appendU32(out,
              static_cast<std::uint32_t>(type.registration_bindings.size()));
    for (const auto &binding : type.registration_bindings) {
      appendField(out, binding.name);
      appendU32(out, binding.parameter_index);
    }
    for (const auto parameter : type.registration_arm_parameters)
      appendU32(out, parameter);
    for (const auto parameter : type.registration_detach_parameters)
      appendU32(out, parameter);
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
    appendField(out,
                encodeForeignResourceProtocol(type.foreign_resource_protocol));
  } else if (type.kind == PublicTypeKind::CallbackCompletion) {
    out.push_back(static_cast<char>(type.registration_authority));
    for (const auto parameter : type.registration_arm_parameters)
      appendU32(out, parameter);
    for (const auto parameter : type.registration_detach_parameters)
      appendU32(out, parameter);
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
    appendField(out,
                encodeForeignResourceProtocol(type.foreign_resource_protocol));
  } else if (type.kind == PublicTypeKind::CallbackWake) {
    appendU32(out, static_cast<std::uint32_t>(type.arguments.size()));
    for (const auto &argument : type.arguments)
      appendType(out, argument);
  } else if (type.kind == PublicTypeKind::ForeignCompletion ||
             type.kind == PublicTypeKind::ForeignWake) {
    appendEntity(out, type.nominal_entity);
  } else if (type.kind == PublicTypeKind::ForeignOperationState) {
    appendEntity(out, type.nominal_entity);
    out.push_back(static_cast<char>(type.foreign_operation_state));
  }
}

void appendEntity(std::string &out,
                  const PublicEntityReferenceArtifact &entity) {
  out.push_back(static_cast<char>(entity.kind));
  appendField(out, entity.canonical_package);
  appendField(out, entity.canonical_module);
  appendField(out, entity.canonical_name);
  appendFingerprint(out, entity.expected_fingerprint);
}

void appendSemanticContract(std::string &out,
                            const CallableSemanticContract &contract) {
  out.push_back(static_cast<char>(contract.domain));
  out.push_back(static_cast<char>(contract.role));
  appendU32(out, contract.capability);
  appendType(out, contract.owner);
  appendU32(out, contract.projector_field);
  out.push_back(contract.whole_carrier ? 1 : 0);
  appendU32(out, static_cast<std::uint32_t>(contract.carrier_path.size()));
  for (const auto field : contract.carrier_path)
    appendU32(out, field);
  out.push_back(contract.has_bit_range ? 1 : 0);
  appendU32(out, contract.bit_begin);
  appendU32(out, contract.bit_end);
}
