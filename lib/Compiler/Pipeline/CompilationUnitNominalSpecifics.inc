// Included inside buildNominalArtifacts; this fragment aliases the shared
// SemIR/public-interface and nominal-layout state.
  std::unordered_map<std::string, std::size_t> specific_by_type;
  std::unordered_set<std::string> specific_in_progress;
  std::unordered_map<std::string, StableFingerprint>
      recursive_specific_witnesses;
  std::unordered_set<std::string> representation_in_progress;
  std::function<std::optional<StableFingerprint>(const PublicType &,
                                                 std::string &)>
      build_specific;
  std::function<bool(const PublicType &, std::vector<StableFingerprint> &,
                     std::string &)>
      build_representation_dependencies;
  const auto is_atomic_storage = [&](const auto &nominal_entity) {
    for (std::uint32_t index = 0; index < registry.entityCount(); ++index) {
      const auto *candidate = registry.tryGetEntity(PublicEntityId(index));
      if (!candidate || candidate->kind != PublicEntityKind::Function ||
          !isAtomicCompilerIntrinsic(candidate->intrinsic_role) ||
          !candidate->member_owner)
        continue;
      const auto &owner = *candidate->member_owner;
      if (owner.kind == PublicEntityKind::NominalType &&
          owner.canonical_package == nominal_entity.canonical_package &&
          owner.canonical_module == nominal_entity.canonical_module &&
          owner.canonical_name == nominal_entity.canonical_name &&
          owner.expected_fingerprint == nominal_entity.expected_fingerprint)
        return true;
    }
    return false;
  };
  build_representation_dependencies =
      [&](const PublicType &dependency,
          std::vector<StableFingerprint> &fingerprints,
          std::string &nested_error) -> bool {
    if (dependency.kind == PublicTypeKind::Reference)
      return true;
    if (dependency.kind == PublicTypeKind::Array) {
      if (dependency.arguments.size() != 1) {
        nested_error = "custom value representation has an invalid array "
                       "carrier";
        return false;
      }
      return build_representation_dependencies(dependency.arguments.front(),
                                               fingerprints, nested_error);
    }
    if (dependency.kind != PublicTypeKind::Nominal)
      return true;
    const auto dependency_key = canonicalPublicTypeBytes(dependency);
    if (representation_in_progress.contains(dependency_key)) {
      nested_error = "custom representation has a recursive carrier dependency";
      return false;
    }
    const auto fingerprint = build_specific(dependency, nested_error);
    if (!fingerprint)
      return false;
    fingerprints.push_back(*fingerprint);
    return true;
  };
  build_specific =
      [&](const PublicType &type,
          std::string &nested_error) -> std::optional<StableFingerprint> {
    const auto key = canonicalPublicTypeBytes(type);
    if (const auto found = specific_by_type.find(key);
        found != specific_by_type.end())
      return result.specifics[found->second].result_fingerprint;
    if (representation_in_progress.contains(key)) {
      nested_error = "custom representation has a recursive carrier dependency";
      return std::nullopt;
    }
    if (type.kind != PublicTypeKind::Nominal)
      return std::nullopt;
    const auto *definition = find_definition(type.nominal_entity);
    if (!definition) {
      nested_error = "nominal type `" + type.nominal_entity.canonical_name +
                     "` has no matching public definition";
      return std::nullopt;
    }
    if (!specific_in_progress.insert(key).second) {
      // Recursive nominal witnesses are represented by a stable SCC seed. The
      // seed is the canonical backedge identity in the completed component.
      const auto [it, inserted] = recursive_specific_witnesses.emplace(
          key, StableFingerprint::fromCanonicalBytes(
                   std::string("chtholly.next.nominal-witness-scc.v1") + key));
      (void)inserted;
      return it->second;
    }
    auto specific = buildNominalTypeSpecific(
        {.definition = definition,
         .arguments = type.arguments,
         .semantic_options_fingerprint =
             resolve_semantic_options
                 ? resolve_semantic_options(type.nominal_entity)
                 : semantic_options,
         .child_specific_fingerprint = build_specific},
        nested_error);
    specific_in_progress.erase(key);
    if (!specific)
      return std::nullopt;
    if (specific->kind == NominalKind::Union) {
      const auto is_trivial_member = [&](const auto &self,
                                         const PublicType &member) -> bool {
        switch (member.kind) {
        case PublicTypeKind::Bool:
        case PublicTypeKind::Integer:
        case PublicTypeKind::Float:
        case PublicTypeKind::RawPointer:
        case PublicTypeKind::Function:
        case PublicTypeKind::CFunctionPointer:
          return true;
        case PublicTypeKind::Array:
          return member.array_bound != 0 && member.arguments.size() == 1 &&
                 self(self, member.arguments.front());
        case PublicTypeKind::Nominal: {
          const auto child =
              specific_by_type.find(canonicalPublicTypeBytes(member));
          if (child == specific_by_type.end())
            return false;
          const auto &child_specific = result.specifics[child->second];
          const auto &facts =
              child_specific.nominal_semantic_witness.representation;
          return child_specific.representation_policy ==
                     NominalRepresentationPolicy::C &&
                 facts.value_repr == ValueReprKind::Pointer &&
                 facts.init_repr == InitReprKind::InPlace &&
                 facts.copy == CopyReprKind::Trivial &&
                 facts.move == MoveReprKind::Trivial &&
                 facts.destroy == DestroyReprKind::Trivial &&
                 facts.object_repr == ObjectReprKind::NominalAggregate &&
                 std::ranges::all_of(
                     child_specific.fields,
                     [&](const auto &field) { return self(self, field.type); });
        }
        case PublicTypeKind::Void:
        case PublicTypeKind::Never:
        case PublicTypeKind::String:
        case PublicTypeKind::Reference:
        case PublicTypeKind::TypeParameter:
        case PublicTypeKind::CallbackAdapter:
        case PublicTypeKind::CallbackRegistration:
        case PublicTypeKind::CallbackCompletion:
        case PublicTypeKind::CallbackWake:
        case PublicTypeKind::Count:
          return false;
        }
        return false;
      };
      if (specific->representation_policy != NominalRepresentationPolicy::C ||
          !std::ranges::all_of(specific->fields, [&](const auto &field) {
            return is_trivial_member(is_trivial_member, field.type);
          })) {
        nested_error = "repr(C) union requires trivially copyable, movable, "
                       "destructible C-compatible members";
        return std::nullopt;
      }
    }
    std::vector<StableFingerprint> carrier_specific_fingerprints;
    if (specific->nominal_semantic_witness.representation.value_repr ==
        ValueReprKind::Custom) {
      if (!specific->nominal_semantic_witness.value_repr_carrier) {
        nested_error = "custom value representation has no carrier";
        return std::nullopt;
      }
      representation_in_progress.insert(key);
      const auto valid = build_representation_dependencies(
          *specific->nominal_semantic_witness.value_repr_carrier,
          carrier_specific_fingerprints, nested_error);
      representation_in_progress.erase(key);
      if (!valid)
        return std::nullopt;
    }
    if (specific->object_repr_carrier) {
      representation_in_progress.insert(key);
      const auto valid = build_representation_dependencies(
          *specific->object_repr_carrier, carrier_specific_fingerprints,
          nested_error);
      representation_in_progress.erase(key);
      if (!valid)
        return std::nullopt;
    }
    for (std::uint32_t nominal_index = 0;
         nominal_index < sem_ir.nominalTypeCount(); ++nominal_index) {
      const auto nominal_id = NominalTypeId(nominal_index);
      const auto &nominal = sem_ir.nominalType(nominal_id);
      if (!definitions[nominal_index] ||
          definitions[nominal_index]->entity != specific->template_entity)
        continue;
      const auto *canonical_nominal =
          registry.tryGetEntity(nominal.canonical_entity);
      const auto is_structural_closure_environment =
          canonical_nominal && !canonical_nominal->nominal_is_exported &&
          sem_ir.identifier(canonical_nominal->name).starts_with("$closure$");
      if (nominal.canonical_entity.hasValue() &&
          !is_structural_closure_environment) {
        auto witness =
            resolve_nominal_semantic_witness
                ? resolve_nominal_semantic_witness(*specific, nested_error)
                : std::nullopt;
        if (!witness) {
          if (!nested_error.empty())
            return std::nullopt;
          if (is_atomic_storage(specific->template_entity)) {
            NominalSemanticWitnessBuildInput atomic_witness_input{
                .specific = &*specific,
                .representation =
                    specific->nominal_semantic_witness.representation,
                .concurrency = {true, true},
                .value_repr_carrier =
                    specific->nominal_semantic_witness.value_repr_carrier,
                .object_repr_carrier = specific->object_repr_carrier,
                .transitive_specific_fingerprints =
                    specific->child_specific_fingerprints};
            auto atomic_witness = buildNominalSemanticWitnessArtifact(
                atomic_witness_input, nested_error);
            if (!atomic_witness ||
                !bindNominalSemanticWitness(
                    *specific, std::move(*atomic_witness), nested_error))
              return std::nullopt;
            break;
          }
          // A generic imported nominal may not have a concrete witness in the
          // provider package. Fall through to the lifecycle target lookup so
          // its generic canonical drop/copy function can be materialized from
          // the provider public interface.
        }
        if (witness) {
          if (witness->nominal_template != specific->template_entity ||
              witness->arguments != specific->arguments ||
              witness->semantic_options_fingerprint !=
                  specific->semantic_options_fingerprint ||
              witness->structural_specific_fingerprint !=
                  specific->structural_fingerprint ||
              !bindNominalSemanticWitness(*specific, *witness, nested_error)) {
            if (nested_error.empty())
              nested_error =
                  "imported nominal semantic witness does not match its "
                  "nominal specific";
            return std::nullopt;
          }
          const auto materialize_target =
              [&](const std::optional<PublicEntityReferenceArtifact> &target) {
                if (!target)
                  return true;
                const auto entity = registry.findEntity(
                    target->canonical_package, target->canonical_module,
                    target->canonical_name, PublicEntityKind::Function,
                    target->expected_fingerprint);
                const auto *definition = registry.tryGetEntity(entity);
                if (!definition ||
                    definition->fingerprint != target->expected_fingerprint) {
                  nested_error =
                      "nominal semantic witness target has a stale canonical "
                      "entity";
                  return false;
                }
                return sem_ir
                    .addCanonicalExternalFunctionRef(entity, nested_error,
                                                     specific->arguments)
                    .hasValue();
              };
          if (!materialize_target(witness->copy_target) ||
              !materialize_target(witness->destroy_target) ||
              !materialize_target(witness->pack_target) ||
              !materialize_target(witness->init_target))
            return std::nullopt;
          break;
        }
      }
      TypeRepresentationFacts representation =
          specific->nominal_semantic_witness.representation;
      const auto declared_copy =
          static_cast<SemLifecycleCopyPolicy>(nominal.lifecycle_copy);
      const auto move =
          static_cast<SemLifecycleMovePolicy>(nominal.lifecycle_move);
      const auto declared_drop =
          static_cast<SemLifecycleDropPolicy>(nominal.lifecycle_drop);
      const auto pattern_applies = [&](TypeId pattern, GenericId generic) {
        if (!pattern.hasValue() || !generic.hasValue())
          return false;
        auto public_pattern = map_type(pattern);
        if (!public_pattern)
          return false;
        PublicType concrete(specific->template_entity, specific->arguments);
        std::vector<std::optional<PublicType>> bindings(
            sem_ir.genericValues().generic(generic).binding_count);
        const auto match = [&](const auto &self, const PublicType &candidate,
                               const PublicType &actual) -> bool {
          if (candidate.kind == PublicTypeKind::TypeParameter) {
            if (candidate.binding_index >= bindings.size())
              return false;
            auto &binding = bindings[candidate.binding_index];
            if (!binding)
              binding = actual;
            return *binding == actual;
          }
          if (candidate.kind != actual.kind ||
              candidate.arguments.size() != actual.arguments.size())
            return false;
          if (candidate.kind == PublicTypeKind::Nominal &&
              candidate.nominal_entity != actual.nominal_entity)
            return false;
          if (candidate.kind == PublicTypeKind::Reference &&
              (candidate.reference_mutability != actual.reference_mutability ||
               candidate.reference_provenance != actual.reference_provenance))
            return false;
          for (std::size_t index = 0; index < candidate.arguments.size();
               ++index)
            if (!self(self, candidate.arguments[index],
                      actual.arguments[index]))
              return false;
          return true;
        };
        return match(match, *public_pattern, concrete) &&
               std::ranges::all_of(bindings, [](const auto &binding) {
                 return binding.has_value();
               });
      };
      const auto imported_copy_target =
          nominal.canonical_entity.hasValue()
              ? find_lifecycle_target(specific->template_entity,
                                      SemCanonicalFunctionRole::Copy)
              : std::nullopt;
      const auto imported_drop_target =
          nominal.canonical_entity.hasValue()
              ? find_lifecycle_target(specific->template_entity,
                                      SemCanonicalFunctionRole::Drop)
              : std::nullopt;
      const auto materialize_imported_target =
          [&](const std::optional<PublicEntityReferenceArtifact> &target) {
            if (!target)
              return true;
            const auto target_entity = registry.findEntity(
                target->canonical_package, target->canonical_module,
                target->canonical_name, PublicEntityKind::Function,
                target->expected_fingerprint);
            const auto *definition = registry.tryGetEntity(target_entity);
            if (!definition ||
                definition->fingerprint != target->expected_fingerprint) {
              nested_error =
                  "imported lifecycle target has a stale canonical entity";
              return false;
            }
            return sem_ir
                .addCanonicalExternalFunctionRef(target_entity, nested_error,
                                                 specific->arguments)
                .hasValue();
          };
      if (!materialize_imported_target(imported_copy_target) ||
          !materialize_imported_target(imported_drop_target))
        return std::nullopt;
      const auto generic_copy_applies =
          pattern_applies(nominal.lifecycle_copy_pattern,
                          nominal.lifecycle_copy_generic) ||
          imported_copy_target.has_value();
      const auto generic_drop_applies =
          pattern_applies(nominal.lifecycle_drop_pattern,
                          nominal.lifecycle_drop_generic) ||
          imported_drop_target.has_value();
      bool fields_copyable = true;
      bool fields_movable = true;
      const auto inspect_field = [&](const auto &field) {
        if (field.type.kind == PublicTypeKind::CallbackAdapter ||
            field.type.kind == PublicTypeKind::CallbackRegistration ||
            field.type.kind == PublicTypeKind::CallbackCompletion ||
            field.type.kind == PublicTypeKind::CallbackWake) {
          fields_copyable = false;
          return;
        }
        if (field.type.kind != PublicTypeKind::Nominal)
          return;
        const auto child =
            specific_by_type.find(canonicalPublicTypeBytes(field.type));
        if (child == specific_by_type.end())
          return;
        const auto &child_specific = result.specifics[child->second];
        fields_copyable =
            fields_copyable &&
            child_specific.nominal_semantic_witness.representation.copy !=
                CopyReprKind::Unavailable;
        fields_movable =
            fields_movable &&
            child_specific.nominal_semantic_witness.representation.move !=
                MoveReprKind::Unavailable;
      };
      for (const auto &field : specific->fields)
        inspect_field(field);
      for (const auto &variant : specific->variants)
        for (const auto &field : variant.fields)
          inspect_field(field);
      // A generic lifecycle impl is an applicability template. Its concrete
      // nominal specific receives generated field-wise work instead of a
      // process-local generic function reference.
      const auto copy = imported_copy_target ? SemLifecycleCopyPolicy::Custom
                        : nominal.lifecycle_copy_generic.hasValue()
                            ? generic_copy_applies
                                  ? SemLifecycleCopyPolicy::Default
                                  : SemLifecycleCopyPolicy::Delete
                            : declared_copy;
      const auto drop =
          imported_drop_target ? SemLifecycleDropPolicy::Custom
          : nominal.lifecycle_drop_generic.hasValue()
              ? (generic_drop_applies &&
                         declared_drop == SemLifecycleDropPolicy::Custom
                     ? SemLifecycleDropPolicy::Custom
                     : SemLifecycleDropPolicy::Default)
              : declared_drop;
      representation.copy =
          (copy == SemLifecycleCopyPolicy::Delete || !fields_copyable)
              ? CopyReprKind::Unavailable
          : copy == SemLifecycleCopyPolicy::Custom ? CopyReprKind::Custom
                                                   : CopyReprKind::Trivial;
      representation.move =
          move == SemLifecycleMovePolicy::Delete || !fields_movable
              ? MoveReprKind::Unavailable
              : MoveReprKind::Trivial;
      representation.destroy = drop == SemLifecycleDropPolicy::Custom
                                   ? DestroyReprKind::Custom
                                   : DestroyReprKind::Trivial;
      const auto target =
          [&](std::string_view role,
              bool representation =
                  false) -> std::optional<PublicEntityReferenceArtifact> {
        const auto hidden =
            std::string(representation ? "$representation$" : "$lifecycle$") +
            std::string(sem_ir.identifier(sem_ir.name(nominal.name).text)) +
            "$" + std::string(role);
        const auto *function = public_interface.findFunction(hidden);
        if (!function)
          return std::nullopt;
        return PublicEntityReferenceArtifact{
            PublicEntityKind::Function, function->canonical_package,
            function->canonical_module, function->canonical_name,
            function->entity_fingerprint};
      };
      const auto named_target = [&](std::string hidden)
          -> std::optional<PublicEntityReferenceArtifact> {
        const auto *function = public_interface.findFunction(hidden);
        if (!function)
          return std::nullopt;
        return PublicEntityReferenceArtifact{
            PublicEntityKind::Function, function->canonical_package,
            function->canonical_module, function->canonical_name,
            function->entity_fingerprint};
      };
      const auto projection_target = [&](std::string_view projector,
                                         std::string_view role) {
        return named_target(
            "$projection$" +
            std::string(sem_ir.identifier(sem_ir.name(nominal.name).text)) +
            "$" + std::string(projector) + "$" + std::string(role));
      };
      const auto object_target = [&](std::string_view role) {
        return named_target(
            "$object$" +
            std::string(sem_ir.identifier(sem_ir.name(nominal.name).text)) +
            "$" + std::string(role));
      };
      NominalSemanticWitnessBuildInput witness_input{
          .specific = &*specific,
          .representation = representation,
          .concurrency =
              [&] {
                if (is_atomic_storage(specific->template_entity))
                  return TypeConcurrencyFacts{true, true};
                TypeConcurrencySummary summary(
                    [&](const PublicType &type)
                        -> const NominalTypeSpecificArtifact * {
                      const auto found =
                          specific_by_type.find(canonicalPublicTypeBytes(type));
                      return found == specific_by_type.end()
                                 ? nullptr
                                 : &result.specifics[found->second];
                    });
                TypeConcurrencyFacts nominal_facts{true, true};
                std::string concurrency_error;
                const auto append = [&](const PublicType &type) {
                  std::string current_error;
                  const auto facts = summary.summarize(type, current_error);
                  if (!current_error.empty())
                    concurrency_error = std::move(current_error);
                  nominal_facts.transferable &= facts.transferable;
                  nominal_facts.shareable &= facts.shareable;
                };
                for (const auto &field : specific->fields)
                  append(field.type);
                for (const auto &variant : specific->variants)
                  for (const auto &field : variant.fields)
                    append(field.type);
                // Capability is deliberately fail-closed if a malformed
                // recursive graph exceeds the bounded summary worklist.
                if (!concurrency_error.empty())
                  return TypeConcurrencyFacts{};
                return nominal_facts;
              }(),
          .value_repr_carrier =
              specific->nominal_semantic_witness.value_repr_carrier,
          .object_repr_carrier = specific->object_repr_carrier};
      if (specific->object_repr_carrier) {
        const auto prefix = [](std::span<const std::uint32_t> lhs,
                               std::span<const std::uint32_t> rhs) {
          return lhs.size() <= rhs.size() &&
                 std::ranges::equal(lhs, rhs.first(lhs.size()));
        };
        for (const auto &logical_field : specific->fields) {
          ObjectFieldProjectionArtifact projection;
          projection.kind = static_cast<ObjectFieldProjectionKind>(
              logical_field.projection_kind);
          projection.bit_begin = logical_field.bit_begin;
          projection.bit_end = logical_field.bit_end;
          if (projection.kind == ObjectFieldProjectionKind::Computed) {
            if (!logical_field.projection_region_path.empty()) {
              PublicType current = *specific->object_repr_carrier;
              for (const auto &component :
                   logical_field.projection_region_path) {
                if (current.kind != PublicTypeKind::Nominal) {
                  nested_error =
                      "computed projection region crosses a non-nominal field";
                  return std::nullopt;
                }
                const auto carrier_found =
                    specific_by_type.find(canonicalPublicTypeBytes(current));
                if (carrier_found == specific_by_type.end()) {
                  nested_error =
                      "computed projection region has no completed carrier";
                  return std::nullopt;
                }
                const auto &carrier_specific =
                    result.specifics[carrier_found->second];
                const auto region_field =
                    std::ranges::find(carrier_specific.fields, component,
                                      &PublicNominalFieldArtifact::name);
                if (region_field == carrier_specific.fields.end()) {
                  nested_error = "computed projection region names a missing "
                                 "carrier field";
                  return std::nullopt;
                }
                projection.region_indices.push_back(
                    static_cast<std::uint32_t>(std::distance(
                        carrier_specific.fields.begin(), region_field)));
                current = region_field->type;
              }
            }
            projection.load_target =
                projection_target(logical_field.projector_name, "load");
            projection.store_target =
                projection_target(logical_field.projector_name, "store");
            projection.take_target =
                projection_target(logical_field.projector_name, "take");
            projection.init_target =
                projection_target(logical_field.projector_name, "init");
            projection.borrow_target =
                projection_target(logical_field.projector_name, "borrow");
            projection.borrow_mut_target =
                projection_target(logical_field.projector_name, "borrow_mut");
            if (!projection.load_target || !projection.store_target ||
                !projection.take_target || !projection.init_target) {
              nested_error =
                  "computed object projection is missing a canonical "
                  "load/store/take/init target";
              return std::nullopt;
            }
            projection.capabilities = ProjectionLoad | ProjectionStore |
                                      ProjectionTake | ProjectionInit;
            if (projection.borrow_target)
              projection.capabilities |= ProjectionBorrow;
            if (projection.borrow_mut_target)
              projection.capabilities |= ProjectionBorrowMut;
            witness_input.object_field_projections.push_back(
                std::move(projection));
            continue;
          }
          auto path_names = logical_field.storage_path;
          if (path_names.empty())
            path_names.push_back(logical_field.name);
          PublicType current = *specific->object_repr_carrier;
          for (const auto &component : path_names) {
            if (current.kind != PublicTypeKind::Nominal) {
              nested_error =
                  "object storage projection crosses a non-nominal field";
              return std::nullopt;
            }
            const auto current_key = canonicalPublicTypeBytes(current);
            const auto found_specific = specific_by_type.find(current_key);
            if (found_specific == specific_by_type.end()) {
              nested_error =
                  "object storage projection has no completed carrier specific";
              return std::nullopt;
            }
            const auto &carrier_specific =
                result.specifics[found_specific->second];
            const auto field =
                std::ranges::find(carrier_specific.fields, component,
                                  &PublicNominalFieldArtifact::name);
            if (field == carrier_specific.fields.end()) {
              nested_error = "object storage projection names a missing field";
              return std::nullopt;
            }
            projection.field_indices.push_back(static_cast<std::uint32_t>(
                std::distance(carrier_specific.fields.begin(), field)));
            current = field->type;
          }
          if (projection.kind == ObjectFieldProjectionKind::StableAddress &&
              current != logical_field.type) {
            nested_error =
                "object storage projection type does not match logical field";
            return std::nullopt;
          }
          if (projection.kind == ObjectFieldProjectionKind::BitPacked &&
              (current.kind != PublicTypeKind::Integer ||
               current.scalar_width != 32 ||
               logical_field.type.kind != PublicTypeKind::Integer ||
               logical_field.type.scalar_width != 32 ||
               projection.bit_end > 32)) {
            nested_error = "bit-packed projection requires an in-range i32 "
                           "logical field and carrier";
            return std::nullopt;
          }
          for (const auto &other : witness_input.object_field_projections) {
            const auto same_path =
                other.field_indices == projection.field_indices;
            const auto overlapping_bits =
                same_path &&
                other.kind == ObjectFieldProjectionKind::BitPacked &&
                projection.kind == ObjectFieldProjectionKind::BitPacked &&
                std::max(other.bit_begin, projection.bit_begin) <
                    std::min(other.bit_end, projection.bit_end);
            if (overlapping_bits ||
                (other.kind != ObjectFieldProjectionKind::BitPacked &&
                 projection.kind != ObjectFieldProjectionKind::BitPacked &&
                 (prefix(other.field_indices, projection.field_indices) ||
                  prefix(projection.field_indices, other.field_indices))) ||
                (same_path && other.kind != projection.kind)) {
              nested_error = "object storage projections overlap";
              return std::nullopt;
            }
          }
          projection.region_indices = projection.field_indices;
          projection.capabilities = ProjectionLoad | ProjectionStore |
                                    ProjectionTake | ProjectionInit;
          if (projection.kind == ObjectFieldProjectionKind::StableAddress)
            projection.capabilities |= ProjectionBorrow | ProjectionBorrowMut;
          witness_input.object_field_projections.push_back(
              std::move(projection));
        }
        const auto has_computed = std::ranges::any_of(
            witness_input.object_field_projections, [](const auto &projection) {
              return projection.kind == ObjectFieldProjectionKind::Computed;
            });
        if (has_computed) {
          witness_input.object_init_target = object_target("object_init");
          witness_input.object_copy_init_target =
              object_target("object_copy_init");
          witness_input.object_move_init_target =
              object_target("object_move_init");
          witness_input.object_drop_target = object_target("object_drop");
          if (!witness_input.object_init_target ||
              !witness_input.object_copy_init_target ||
              !witness_input.object_move_init_target ||
              !witness_input.object_drop_target) {
            nested_error =
                "opaque object representation is missing canonical shell "
                "lifecycle targets";
            return std::nullopt;
          }
        }
      }
      if (copy == SemLifecycleCopyPolicy::Custom)
        witness_input.copy_target =
            imported_copy_target ? imported_copy_target : target("copy");
      if (drop == SemLifecycleDropPolicy::Custom)
        witness_input.destroy_target =
            imported_drop_target ? imported_drop_target : target("drop");
      if (representation.value_repr == ValueReprKind::Custom) {
        witness_input.pack_target = target("pack", true);
        witness_input.init_target = target("init", true);
      }
      if ((copy == SemLifecycleCopyPolicy::Custom &&
           !witness_input.copy_target) ||
          (drop == SemLifecycleDropPolicy::Custom &&
           !witness_input.destroy_target) ||
          (representation.value_repr == ValueReprKind::Custom &&
           (!witness_input.pack_target || !witness_input.init_target))) {
        nested_error =
            "canonical semantic target is absent from the public interface";
        return std::nullopt;
      }
      witness_input.transitive_specific_fingerprints =
          specific->child_specific_fingerprints;
      witness_input.transitive_specific_fingerprints.insert(
          witness_input.transitive_specific_fingerprints.end(),
          carrier_specific_fingerprints.begin(),
          carrier_specific_fingerprints.end());
      if (generic_copy_applies) {
        const auto template_target =
            imported_copy_target ? imported_copy_target : target("copy");
        if (!template_target) {
          nested_error =
              "generic copy lifecycle template is absent from the public "
              "interface";
          return std::nullopt;
        }
        witness_input.transitive_specific_fingerprints.push_back(
            template_target->expected_fingerprint);
      }
      if (generic_drop_applies) {
        const auto template_target =
            imported_drop_target ? imported_drop_target : target("drop");
        if (!template_target) {
          nested_error =
              "generic drop lifecycle template is absent from the public "
              "interface";
          return std::nullopt;
        }
        witness_input.transitive_specific_fingerprints.push_back(
            template_target->expected_fingerprint);
      }
      // Materialize a canonical field-wise lifecycle body while the concrete
      // specific is available. Primitive fields use a stable pseudo-witness;
      // nominal fields use the recursively produced child specific witness.
      const auto field_witness = [&](const PublicType &field_type,
                                     std::size_t nominal_child_index) {
        if (field_type.kind == PublicTypeKind::Nominal &&
            nominal_child_index < specific->child_specific_fingerprints.size())
          return specific->child_specific_fingerprints[nominal_child_index];
        return StableFingerprint::fromCanonicalBytes(
            std::string("chtholly.next.lifecycle-primitive.v1") +
            canonicalPublicTypeBytes(field_type));
      };
      std::size_t nominal_child_index = 0;
      if (copy == SemLifecycleCopyPolicy::Default && fields_copyable) {
        for (std::size_t field_index = 0; field_index < specific->fields.size();
             ++field_index) {
          const auto &field = specific->fields[field_index];
          witness_input.copy_body.push_back(
              {LifecycleBodyOpKind::CopyField,
               static_cast<std::uint32_t>(field_index),
               field_witness(field.type, nominal_child_index)});
          if (field.type.kind == PublicTypeKind::Nominal)
            ++nominal_child_index;
        }
      }
      nominal_child_index = 0;
      if (drop == SemLifecycleDropPolicy::Default) {
        for (std::size_t field_index = 0; field_index < specific->fields.size();
             ++field_index) {
          const auto &field = specific->fields[field_index];
          witness_input.drop_body.push_back(
              {LifecycleBodyOpKind::DropField,
               static_cast<std::uint32_t>(field_index),
               field_witness(field.type, nominal_child_index)});
          if (field.type.kind == PublicTypeKind::Nominal)
            ++nominal_child_index;
        }
      }
      if (witness_input.copy_target)
        witness_input.transitive_specific_fingerprints.push_back(
            witness_input.copy_target->expected_fingerprint);
      if (witness_input.destroy_target)
        witness_input.transitive_specific_fingerprints.push_back(
            witness_input.destroy_target->expected_fingerprint);
      if (witness_input.pack_target)
        witness_input.transitive_specific_fingerprints.push_back(
            witness_input.pack_target->expected_fingerprint);
      if (witness_input.init_target)
        witness_input.transitive_specific_fingerprints.push_back(
            witness_input.init_target->expected_fingerprint);
      for (const auto &projection : witness_input.object_field_projections) {
        const std::array targets{
            projection.load_target,   projection.store_target,
            projection.take_target,   projection.init_target,
            projection.borrow_target, projection.borrow_mut_target};
        for (const auto &projection_target_value : targets)
          if (projection_target_value)
            witness_input.transitive_specific_fingerprints.push_back(
                projection_target_value->expected_fingerprint);
      }
      const std::array shell_targets{witness_input.object_init_target,
                                     witness_input.object_copy_init_target,
                                     witness_input.object_move_init_target,
                                     witness_input.object_drop_target};
      for (const auto &shell_target : shell_targets)
        if (shell_target)
          witness_input.transitive_specific_fingerprints.push_back(
              shell_target->expected_fingerprint);
      auto witness =
          buildNominalSemanticWitnessArtifact(witness_input, nested_error);
      if (!witness || !bindNominalSemanticWitness(
                          *specific, std::move(*witness), nested_error))
        return std::nullopt;
      break;
    }
    const auto index = result.specifics.size();
    result.specifics.push_back(std::move(*specific));
    specific_by_type.emplace(key, index);
    return result.specifics.back().result_fingerprint;
  };
