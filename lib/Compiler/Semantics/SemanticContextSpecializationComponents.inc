// Included by SemanticContextSpecialization.cpp; this fragment aliases the
// single SemanticContext specialization state and preserves queue ordering.
void SemanticContext::buildConcreteSpecializationComponents() {
  // Typed transitions are lowered only after their payload witnesses have
  // been materialized. Populate the session-local descriptor store before
  // constructing concrete specialization artifacts so every artifact sees
  // the same canonical identity.
  buildTypedChannelDescriptors();
  std::vector<SpecificId> candidates;
  for (const auto &[index, unused] : cacheable_specifics_) {
    (void)unused;
    const auto id = SpecificId(index);
    if (!specific_component_fingerprints_.contains(index) &&
        values_.generics().specific(id).definition_state ==
            SpecificRegionState::Ready)
      candidates.push_back(id);
  }
  std::ranges::sort(candidates, [&](SpecificId lhs, SpecificId rhs) {
    return cacheable_specifics_.at(lhs.index).request_fingerprint.hex() <
           cacheable_specifics_.at(rhs.index).request_fingerprint.hex();
  });
  std::unordered_map<std::uint32_t, bool> is_candidate;
  for (const auto id : candidates)
    is_candidate.emplace(id.index, true);

  std::unordered_map<std::uint32_t, std::uint32_t> index;
  std::unordered_map<std::uint32_t, std::uint32_t> lowlink;
  std::unordered_map<std::uint32_t, bool> on_stack;
  std::vector<SpecificId> stack;
  std::vector<std::vector<SpecificId>> components;
  std::uint32_t next_index = 0;
  const auto visit = [&](const auto &self, SpecificId id) -> void {
    index.emplace(id.index, next_index);
    lowlink.emplace(id.index, next_index++);
    stack.push_back(id);
    on_stack[id.index] = true;
    for (const auto &dependency :
         values_.generics().specific(id).dependencies) {
      if (!dependency.target.hasValue() ||
          !is_candidate.contains(dependency.target.index))
        continue;
      if (!index.contains(dependency.target.index)) {
        self(self, dependency.target);
        lowlink[id.index] =
            std::min(lowlink[id.index], lowlink[dependency.target.index]);
      } else if (on_stack[dependency.target.index]) {
        lowlink[id.index] =
            std::min(lowlink[id.index], index[dependency.target.index]);
      }
    }
    if (lowlink[id.index] != index[id.index])
      return;
    std::vector<SpecificId> component;
    while (!stack.empty()) {
      const auto member = stack.back();
      stack.pop_back();
      on_stack[member.index] = false;
      component.push_back(member);
      if (member == id)
        break;
    }
    components.push_back(std::move(component));
  };
  for (const auto id : candidates)
    if (!index.contains(id.index))
      visit(visit, id);

  // Tarjan emits sink components first for the specific-to-callee graph.
  for (auto &specifics : components) {
    std::ranges::sort(specifics, [&](SpecificId lhs, SpecificId rhs) {
      return cacheable_specifics_.at(lhs.index).request_fingerprint.hex() <
             cacheable_specifics_.at(rhs.index).request_fingerprint.hex();
    });
    std::unordered_map<std::uint32_t, std::uint32_t> component_nodes;
    for (std::uint32_t node_index = 0; node_index < specifics.size();
         ++node_index)
      component_nodes.emplace(specifics[node_index].index, node_index);
    std::vector<ConcreteSpecificNodeArtifact> nodes;
    std::string error;
    for (const auto id : specifics) {
      auto node = buildConcreteSpecificNode(id, component_nodes, error);
      if (!node) {
        nodes.clear();
        break;
      }
      nodes.push_back(std::move(*node));
    }
    if (nodes.empty())
      continue;
    ConcreteSpecializationComponentArtifact component(
        semantic_options_fingerprint_, std::move(nodes));
    if (!component.verify(error))
      continue;
    const auto fingerprint = component.fingerprint();
    for (const auto id : specifics) {
      specific_component_fingerprints_.insert_or_assign(id.index, fingerprint);
      values_.generics().specific(id).fingerprint_key = fingerprint.hex();
    }
    sem_ir_.recordSpecializationComponent(std::move(component), true);
  }
  buildConcreteContainerVTables();
}

