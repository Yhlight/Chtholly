// Included inside PlaceStateAnalyzer; this fragment aliases its existing
// place-state maps, cleanup plans, and worklists.
  explicit PlaceStateAnalyzer(SemIR &sem_ir) : sem_ir_(sem_ir) {
    for (std::uint32_t index = 0; index < sem_ir_.instCount(); ++index) {
      const auto &inst = sem_ir_.inst(InstId(index));
      if (inst.kind == SemInstKind::StructFieldAccess ||
          inst.kind == SemInstKind::UnionFieldAccess ||
          inst.kind == SemInstKind::EnumPayloadAccess ||
          inst.kind == SemInstKind::Index || inst.kind == SemInstKind::Move ||
          inst.kind == SemInstKind::Copy ||
          inst.kind == SemInstKind::BorrowPlace ||
          inst.kind == SemInstKind::Assign ||
          inst.kind == SemInstKind::Placement ||
          inst.kind == SemInstKind::CarrierView ||
          inst.kind == SemInstKind::TypedChannelSendPrepare ||
          inst.kind == SemInstKind::TypedChannelSendCommit ||
          inst.kind == SemInstKind::TypedChannelSendCancel ||
          inst.kind == SemInstKind::TypedChannelReceiveAcquire ||
          inst.kind == SemInstKind::TypedChannelReceiveCommit ||
          inst.kind == SemInstKind::TypedChannelReceiveCancel ||
          inst.kind == SemInstKind::TypedChannelClose)
        projection_inputs_.insert(inst.arg0);
      if (inst.kind == SemInstKind::TypedChannelSendPrepare ||
          inst.kind == SemInstKind::TypedChannelSendCommit ||
          inst.kind == SemInstKind::TypedChannelReceiveCommit)
        projection_inputs_.insert(inst.arg1);
    }
  }

  std::vector<PlaceStateDiagnostic> run() {
    for (std::uint32_t index = 0; index < sem_ir_.functionCount(); ++index) {
      const auto function_id = FunctionId(index);
      if ((sem_ir_.function(function_id).flags & SemFunctionTemplate) == 0)
        analyzeFunction(function_id);
    }
    sem_ir_.setPlaceStateQuery(PlaceStateQuery(
        std::move(places_), std::move(move_places_), std::move(copy_places_),
        std::move(return_cleanups_), std::move(edge_cleanups_),
        std::move(full_expression_cleanups_), std::move(block_cleanups_),
        std::move(suspension_cleanups_), std::move(cancellation_cleanups_),
        std::move(reinitializations_), std::move(observations_)));
    for (auto &diagnostic : diagnostics_) {
      if (!diagnostic.evidence.empty())
        continue;
      OwnershipEvidenceKind evidence_kind =
          OwnershipEvidenceKind::ConflictingAccess;
      std::string detail = "place-state analysis rejected this operation";
      switch (diagnostic.kind) {
      case PlaceStateDiagnosticKind::UseAfterMove:
      case PlaceStateDiagnosticKind::UseOfMaybeMovedPlace:
      case PlaceStateDiagnosticKind::UseOfPartiallyMovedPlace:
      case PlaceStateDiagnosticKind::InvalidMoveOperand:
      case PlaceStateDiagnosticKind::MoveUnavailable:
        evidence_kind = OwnershipEvidenceKind::MoveOrigin;
        detail = "the place is not live for this move or use";
        break;
      case PlaceStateDiagnosticKind::UninitializedStorage:
        evidence_kind = OwnershipEvidenceKind::CallEffect;
        detail = "no verified initialization effect established this storage";
        break;
      case PlaceStateDiagnosticKind::AnalysisNonConvergent:
        evidence_kind = OwnershipEvidenceKind::WidenedRegion;
        detail = "place-state worklist did not converge within its budget";
        break;
      default:
        break;
      }
      diagnostic.evidence.push_back(
          {diagnostic.instruction, evidence_kind, 0, std::move(detail)});
    }
    return std::move(diagnostics_);
  }

private:
  struct CleanupRegistration {
    enum class Kind : std::uint8_t {
      Local,
      Temporary,
      Extended,
      Defer
    } kind = Kind::Local;
    LocalId local;
    InstBlockId block;
  };

  CanonicalTypeId
  substituteCanonicalType(CanonicalTypeId type_id, GenericId generic,
                          std::span<const CanonicalTypeId> arguments) {
    const auto type = sem_ir_.genericValues().type(type_id);
    if (type.kind == CanonicalTypeKind::TypeParameter &&
        type.arg0 == generic.index)
      return type.arg1 < arguments.size() ? arguments[type.arg1]
                                          : CanonicalTypeId::invalid();
    if (type.kind == CanonicalTypeKind::Array) {
      const auto element = substituteCanonicalType(CanonicalTypeId(type.arg0),
                                                   generic, arguments);
      return sem_ir_.genericValues().internType(
          {.kind = CanonicalTypeKind::Array,
           .arg0 = element.index,
           .arg1 = type.arg1});
    }
    if (type.kind == CanonicalTypeKind::Function ||
        type.kind == CanonicalTypeKind::Reference ||
        type.kind == CanonicalTypeKind::Nominal ||
        type.kind == CanonicalTypeKind::RawPointer ||
        type.kind == CanonicalTypeKind::CFunctionPointer ||
        type.kind == CanonicalTypeKind::CVariadicFunctionPointer ||
        type.kind == CanonicalTypeKind::CallbackAdapter) {
      CanonicalType result = type;
      result.elements.clear();
      for (const auto element : type.elements)
        result.elements.push_back(
            substituteCanonicalType(element, generic, arguments));
      return sem_ir_.genericValues().internType(std::move(result));
    }
    return type_id;
  }

  TypeId nominalFieldType(TypeId nominal_type, std::uint32_t field_index) {
    const auto &type = sem_ir_.type(nominal_type);
    const auto &nominal = sem_ir_.nominalType(NominalTypeId(type.arg0));
    const auto field_type = nominal.fields[field_index].type;
    if (!nominal.generic.hasValue())
      return field_type;
    std::vector<CanonicalTypeId> arguments;
    for (const auto argument : sem_ir_.typeBlock(TypeBlockId(type.arg1)))
      arguments.push_back(sem_ir_.canonicalType(argument));
    const auto canonical = substituteCanonicalType(
        sem_ir_.canonicalType(field_type), nominal.generic, arguments);
    return canonical.hasValue() ? sem_ir_.materializeType(canonical)
                                : TypeId::invalid();
  }

  void collectNestedUnions(TypeId type, std::vector<PlaceProjection> &path,
                           std::vector<UnionState> &unions,
                           std::vector<EnumState> &enums, std::uint32_t depth) {
    if (depth > 128)
      return;
    const auto &semantic_type = sem_ir_.type(type);
    if (semantic_type.kind == SemTypeKind::Nominal) {
      const auto &nominal =
          sem_ir_.nominalType(NominalTypeId(semantic_type.arg0));
      if (nominal.kind == NominalKind::Union)
        unions.push_back({path, type,
                          std::vector<std::uint8_t>(nominal.fields.size()),
                          false});
      if (nominal.kind == NominalKind::Enum) {
        enums.push_back({path, type,
                         std::vector<std::uint8_t>(nominal.variants.size()),
                         true});
        for (std::uint32_t variant = 0; variant < nominal.variants.size();
             ++variant)
          for (std::uint32_t field = 0;
               field < nominal.variants[variant].fields.size(); ++field) {
            path.push_back({PlaceProjectionKind::EnumPayload, field, variant});
            collectNestedUnions(
                sem_ir_.enumPayloadFieldType(type, variant, field), path,
                unions, enums, depth + 1);
            path.pop_back();
          }
        return;
      }
      for (std::uint32_t index = 0; index < nominal.fields.size(); ++index) {
        path.push_back({PlaceProjectionKind::Field, index});
        collectNestedUnions(nominalFieldType(type, index), path, unions, enums,
                            depth + 1);
        path.pop_back();
      }
      return;
    }
    if (semantic_type.kind == SemTypeKind::Array && semantic_type.arg1 != 0 &&
        semantic_type.arg1 <= MaxTrackedElements) {
      for (std::uint32_t index = 0; index < semantic_type.arg1; ++index) {
        path.push_back({PlaceProjectionKind::Element, index});
        collectNestedUnions(TypeId(semantic_type.arg0), path, unions, enums,
                            depth + 1);
        path.pop_back();
      }
      return;
    }
    if (semantic_type.kind == SemTypeKind::Tuple) {
      const auto elements = sem_ir_.typeBlock(TypeBlockId(semantic_type.arg0));
      for (std::uint32_t index = 0; index < elements.size(); ++index) {
        path.push_back({PlaceProjectionKind::Field, index});
        collectNestedUnions(elements[index], path, unions, enums, depth + 1);
        path.pop_back();
      }
    }
  }

  void collectLeaves(TypeId type, std::vector<PlaceProjection> &path,
                     std::vector<LeafState> &leaves,
                     std::vector<UnionState> &unions,
                     std::vector<EnumState> &enums, std::uint32_t depth = 0) {
    if (depth > 128) {
      leaves.push_back({path, type, MovedBit});
      return;
    }
    const auto representation = sem_ir_.typeRepresentation(type);
    const auto &semantic_type = sem_ir_.type(type);
    if (semantic_type.kind == SemTypeKind::Nominal &&
        representation.destroy == DestroyReprKind::Trivial) {
      const auto &nominal =
          sem_ir_.nominalType(NominalTypeId(semantic_type.arg0));
      if (nominal.kind == NominalKind::Union) {
        unions.push_back({path, type,
                          std::vector<std::uint8_t>(nominal.fields.size()),
                          false});
        for (std::uint32_t index = 0; index < nominal.fields.size(); ++index) {
          path.push_back({PlaceProjectionKind::Field, index});
          collectNestedUnions(nominalFieldType(type, index), path, unions,
                              enums, depth + 1);
          path.pop_back();
        }
        leaves.push_back({path, type, MovedBit});
        return;
      }
      if (nominal.kind == NominalKind::Enum) {
        enums.push_back({path, type,
                         std::vector<std::uint8_t>(nominal.variants.size()),
                         true});
        // The tag is a non-owning leaf so whole-value state remains distinct
        // from the state of any one payload alternative.
        leaves.push_back({path, sem_ir_.voidType(), MovedBit});
        for (std::uint32_t variant = 0; variant < nominal.variants.size();
             ++variant)
          for (std::uint32_t field = 0;
               field < nominal.variants[variant].fields.size(); ++field) {
            path.push_back({PlaceProjectionKind::EnumPayload, field, variant});
            collectLeaves(sem_ir_.enumPayloadFieldType(type, variant, field),
                          path, leaves, unions, enums, depth + 1);
            path.pop_back();
          }
        return;
      }
      if (!nominal.fields.empty()) {
        for (std::uint32_t index = 0; index < nominal.fields.size(); ++index) {
          path.push_back({PlaceProjectionKind::Field, index});
          const auto field_type = nominalFieldType(type, index);
          if (nominal.fields[index].projection_kind ==
              PublicObjectProjectionKind::StableAddress)
            collectLeaves(field_type, path, leaves, unions, enums, depth + 1);
          else
            leaves.push_back({path, field_type, MovedBit});
          path.pop_back();
        }
        return;
      }
    }
    if (semantic_type.kind == SemTypeKind::Array && semantic_type.arg1 != 0 &&
        semantic_type.arg1 <= MaxTrackedElements) {
      for (std::uint32_t index = 0; index < semantic_type.arg1; ++index) {
        path.push_back({PlaceProjectionKind::Element, index});
        collectLeaves(TypeId(semantic_type.arg0), path, leaves, unions, enums,
                      depth + 1);
        path.pop_back();
      }
      return;
    }
    if (semantic_type.kind == SemTypeKind::Tuple) {
      const auto elements = sem_ir_.typeBlock(TypeBlockId(semantic_type.arg0));
      for (std::uint32_t index = 0; index < elements.size(); ++index) {
        path.push_back({PlaceProjectionKind::Field, index});
        collectLeaves(elements[index], path, leaves, unions, enums, depth + 1);
        path.pop_back();
      }
      return;
    }
    if (semantic_type.kind == SemTypeKind::Slice) {
      path.push_back({PlaceProjectionKind::AnyElement, 0});
      leaves.push_back({path, TypeId(semantic_type.arg0), InitializedBit});
      path.pop_back();
      return;
    }
    leaves.push_back({path, type, MovedBit});
  }