void SemanticContext::buildTypedChannelDescriptors() {
  // Build an instruction-owner map using the same explicit block walk as the
  // verifier. Instruction IDs are session-local; the owner function is the
  // stable boundary carried by SemTypedChannelDescriptor.
  std::vector<FunctionId> owners(sem_ir_.instCount());
  for (std::uint32_t function_index = 0;
       function_index < sem_ir_.functionCount(); ++function_index) {
    const FunctionId function(function_index);
    const auto &record = sem_ir_.function(function);
    std::vector<InstBlockId> pending{record.body};
    std::unordered_set<std::uint32_t> visited;
    while (!pending.empty()) {
      const auto block = pending.back();
      pending.pop_back();
      if (!block.hasValue() || !visited.insert(block.index).second)
        continue;
      for (const auto instruction : sem_ir_.instBlock(block)) {
        if (instruction.index < owners.size())
          owners[instruction.index] = function;
        const auto &value = sem_ir_.inst(instruction);
        const auto add = [&](std::uint32_t raw) {
          if (raw != core::AnyId::InvalidIndex)
            pending.push_back(InstBlockId(raw));
        };
        switch (value.kind) {
        case SemInstKind::If:
        case SemInstKind::Switch:
          add(value.arg1);
          break;
        case SemInstKind::IfArm:
        case SemInstKind::While:
        case SemInstKind::For:
        case SemInstKind::DoWhile:
        case SemInstKind::Defer:
        case SemInstKind::ScopedBlock:
          add(value.arg0);
          if (value.kind == SemInstKind::While ||
              value.kind == SemInstKind::For ||
              value.kind == SemInstKind::DoWhile)
            add(value.arg1);
          break;
        case SemInstKind::ForClause:
          add(value.arg0);
          add(value.arg1);
          break;
        case SemInstKind::SwitchArm:
          add(value.arg1);
          break;
        default:
          break;
        }
      }
    }
  }

  const auto public_or_structural_fingerprint = [&](TypeId type) {
    if (const auto public_type = concretePublicType(sem_ir_.canonicalType(type)))
      return publicTypeFingerprint(*public_type);
    return StableFingerprint::fromCanonicalBytes(
        "chtholly.next.channel-local-type.v1\n" +
        std::to_string(static_cast<unsigned>(sem_ir_.type(type).kind)) +
        ":" + std::to_string(type.index));
  };
  const auto concurrency = [&](const auto &self, TypeId type,
                               std::unordered_set<std::uint32_t> &active)
      -> TypeConcurrencyFacts {
    if (!type.hasValue() || !active.insert(type.index).second)
      return {};
    const auto leave = [&] { active.erase(type.index); };
    const auto &semantic = sem_ir_.type(type);
    TypeConcurrencyFacts result{true, true};
    switch (semantic.kind) {
    case SemTypeKind::Void:
    case SemTypeKind::Never:
    case SemTypeKind::String:
    case SemTypeKind::Function:
    case SemTypeKind::CFunctionPointer:
    case SemTypeKind::CVariadicFunctionPointer:
    case SemTypeKind::Reference:
    case SemTypeKind::Slice:
    case SemTypeKind::RawPointer:
    case SemTypeKind::TypeParameter:
    case SemTypeKind::TypeProjection:
      result = {};
      break;
    case SemTypeKind::Nominal:
      if (const auto *witness = sem_ir_.nominalSemanticWitness(type)) {
        result = witness->concurrency;
      } else {
        const auto &nominal =
            sem_ir_.nominalType(NominalTypeId(semantic.arg0));
        for (std::uint32_t field = 0; field < nominal.fields.size(); ++field) {
          const auto child = self(self, sem_ir_.nominalFieldType(type, field),
                                  active);
          result.transferable &= child.transferable;
          result.shareable &= child.shareable;
        }
        for (const auto &variant : nominal.variants)
          for (std::uint32_t field = 0; field < variant.fields.size(); ++field) {
            const auto child = self(self, sem_ir_.enumPayloadFieldType(
                                             type,
                                             static_cast<std::uint32_t>(
                                                 &variant - nominal.variants.data()),
                                             field),
                                    active);
            result.transferable &= child.transferable;
            result.shareable &= child.shareable;
          }
      }
      break;
    case SemTypeKind::Array:
      result = self(self, TypeId(semantic.arg0), active);
      break;
    case SemTypeKind::Tuple:
      for (const auto element :
           sem_ir_.typeBlock(TypeBlockId(semantic.arg0))) {
        const auto child = self(self, element, active);
        result.transferable &= child.transferable;
        result.shareable &= child.shareable;
      }
      break;
    default:
      break;
    }
    leave();
    return result;
  };

  for (std::uint32_t index = 0; index < sem_ir_.instCount(); ++index) {
    const InstId instruction_id(index);
    const auto &instruction = sem_ir_.inst(instruction_id);
    const auto intrinsic_role =
        instruction.kind == SemInstKind::CompilerIntrinsicCall
            ? sem_ir_.functionIntrinsicRole(FunctionRefId(instruction.arg0))
            : CompilerIntrinsicRole::None;
    const bool channel_call =
        instruction.kind == SemInstKind::CompilerIntrinsicCall &&
        (intrinsic_role == CompilerIntrinsicRole::ChannelMake ||
         intrinsic_role == CompilerIntrinsicRole::ChannelInit ||
         intrinsic_role == CompilerIntrinsicRole::ChannelSend ||
         intrinsic_role == CompilerIntrinsicRole::ChannelReceive ||
         intrinsic_role == CompilerIntrinsicRole::ChannelClose ||
         intrinsic_role == CompilerIntrinsicRole::ChannelDrop);
    const bool send = instruction.kind == SemInstKind::TypedChannelSendPrepare ||
                      instruction.kind == SemInstKind::TypedChannelSendCommit;
    const bool receive =
        instruction.kind == SemInstKind::TypedChannelReceiveCommit;
    if (!send && !receive && !channel_call)
      continue;
    if (!owners[index].hasValue())
      continue;
    if (!channel_call && (!InstId(instruction.arg0).hasValue() ||
                  instruction.arg0 >= sem_ir_.instCount()))
      continue;
    const auto operands = channel_call
                              ? sem_ir_.instBlock(InstBlockId(instruction.arg1))
                              : std::span<const InstId>{};
    const std::size_t channel_index =
        channel_call
            ? (operands.size() == 1 ||
                       sem_ir_.type(TypeId(sem_ir_.inst(operands[0]).type))
                               .kind == SemTypeKind::Reference
                   ? 0
                   : 1)
            : 0;
    if (channel_call && channel_index >= operands.size())
      continue;
    const auto &channel = channel_call
                              ? sem_ir_.inst(operands[channel_index])
                              : sem_ir_.inst(InstId(instruction.arg0));
    TypeId channel_type(channel.type);
    if (channel_type.hasValue() &&
        sem_ir_.type(channel_type).kind == SemTypeKind::Reference)
      channel_type = sem_ir_.referencePointee(channel_type);
    if (!channel_type.hasValue() ||
        sem_ir_.type(channel_type).kind != SemTypeKind::Nominal)
      continue;
    TypeId payload = TypeId::invalid();
    if (channel_call) {
      const auto arguments = sem_ir_.typeBlock(
          TypeBlockId(sem_ir_.type(channel_type).arg1));
      if (arguments.size() == 1)
        payload = arguments.front();
    } else if (InstId(instruction.arg1).hasValue() &&
               instruction.arg1 < sem_ir_.instCount()) {
      payload = TypeId(sem_ir_.inst(InstId(instruction.arg1)).type);
      if (instruction.kind == SemInstKind::TypedChannelSendPrepare &&
          sem_ir_.type(payload).kind == SemTypeKind::Reference)
        payload = sem_ir_.referencePointee(payload);
    }
    if (!payload.hasValue())
      continue;
    const auto payload_kind = sem_ir_.type(payload).kind;
    if (payload_kind == SemTypeKind::TypeParameter ||
        payload_kind == SemTypeKind::TypeProjection)
      continue;
    SemTypedChannelDescriptor descriptor;
    descriptor.owner_function = owners[index];
    descriptor.payload_type = payload;
    descriptor.payload_type_fingerprint =
        public_or_structural_fingerprint(payload);
    descriptor.representation = sem_ir_.typeRepresentation(payload);
    // Runtime descriptors always carry an explicit drop callback. Scalar and
    // pointer-like values report `Destroy=None` in ordinary value contexts;
    // for channel storage that is the trivial destructor, not an absent
    // witness.
    if (descriptor.representation.destroy == DestroyReprKind::None)
      descriptor.representation.destroy = DestroyReprKind::Trivial;
    std::unordered_set<std::uint32_t> active;
    descriptor.concurrency = concurrency(concurrency, payload, active);
    if (const auto *witness = sem_ir_.nominalSemanticWitness(payload)) {
      descriptor.lifecycle_fingerprint = witness->result_fingerprint;
      descriptor.drop_target = witness->destroy_target;
    } else {
      descriptor.lifecycle_fingerprint =
          StableFingerprint::fromCanonicalBytes(
              "chtholly.next.channel-lifecycle.v1\n" +
              descriptor.payload_type_fingerprint.hex() + ":" +
              std::to_string(static_cast<unsigned>(
                  descriptor.representation.move)) + ":" +
              std::to_string(static_cast<unsigned>(
                  descriptor.representation.destroy)));
    }
    descriptor.layout_fingerprint =
        StableFingerprint::fromCanonicalBytes(
            "chtholly.next.channel-layout.v1\n" +
            descriptor.payload_type_fingerprint.hex() + ":" +
            std::to_string(static_cast<unsigned>(
                descriptor.representation.value_repr)) + ":" +
            std::to_string(static_cast<unsigned>(
                descriptor.representation.init_repr)) + ":" +
            std::to_string(static_cast<unsigned>(
              descriptor.representation.ownership)));
    descriptor.outcome_fingerprint = makeChannelOutcome().fingerprint();
    descriptor.component_identity = "chtholly.typed-channel";
    descriptor.operation_identity =
        "payload." + descriptor.payload_type_fingerprint.hex();
    descriptor.operation_kind =
        receive || intrinsic_role == CompilerIntrinsicRole::ChannelReceive
            ? ComponentAbi2OperationKind::Receive : ComponentAbi2OperationKind::Send;
    descriptor.lease_policy = ComponentAbi2LeasePolicy::Exclusive;
    descriptor.ownership_flags = 1;
    descriptor.component_descriptor_digest =
        StableFingerprint::fromCanonicalBytes(
            "chtholly.component.payload.v1\n" +
            descriptor.component_identity + "\n" + descriptor.operation_identity +
            "\n" + descriptor.payload_type_fingerprint.hex() + "\n" +
            descriptor.layout_fingerprint.hex() + "\n" +
            descriptor.lifecycle_fingerprint.hex());
    descriptor.runtime_abi_epoch = 1;
    if (!descriptor.concurrency.transferable) {
      emit(DiagnosticKind::InvalidObjectType, sem_ir_.location(instruction_id));
      continue;
    }
    sem_ir_.recordTypedChannelDescriptor(std::move(descriptor));
  }
}

void SemanticContext::buildConcreteContainerVTables() {
  // Intrinsic calls are the point at which all generic arguments and their
  // constraint witnesses are concrete.  Record a semantic vtable descriptor
  // here; LLVM lowering later resolves callbacks for the current target.
  for (std::uint32_t index = 0; index < sem_ir_.instCount(); ++index) {
    const auto instruction = sem_ir_.inst(InstId(index));
    if (instruction.kind != SemInstKind::CompilerIntrinsicCall)
      continue;
    const auto role = sem_ir_.functionIntrinsicRole(FunctionRefId(instruction.arg0));
    if (!isContainerCompilerIntrinsic(role))
      continue;
    const bool hash_map = isHashMapCompilerIntrinsic(role);
    TypeId container_type(instruction.type);
    const auto operands = sem_ir_.instBlock(InstBlockId(instruction.arg1));
    if (!operands.empty() && role != CompilerIntrinsicRole::HashMapMake &&
        role != CompilerIntrinsicRole::HashSetMake) {
      container_type = TypeId(sem_ir_.inst(operands.front()).type);
      if (sem_ir_.type(container_type).kind == SemTypeKind::Reference)
        container_type = sem_ir_.referencePointee(container_type);
    }
    if (!container_type.hasValue() ||
        sem_ir_.type(container_type).kind != SemTypeKind::Nominal)
      continue;
    const auto type_arguments = sem_ir_.typeBlock(
        TypeBlockId(sem_ir_.type(container_type).arg1));
    if (type_arguments.size() != (hash_map ? 2U : 1U))
      continue;
    const auto key_type = type_arguments.front();
    const auto value_type = hash_map ? type_arguments.back() : key_type;
    // PublicType is the preferred identity for independently compiled
    // entities. During the same semantic session a package-local nominal may
    // not have received its public entity yet; keep a structural, module-
    // qualified identity for that case instead of dropping the descriptor and
    // making a valid local native use fail later in lowering.
    const auto structural_type = [&](const auto &self, TypeId type,
                                     std::vector<TypeId> &stack) -> std::string {
      if (std::ranges::find(stack, type) != stack.end())
        return "<cycle>";
      stack.push_back(type);
      const auto &semantic = sem_ir_.type(type);
      std::string result = std::to_string(static_cast<unsigned>(semantic.kind));
      if (semantic.kind == SemTypeKind::Nominal) {
        const auto &nominal = sem_ir_.nominalType(NominalTypeId(semantic.arg0));
        result += ":" + std::string(sem_ir_.identifier(
                               sem_ir_.name(nominal.name).text));
        result += ":" + std::string(sem_ir_.identifier(sem_ir_.moduleName()));
        for (const auto argument :
             sem_ir_.typeBlock(TypeBlockId(semantic.arg1)))
          result += ":" + self(self, argument, stack);
      } else if (semantic.kind == SemTypeKind::Array) {
        result += ":" + std::to_string(semantic.arg1) + ":" +
                  self(self, TypeId(semantic.arg0), stack);
      } else if (semantic.kind == SemTypeKind::Reference) {
        result += ":" +
                  std::to_string(static_cast<unsigned>(
                      sem_ir_.referenceMutability(type))) + ":" +
                  self(self, TypeId(semantic.arg0), stack);
      } else if (semantic.kind == SemTypeKind::RawPointer) {
        result += ":" + std::to_string(sem_ir_.rawPointerPointeeConst(type)) +
                  ":" + self(self, TypeId(semantic.arg0), stack);
      } else if (semantic.kind == SemTypeKind::Slice) {
        result += ":" + std::to_string(sem_ir_.sliceMutable(type)) + ":" +
                  self(self, TypeId(semantic.arg0), stack);
        result += ":" + self(self, TypeId(semantic.arg0), stack);
      } else if (semantic.kind == SemTypeKind::Tuple) {
        for (const auto element :
             sem_ir_.typeBlock(TypeBlockId(semantic.arg0)))
          result += ":" + self(self, element, stack);
      } else if (semantic.kind == SemTypeKind::Integer) {
        result += ":" + std::to_string(semantic.arg0) + ":" +
                  std::to_string(semantic.arg1);
      } else if (semantic.kind == SemTypeKind::Float) {
        result += ":" + std::to_string(semantic.arg0);
      }
      stack.pop_back();
      return result;
    };
    const auto type_fingerprint = [&](TypeId type) {
      if (const auto public_type = concretePublicType(sem_ir_.canonicalType(type)))
        return publicTypeFingerprint(*public_type);
      std::vector<TypeId> stack;
      return StableFingerprint::fromCanonicalBytes(
          "chtholly.next.container-local-type.v1\n" +
          structural_type(structural_type, type, stack));
    };
    const auto key_fingerprint = type_fingerprint(key_type);
    const auto value_fingerprint = type_fingerprint(value_type);
    SemConcreteContainerVTable descriptor;
    descriptor.container_type = container_type;
    descriptor.key_type = key_type;
    descriptor.value_type = value_type;
    descriptor.hash_map = hash_map;
    descriptor.key_type_fingerprint = key_fingerprint;
    descriptor.value_type_fingerprint = value_fingerprint;
    const auto &reference =
        sem_ir_.functionRef(FunctionRefId(instruction.arg0));
    if (reference.specific.hasValue()) {
      const auto &specific = values_.generics().specific(reference.specific);
      descriptor.constraint_witnesses = specific.constraint_witnesses;
    }
    std::string layout_key = "chtholly.next.container-layout.v2\n" +
                             descriptor.key_type_fingerprint.hex() + "\n" +
                             descriptor.value_type_fingerprint.hex() + "\n" +
                             (descriptor.hash_map ? "map\n" : "set\n");
    for (const auto &witness : descriptor.constraint_witnesses)
      layout_key += witness.hex() + "\n";
    const auto key_representation = sem_ir_.typeRepresentation(key_type);
    const auto value_representation = sem_ir_.typeRepresentation(value_type);
    layout_key += std::to_string(static_cast<unsigned>(key_representation.move)) +
                  ":" + std::to_string(static_cast<unsigned>(key_representation.destroy)) +
                  ":" + std::to_string(static_cast<unsigned>(value_representation.move)) +
                  ":" + std::to_string(static_cast<unsigned>(value_representation.destroy));
    descriptor.layout_fingerprint =
        StableFingerprint::fromCanonicalBytes(std::move(layout_key));
    // Resolve the concrete Hash and Equal member targets from the materialized
    // witness rather than asking LLVM lowering to infer them from layout. The
    // witness fingerprint is the persistent identity; the FunctionRefId is a
    // session-local handle used only while emitting this module.
    const auto find_witness_member = [&](std::string_view interface_name,
                                         std::string_view member_name)
        -> FunctionRefId {
      for (std::uint32_t witness_index = 0;
           witness_index < sem_ir_.interfaceWitnessCount(); ++witness_index) {
        const auto witness_id = InterfaceWitnessId(witness_index);
        const auto &candidate = sem_ir_.interfaceWitness(witness_id);
        if (candidate.state != SemInterfaceWitnessState::Complete ||
            sem_ir_.canonicalType(candidate.self_type) !=
                sem_ir_.canonicalType(key_type))
          continue;
        // The constraint fingerprint is intentionally the primary match. A
        // concrete witness can be materialized after the generic call's
        // specific is created, so retain a structural fallback on the same
        // subject/interface when a provider did not persist the fingerprint
        // in the local specific record.
        const bool fingerprint_match =
            std::ranges::find(descriptor.constraint_witnesses,
                              candidate.fingerprint) !=
            descriptor.constraint_witnesses.end();
        const auto &interface_value = sem_ir_.interface(candidate.interface_id);
        if (!interface_value.canonical_entity.hasValue())
          continue;
        const auto *entity = sem_ir_.importIRs().tryGetEntity(
            interface_value.canonical_entity);
        if (!entity || sem_ir_.identifier(entity->package_name) != "std" ||
            sem_ir_.identifier(entity->module_name) != "std::hash" ||
            sem_ir_.identifier(entity->name) != interface_name ||
            (!fingerprint_match && candidate.generic.hasValue()))
          continue;
        const auto requirement = std::ranges::find_if(
            interface_value.requirements, [&](const auto &entry) {
              return entry.kind == SemInterfaceRequirementKind::Function &&
                     sem_ir_.identifier(sem_ir_.name(entry.name).text) ==
                         member_name;
            });
        if (requirement == interface_value.requirements.end())
          continue;
        const auto requirement_index = static_cast<std::uint32_t>(
            requirement - interface_value.requirements.begin());
        const auto entry = std::ranges::find(
            candidate.entries, requirement_index,
            &SemInterfaceWitnessEntry::requirement);
        if (entry != candidate.entries.end() && entry->function.hasValue())
          return entry->function;
      }
      return FunctionRefId::invalid();
    };
    descriptor.key_hash_function =
        find_witness_member("Hash", "hash");
    descriptor.key_equal_function =
        find_witness_member("Equal", "equal");
    sem_ir_.recordConcreteContainerVTable(std::move(descriptor));
  }
}

void SemanticContext::finalizeSpecificFingerprints() {
  const auto append_key_field = [](std::string &key, std::string_view value) {
    key += std::to_string(value.size());
    key += ':';
    key += value;
  };
  const auto generic_key = [&](GenericId id) {
    const auto &generic = values_.generics().generic(id);
    std::string key;
    for (std::uint32_t index = 0; index < sem_ir_.functionRefCount(); ++index) {
      const auto &reference = sem_ir_.functionRef(FunctionRefId(index));
      if (reference.generic != id ||
          reference.specific != generic.self_specific ||
          !reference.public_entity.hasValue())
        continue;
      if (const auto *entity =
              sem_ir_.importIRs().tryGetEntity(reference.public_entity)) {
        append_key_field(key, values_.identifier(entity->package_name));
        append_key_field(key, values_.identifier(entity->module_name));
        append_key_field(key, values_.identifier(entity->name));
        append_key_field(key, entity->fingerprint.hex());
        return key;
      }
    }
    append_key_field(key, "local");
    append_key_field(key, values_.identifier(generic.module_name));
    append_key_field(key, values_.identifier(generic.name));
    return key;
  };
  const auto append_type_key = [&](const auto &self, std::string &key,
                                   CanonicalTypeId id) -> void {
    const auto &type = values_.generics().type(id);
    append_key_field(key, canonicalTypeKindName(type.kind));
    switch (type.kind) {
    case CanonicalTypeKind::Array:
      self(self, key, CanonicalTypeId(type.arg0));
      append_key_field(key, std::to_string(type.arg1));
      break;
    case CanonicalTypeKind::Function:
    case CanonicalTypeKind::AsyncFunction:
    case CanonicalTypeKind::Reference:
    case CanonicalTypeKind::RawPointer:
    case CanonicalTypeKind::CallbackAdapter:
      append_key_field(key, std::to_string(type.arg0));
      append_key_field(key, std::to_string(type.elements.size()));
      for (const auto element : type.elements)
        self(self, key, element);
      break;
    case CanonicalTypeKind::CallbackRegistration:
      append_key_field(key, std::to_string(type.registration_authority));
      append_key_field(key, std::to_string(type.registration_entry_parameter));
      append_key_field(key,
                       std::to_string(type.registration_userdata_parameter));
      append_key_field(key,
                       std::to_string(type.registration_release_parameter));
      append_key_field(key, std::to_string(type.registration_bindings.size()));
      for (const auto &binding : type.registration_bindings) {
        append_key_field(key, binding.name);
        append_key_field(key, std::to_string(binding.parameter_index));
      }
      for (const auto role : type.registration_arm_parameters)
        append_key_field(key, std::to_string(role));
      for (const auto role : type.registration_detach_parameters)
        append_key_field(key, std::to_string(role));
      append_key_field(key, std::to_string(type.elements.size()));
      for (const auto element : type.elements)
        self(self, key, element);
      break;
    case CanonicalTypeKind::CallbackCompletion:
      append_key_field(key, std::to_string(type.registration_authority));
      for (const auto role : type.registration_arm_parameters)
        append_key_field(key, std::to_string(role));
      for (const auto role : type.registration_detach_parameters)
        append_key_field(key, std::to_string(role));
      append_key_field(key, std::to_string(type.elements.size()));
      for (const auto element : type.elements)
        self(self, key, element);
      break;
    case CanonicalTypeKind::CallbackWake:
      append_key_field(key, std::to_string(type.elements.size()));
      for (const auto element : type.elements)
        self(self, key, element);
      break;
    case CanonicalTypeKind::CoroutineTask:
    case CanonicalTypeKind::CoroutineTaskOutcome:
    case CanonicalTypeKind::CoroutineTaskCompletion:
    case CanonicalTypeKind::CoroutineTaskCompletionSet:
    case CanonicalTypeKind::CoroutineTaskSelection:
    case CanonicalTypeKind::CoroutineChecked:
      append_key_field(key, std::to_string(type.elements.size()));
      for (const auto element : type.elements)
        self(self, key, element);
      break;
    case CanonicalTypeKind::CFunctionPointer:
    case CanonicalTypeKind::CVariadicFunctionPointer:
      append_key_field(key, std::to_string(type.callable_context_parameter));
      append_key_field(key, std::to_string(type.arg0));
      append_key_field(key, std::to_string(type.elements.size()));
      for (const auto element : type.elements)
        self(self, key, element);
      break;
    case CanonicalTypeKind::TypeParameter:
      append_key_field(key, generic_key(GenericId(type.arg0)));
      append_key_field(key, std::to_string(type.arg1));
      break;
    case CanonicalTypeKind::Nominal:
      append_key_field(key, type.nominal_key);
      append_key_field(key, std::to_string(type.elements.size()));
      for (const auto element : type.elements)
        self(self, key, element);
      break;
    case CanonicalTypeKind::Void:
    case CanonicalTypeKind::Never:
    case CanonicalTypeKind::Bool:
    case CanonicalTypeKind::Integer:
    case CanonicalTypeKind::Float:
    case CanonicalTypeKind::String:
    case CanonicalTypeKind::CoroutineScope:
    case CanonicalTypeKind::Count:
      break;
    }
  };
  const auto specific_key = [&](SpecificId id) {
    const auto &specific = values_.generics().specific(id);
    std::string key = generic_key(specific.generic);
    for (const auto argument : specific.arguments) {
      const auto &constant = values_.generics().constant(argument);
      append_type_key(append_type_key, key, constant.type);
      append_key_field(
          key, std::to_string(static_cast<std::uint32_t>(constant.dependence)));
    }
    return key;
  };
  std::vector<SpecificId> roots;
  roots.reserve(deferred_definitions_.entries().size());
  for (const auto &task : deferred_definitions_.entries())
    if (task.hasPayload())
      roots.push_back(task.definition().specific);
  std::ranges::sort(roots, {}, &SpecificId::index);
  roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
  for (const auto root : roots) {
    if (specific_component_fingerprints_.contains(root.index))
      continue;
    std::vector<SpecificId> pending{root};
    std::vector<std::string> nodes;
    std::vector<std::string> edges;
    std::unordered_map<std::uint32_t, bool> seen;
    for (std::size_t index = 0; index < pending.size(); ++index) {
      const auto id = pending[index];
      if (!seen.emplace(id.index, true).second)
        continue;
      const auto &specific = values_.generics().specific(id);
      const auto source_key = specific_key(id);
      nodes.push_back(source_key);
      for (const auto &dependency : specific.dependencies) {
        std::string edge =
            source_key + "->" + std::to_string(dependency.call_ordinal) + ":" +
            (dependency.target.hasValue() ? specific_key(dependency.target)
                                          : std::string("concrete"));
        if (const auto *entity = sem_ir_.importIRs().tryGetEntity(
                PublicEntityId(dependency.public_entity)))
          edge += ":" + entity->fingerprint.hex();
        edges.push_back(std::move(edge));
        if (dependency.target.hasValue())
          pending.push_back(dependency.target);
      }
    }
    std::ranges::sort(nodes);
    std::ranges::sort(edges);
    std::string canonical = "chtholly.next.specific-closure.v3\n";
    for (const auto &node : nodes)
      canonical += "n " + node + "\n";
    for (const auto &edge : edges)
      canonical += "e " + edge + "\n";
    values_.generics().specific(root).fingerprint_key =
        StableFingerprint::fromCanonicalBytes(canonical).hex();
  }
}
