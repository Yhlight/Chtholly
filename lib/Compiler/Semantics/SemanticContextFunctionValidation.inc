// Included in SemanticContextFunctions.cpp; this fragment aliases the
// original SemanticContext state and preserves phase ordering.
void SemanticContext::validateLifecyclePolicies() {
  const auto same_type_pattern = [&](auto &&self, TypeId lhs,
                                     TypeId rhs) -> bool {
    const auto &left = sem_ir_.type(lhs);
    const auto &right = sem_ir_.type(rhs);
    if (left.kind != right.kind)
      return false;
    if (left.kind == SemTypeKind::TypeParameter)
      return left.arg1 == right.arg1;
    if (left.kind == SemTypeKind::Reference)
      return sem_ir_.referenceMutability(lhs) ==
                 sem_ir_.referenceMutability(rhs) &&
             self(self, TypeId(left.arg0), TypeId(right.arg0));
    if (left.kind == SemTypeKind::Array)
      return left.arg1 == right.arg1 &&
             self(self, TypeId(left.arg0), TypeId(right.arg0));
    if (left.kind == SemTypeKind::Nominal) {
      if (left.arg0 != right.arg0)
        return false;
      const auto left_args = sem_ir_.typeBlock(TypeBlockId(left.arg1));
      const auto right_args = sem_ir_.typeBlock(TypeBlockId(right.arg1));
      return left_args.size() == right_args.size() &&
             std::ranges::equal(left_args, right_args, [&](TypeId a, TypeId b) {
               return self(self, a, b);
             });
    }
    return true;
  };
  const auto supports_lifecycle =
      [&](auto &&self, TypeId type, SemCanonicalFunctionRole role,
          std::vector<std::uint8_t> &state) -> bool {
    const auto &value = sem_ir_.type(type);
    if (value.kind == SemTypeKind::Function ||
        value.kind == SemTypeKind::Reference ||
        value.kind == SemTypeKind::RawPointer ||
        value.kind == SemTypeKind::CFunctionPointer ||
        value.kind == SemTypeKind::CVariadicFunctionPointer ||
        value.kind == SemTypeKind::Bool || value.kind == SemTypeKind::Char ||
        value.kind == SemTypeKind::Integer ||
        value.kind == SemTypeKind::Float || value.kind == SemTypeKind::String ||
        value.kind == SemTypeKind::Slice ||
        value.kind == SemTypeKind::TypeParameter)
      return true;
    if (value.kind == SemTypeKind::CallbackAdapter ||
        value.kind == SemTypeKind::CallbackRegistration ||
        value.kind == SemTypeKind::CallbackCompletion ||
        value.kind == SemTypeKind::CallbackWake ||
        value.kind == SemTypeKind::ForeignCompletion ||
        value.kind == SemTypeKind::ForeignWake)
      return role != SemCanonicalFunctionRole::Copy;
    if (value.kind == SemTypeKind::Array)
      return self(self, TypeId(value.arg0), role, state);
    if (value.kind == SemTypeKind::Tuple)
      return std::ranges::all_of(
          sem_ir_.typeBlock(TypeBlockId(value.arg0)),
          [&](TypeId element) { return self(self, element, role, state); });
    if (value.kind != SemTypeKind::Nominal)
      return false;
    const auto nominal_id = NominalTypeId(value.arg0);
    if (state[nominal_id.index] == 1)
      return true;
    if (state[nominal_id.index] == 2)
      return state[sem_ir_.nominalTypeCount() + nominal_id.index] != 0;
    state[nominal_id.index] = 1;
    const auto &nested = sem_ir_.nominalType(nominal_id);
    const auto all_payload_fields = [&](const auto &predicate) {
      if (nested.kind != NominalKind::Enum)
        return std::ranges::all_of(nested.fields, predicate);
      return std::ranges::all_of(nested.variants, [&](const auto &variant) {
        return std::ranges::all_of(variant.fields, predicate);
      });
    };
    bool supported = true;
    if (role == SemCanonicalFunctionRole::Copy) {
      const auto policy =
          static_cast<SemLifecycleCopyPolicy>(nested.lifecycle_copy);
      supported = policy != SemLifecycleCopyPolicy::Delete;
      if (policy == SemLifecycleCopyPolicy::Default)
        supported = all_payload_fields([&](const auto &field) {
          return self(self, field.type, role, state);
        });
    } else {
      const auto policy =
          static_cast<SemLifecycleMovePolicy>(nested.lifecycle_move);
      supported = policy != SemLifecycleMovePolicy::Delete;
      if (policy == SemLifecycleMovePolicy::Default)
        supported = all_payload_fields([&](const auto &field) {
          return self(self, field.type, role, state);
        });
    }
    state[nominal_id.index] = 2;
    state[sem_ir_.nominalTypeCount() + nominal_id.index] = supported ? 1 : 0;
    return supported;
  };
  for (std::uint32_t index = 0; index < sem_ir_.nominalTypeCount(); ++index) {
    const auto id = NominalTypeId(index);
    const auto &nominal = sem_ir_.nominalType(id);
    const auto copy =
        static_cast<SemLifecycleCopyPolicy>(nominal.lifecycle_copy);
    const auto drop =
        static_cast<SemLifecycleDropPolicy>(nominal.lifecycle_drop);
    const auto has_copy = nominal.lifecycle_copy_function.hasValue();
    const auto has_drop = nominal.lifecycle_drop_function.hasValue();
    if ((copy == SemLifecycleCopyPolicy::Custom) != has_copy ||
        (drop == SemLifecycleDropPolicy::Custom) != has_drop ||
        (!nominal.generic.hasValue() &&
         copy == SemLifecycleCopyPolicy::Default &&
         drop == SemLifecycleDropPolicy::Custom)) {
      emit(DiagnosticKind::MissingLifecycleImpl, nominal.declaration);
    }
    if (!nominal.canonical_entity.hasValue() && !nominal.generic.hasValue()) {
      if (copy == SemLifecycleCopyPolicy::Default) {
        std::vector<std::uint8_t> state(sem_ir_.nominalTypeCount() * 2);
        const auto supports_copy = [&](const auto &field) {
          return supports_lifecycle(supports_lifecycle, field.type,
                                    SemCanonicalFunctionRole::Copy, state);
        };
        const auto fields_copy =
            nominal.kind == NominalKind::Enum
                ? std::ranges::all_of(nominal.variants,
                                      [&](const auto &variant) {
                                        return std::ranges::all_of(
                                            variant.fields, supports_copy);
                                      })
                : std::ranges::all_of(nominal.fields, supports_copy);
        if (!fields_copy)
          emit(DiagnosticKind::InvalidLifecycleComposition,
               nominal.declaration);
      }
      if (static_cast<SemLifecycleMovePolicy>(nominal.lifecycle_move) ==
          SemLifecycleMovePolicy::Default) {
        std::vector<std::uint8_t> state(sem_ir_.nominalTypeCount() * 2);
        const auto supports_drop = [&](const auto &field) {
          return supports_lifecycle(supports_lifecycle, field.type,
                                    SemCanonicalFunctionRole::Drop, state);
        };
        const auto fields_drop =
            nominal.kind == NominalKind::Enum
                ? std::ranges::all_of(nominal.variants,
                                      [&](const auto &variant) {
                                        return std::ranges::all_of(
                                            variant.fields, supports_drop);
                                      })
                : std::ranges::all_of(nominal.fields, supports_drop);
        if (!fields_drop)
          emit(DiagnosticKind::InvalidLifecycleComposition,
               nominal.declaration);
      }
    }
    const auto check_callable = [&](FunctionId function,
                                    SemCanonicalFunctionRole role) {
      if (!function.hasValue())
        return;
      const auto &callable = sem_ir_.function(function);
      const auto &type = sem_ir_.type(callable.type);
      const auto parameters = sem_ir_.typeBlock(TypeBlockId(type.arg0));
      const auto valid =
          TypeId(type.arg1) == sem_ir_.voidType() &&
          ((role == SemCanonicalFunctionRole::Drop && parameters.size() == 1 &&
            isCanonicalReference(parameters[0], id,
                                 SemReferenceMutability::Mutable)) ||
           (role == SemCanonicalFunctionRole::Copy && parameters.size() == 2 &&
            isCanonicalReference(parameters[0], id,
                                 SemReferenceMutability::Mutable) &&
            isCanonicalReference(parameters[1], id,
                                 SemReferenceMutability::ReadOnly)));
      if (!valid)
        emit(DiagnosticKind::InvalidLifecycleImpl, nominal.declaration);
    };
    check_callable(nominal.lifecycle_copy_function,
                   SemCanonicalFunctionRole::Copy);
    check_callable(nominal.lifecycle_drop_function,
                   SemCanonicalFunctionRole::Drop);

    if (!nominal.canonical_entity.hasValue()) {
      const auto has_value_repr = nominal.value_repr_pattern.hasValue();
      const auto has_pack = nominal.representation_pack_function.hasValue();
      const auto has_init = nominal.representation_init_function.hasValue();
      if (has_value_repr != has_pack || has_value_repr != has_init) {
        emit(DiagnosticKind::MissingRepresentationImpl, nominal.declaration);
      }
      if (has_value_repr && has_pack && has_init) {
        const auto pattern_type = sem_ir_.type(nominal.representation_pattern);
        bool exact_pattern = pattern_type.kind == SemTypeKind::Nominal &&
                             pattern_type.arg0 == id.index;
        if (exact_pattern && nominal.generic.hasValue()) {
          const auto args = sem_ir_.typeBlock(TypeBlockId(pattern_type.arg1));
          const auto binding_count =
              values_.generics().generic(nominal.generic).binding_count;
          exact_pattern = nominal.representation_generic.hasValue() &&
                          args.size() == binding_count;
          for (std::uint32_t arg_index = 0;
               exact_pattern && arg_index < args.size(); ++arg_index) {
            const auto &arg = sem_ir_.type(args[arg_index]);
            exact_pattern =
                arg.kind == SemTypeKind::TypeParameter && arg.arg1 == arg_index;
          }
        } else if (exact_pattern) {
          exact_pattern = !nominal.representation_generic.hasValue();
        }
        const auto &pack =
            sem_ir_.function(nominal.representation_pack_function);
        const auto &pack_type = sem_ir_.type(pack.type);
        const auto pack_params = sem_ir_.typeBlock(TypeBlockId(pack_type.arg0));
        const auto pack_valid =
            pack_params.size() == 1 &&
            isCanonicalReference(pack_params[0], id,
                                 SemReferenceMutability::ReadOnly) &&
            same_type_pattern(same_type_pattern, TypeId(pack_type.arg1),
                              nominal.value_repr_pattern);
        const auto &init =
            sem_ir_.function(nominal.representation_init_function);
        const auto &init_type = sem_ir_.type(init.type);
        const auto init_params = sem_ir_.typeBlock(TypeBlockId(init_type.arg0));
        const auto init_valid =
            TypeId(init_type.arg1) == sem_ir_.voidType() &&
            init_params.size() == 2 &&
            isCanonicalReference(init_params[0], id,
                                 SemReferenceMutability::Mutable) &&
            same_type_pattern(same_type_pattern, init_params[1],
                              nominal.value_repr_pattern);
        if (!exact_pattern || !pack_valid || !init_valid)
          emit(DiagnosticKind::InvalidRepresentationImpl, nominal.declaration);
      }

      const auto exact_projector_signature = [&](FunctionId function,
                                                 SemCanonicalFunctionRole role,
                                                 TypeId field_type) {
        if (!function.hasValue())
          return role == SemCanonicalFunctionRole::ProjectionBorrow ||
                 role == SemCanonicalFunctionRole::ProjectionBorrowMut;
        const auto &callable = sem_ir_.function(function);
        const auto &callable_type = sem_ir_.type(callable.type);
        const auto parameters =
            sem_ir_.typeBlock(TypeBlockId(callable_type.arg0));
        if (role == SemCanonicalFunctionRole::ProjectionLoad ||
            role == SemCanonicalFunctionRole::ProjectionTake) {
          const auto mutability =
              role == SemCanonicalFunctionRole::ProjectionLoad
                  ? SemReferenceMutability::ReadOnly
                  : SemReferenceMutability::Mutable;
          return parameters.size() == 1 &&
                 isCanonicalReference(parameters[0], id, mutability) &&
                 same_type_pattern(same_type_pattern,
                                   TypeId(callable_type.arg1), field_type);
        }
        if (role == SemCanonicalFunctionRole::ProjectionStore ||
            role == SemCanonicalFunctionRole::ProjectionInit)
          return parameters.size() == 2 &&
                 isCanonicalReference(parameters[0], id,
                                      SemReferenceMutability::Mutable) &&
                 same_type_pattern(same_type_pattern, parameters[1],
                                   field_type) &&
                 TypeId(callable_type.arg1) == sem_ir_.voidType();
        if (parameters.size() != 1)
          return false;
        const auto mutable_borrow =
            role == SemCanonicalFunctionRole::ProjectionBorrowMut;
        const auto result = TypeId(callable_type.arg1);
        return isCanonicalReference(parameters[0], id,
                                    mutable_borrow
                                        ? SemReferenceMutability::Mutable
                                        : SemReferenceMutability::ReadOnly) &&
               sem_ir_.type(result).kind == SemTypeKind::Reference &&
               sem_ir_.referenceMutability(result) ==
                   (mutable_borrow ? SemReferenceMutability::Mutable
                                   : SemReferenceMutability::ReadOnly) &&
               same_type_pattern(same_type_pattern,
                                 sem_ir_.referencePointee(result), field_type);
      };
      for (const auto &field : nominal.fields) {
        if (field.projection_kind != PublicObjectProjectionKind::Computed)
          continue;
        const auto projector =
            std::ranges::find(nominal.object_projectors, field.projector_name,
                              &SemObjectProjector::name);
        const auto valid =
            projector != nominal.object_projectors.end() &&
            exact_projector_signature(projector->load_function,
                                      SemCanonicalFunctionRole::ProjectionLoad,
                                      field.type) &&
            exact_projector_signature(projector->store_function,
                                      SemCanonicalFunctionRole::ProjectionStore,
                                      field.type) &&
            exact_projector_signature(projector->take_function,
                                      SemCanonicalFunctionRole::ProjectionTake,
                                      field.type) &&
            exact_projector_signature(projector->init_function,
                                      SemCanonicalFunctionRole::ProjectionInit,
                                      field.type) &&
            exact_projector_signature(
                projector->borrow_function,
                SemCanonicalFunctionRole::ProjectionBorrow, field.type) &&
            exact_projector_signature(
                projector->borrow_mut_function,
                SemCanonicalFunctionRole::ProjectionBorrowMut, field.type);
        if (!valid)
          emit(DiagnosticKind::InvalidRepresentationImpl, field.declaration);
      }
      if (!nominal.object_projectors.empty()) {
        const auto shell_valid = [&](FunctionId function, bool source,
                                     SemReferenceMutability source_mut) {
          if (!function.hasValue())
            return false;
          const auto &callable_type =
              sem_ir_.type(sem_ir_.function(function).type);
          const auto parameters =
              sem_ir_.typeBlock(TypeBlockId(callable_type.arg0));
          return TypeId(callable_type.arg1) == sem_ir_.voidType() &&
                 parameters.size() == (source ? 2U : 1U) &&
                 isCanonicalReference(parameters[0], id,
                                      SemReferenceMutability::Mutable) &&
                 (!source ||
                  isCanonicalReference(parameters[1], id, source_mut));
        };
        if (!shell_valid(nominal.object_init_function, false,
                         SemReferenceMutability::Mutable) ||
            !shell_valid(nominal.object_copy_init_function, true,
                         SemReferenceMutability::ReadOnly) ||
            !shell_valid(nominal.object_move_init_function, true,
                         SemReferenceMutability::Mutable) ||
            !shell_valid(nominal.object_drop_function, false,
                         SemReferenceMutability::Mutable))
          emit(DiagnosticKind::MissingRepresentationImpl, nominal.declaration);
      }
    }
  }
}

void SemanticContext::validateRepresentationInitializers() {
  for (std::uint32_t index = 0; index < sem_ir_.functionCount(); ++index) {
    const auto function_id = FunctionId(index);
    const auto &function = sem_ir_.function(function_id);
    const auto &contract = sem_ir_.functionSemanticContract(function_id);
    if (contract.role != SemCanonicalFunctionRole::Init ||
        !contract.owner.hasValue())
      continue;
    const auto parameters = sem_ir_.localBlock(function.parameters);
    if (parameters.empty())
      continue;
    const auto destination = parameters.front();
    const auto &nominal = sem_ir_.nominalType(contract.owner);
    const auto field_count = nominal.fields.size();
    std::unordered_set<std::uint32_t> destination_writes;
    const auto destination_dereference = [&](InstId id) {
      if (!id.hasValue())
        return false;
      const auto &inst = sem_ir_.inst(id);
      if (inst.kind != SemInstKind::Dereference)
        return false;
      const auto &base = sem_ir_.inst(InstId(inst.arg0));
      return base.kind == SemInstKind::NameRef &&
             LocalId(base.arg0) == destination;
    };
    const auto mark_write_targets = [&](auto &&self,
                                        InstBlockId block) -> void {
      for (const auto id : sem_ir_.instBlock(block)) {
        const auto &inst = sem_ir_.inst(id);
        if (inst.kind == SemInstKind::Assign) {
          auto target = InstId(inst.arg0);
          const auto &target_inst = sem_ir_.inst(target);
          if (destination_dereference(target))
            destination_writes.insert(target.index);
          else if (target_inst.kind == SemInstKind::StructFieldAccess &&
                   destination_dereference(InstId(target_inst.arg0))) {
            destination_writes.insert(target_inst.arg0);
          }
        } else if (inst.kind == SemInstKind::If) {
          for (const auto arm_id : sem_ir_.instBlock(InstBlockId(inst.arg1)))
            self(self, InstBlockId(sem_ir_.inst(arm_id).arg0));
        } else if (inst.kind == SemInstKind::While) {
          self(self, InstBlockId(inst.arg0));
          self(self, InstBlockId(inst.arg1));
        } else if (inst.kind == SemInstKind::For) {
          for (const auto clause_id : sem_ir_.instBlock(InstBlockId(inst.arg0)))
            self(self, InstBlockId(sem_ir_.inst(clause_id).arg1));
          self(self, InstBlockId(inst.arg1));
        } else if (inst.kind == SemInstKind::DoWhile) {
          self(self, InstBlockId(inst.arg1));
          self(self, InstBlockId(inst.arg0));
        } else if (inst.kind == SemInstKind::Defer) {
          self(self, InstBlockId(inst.arg0));
        }
      }
    };
    mark_write_targets(mark_write_targets, function.body);

    struct InitFlow {
      std::vector<std::uint8_t> fields;
      bool falls_through = true;
    };
    constexpr std::uint8_t initialized_on_some_path = 1U;
    constexpr std::uint8_t initialized_on_all_paths = 2U;
    const auto complete = [&](std::span<const std::uint8_t> fields) {
      return std::ranges::all_of(fields, [&](std::uint8_t value) {
        return (value & initialized_on_all_paths) != 0;
      });
    };
    const auto analyze = [&](auto &&self, InstBlockId block,
                             std::vector<std::uint8_t> fields,
                             std::vector<InstBlockId> deferred,
                             bool repeated_path = false) -> InitFlow {
      const auto defer_begin = deferred.size();
      const auto run_deferred = [&](std::size_t begin) {
        for (std::size_t index = deferred.size(); index != begin; --index) {
          auto cleanup = self(self, deferred[index - 1], std::move(fields),
                              std::vector<InstBlockId>{}, repeated_path);
          fields = std::move(cleanup.fields);
          if (!cleanup.falls_through)
            return false;
        }
        return true;
      };
      bool falls_through = true;
      for (const auto id : sem_ir_.instBlock(block)) {
        if (!falls_through)
          break;
        const auto &inst = sem_ir_.inst(id);
        if (inst.kind == SemInstKind::Dereference &&
            destination_dereference(id) &&
            !destination_writes.contains(id.index)) {
          emit(DiagnosticKind::RepresentationInitReadBeforeWrite,
               sem_ir_.location(id));
        } else if (inst.kind == SemInstKind::Assign) {
          const auto target = InstId(inst.arg0);
          const auto &target_inst = sem_ir_.inst(target);
          if (destination_dereference(target)) {
            std::ranges::fill(fields, initialized_on_some_path |
                                          initialized_on_all_paths);
          } else if (target_inst.kind == SemInstKind::StructFieldAccess &&
                     destination_dereference(InstId(target_inst.arg0))) {
            const auto field = sem_ir_.integer(IntegerId(target_inst.arg1));
            if (field >= 0 && static_cast<std::size_t>(field) < fields.size()) {
              const auto field_index = static_cast<std::size_t>(field);
              if (sem_ir_.type(TypeId(target_inst.type)).kind ==
                      SemTypeKind::Reference &&
                  (repeated_path ||
                   (fields[field_index] & initialized_on_some_path) != 0))
                emit(DiagnosticKind::RepresentationReferenceReinitialization,
                     sem_ir_.location(id));
              fields[field_index] =
                  initialized_on_some_path | initialized_on_all_paths;
            }
          }
        } else if (inst.kind == SemInstKind::If) {
          const auto arms = sem_ir_.instBlock(InstBlockId(inst.arg1));
          std::vector<InitFlow> branches;
          for (const auto arm_id : arms)
            branches.push_back(self(self,
                                    InstBlockId(sem_ir_.inst(arm_id).arg0),
                                    fields, deferred, repeated_path));
          if (arms.size() == 1)
            branches.push_back({fields, true});
          const auto first =
              std::ranges::find_if(branches, [](const auto &branch) {
                return branch.falls_through;
              });
          if (first == branches.end()) {
            falls_through = false;
          } else {
            auto merged = first->fields;
            for (auto branch = std::next(first); branch != branches.end();
                 ++branch) {
              if (!branch->falls_through)
                continue;
              for (std::size_t field = 0; field < merged.size(); ++field) {
                const auto some = (merged[field] | branch->fields[field]) &
                                  initialized_on_some_path;
                const auto all = (merged[field] & branch->fields[field]) &
                                 initialized_on_all_paths;
                merged[field] = static_cast<std::uint8_t>(some | all);
              }
            }
            fields = std::move(merged);
          }
        } else if (inst.kind == SemInstKind::While) {
          (void)self(self, InstBlockId(inst.arg0), fields, deferred,
                     repeated_path);
          (void)self(self, InstBlockId(inst.arg1), fields, deferred, true);
        } else if (inst.kind == SemInstKind::For) {
          for (const auto clause_id : sem_ir_.instBlock(InstBlockId(inst.arg0)))
            (void)self(self, InstBlockId(sem_ir_.inst(clause_id).arg1), fields,
                       deferred, true);
          (void)self(self, InstBlockId(inst.arg1), fields, deferred, true);
        } else if (inst.kind == SemInstKind::DoWhile) {
          (void)self(self, InstBlockId(inst.arg1), fields, deferred, true);
          (void)self(self, InstBlockId(inst.arg0), fields, deferred, true);
        } else if (inst.kind == SemInstKind::Defer) {
          deferred.push_back(InstBlockId(inst.arg0));
        } else if (inst.kind == SemInstKind::Return) {
          (void)run_deferred(0);
          if (!complete(fields))
            emit(DiagnosticKind::IncompleteRepresentationInit,
                 sem_ir_.location(id));
          falls_through = false;
        }
      }
      if (falls_through)
        falls_through = run_deferred(defer_begin);
      return {std::move(fields), falls_through};
    };
    const auto result =
        analyze(analyze, function.body, std::vector<std::uint8_t>(field_count),
                {}, false);
    if (result.falls_through && !complete(result.fields))
      emit(DiagnosticKind::IncompleteRepresentationInit, nominal.declaration);
  }
}

void SemanticContext::validateRepresentationCarrierCycles() {
  std::vector<std::uint8_t> state(sem_ir_.nominalTypeCount());
  const auto visit_type = [&](auto &&self, TypeId type,
                              auto &&visit_nominal) -> bool {
    const auto &value = sem_ir_.type(type);
    if (value.kind == SemTypeKind::Reference)
      return false;
    if (value.kind == SemTypeKind::Array)
      return self(self, TypeId(value.arg0), visit_nominal);
    return value.kind == SemTypeKind::Nominal
               ? visit_nominal(visit_nominal, NominalTypeId(value.arg0))
               : false;
  };
  const auto visit_nominal = [&](auto &&self, NominalTypeId id) -> bool {
    if (state[id.index] == 1)
      return true;
    if (state[id.index] == 2)
      return false;
    state[id.index] = 1;
    const auto &nominal = sem_ir_.nominalType(id);
    const auto recursive =
        (nominal.value_repr_pattern.hasValue() &&
         visit_type(visit_type, nominal.value_repr_pattern, self)) ||
        (nominal.object_repr_pattern.hasValue() &&
         visit_type(visit_type, nominal.object_repr_pattern, self));
    state[id.index] = 2;
    return recursive;
  };
  for (std::uint32_t index = 0; index < sem_ir_.nominalTypeCount(); ++index) {
    const auto id = NominalTypeId(index);
    const auto &nominal = sem_ir_.nominalType(id);
    if (!nominal.canonical_entity.hasValue() &&
        visit_nominal(visit_nominal, id))
      emit(DiagnosticKind::RecursiveRepresentationCarrier, nominal.declaration);
  }
}

void SemanticContext::validateObjectRepresentationProjections() {
  const auto prefix = [](std::span<const std::uint32_t> lhs,
                         std::span<const std::uint32_t> rhs) {
    return lhs.size() <= rhs.size() &&
           std::ranges::equal(lhs, rhs.first(lhs.size()));
  };
  for (std::uint32_t nominal_index = 0;
       nominal_index < sem_ir_.nominalTypeCount(); ++nominal_index) {
    const auto &nominal = sem_ir_.nominalType(NominalTypeId(nominal_index));
    if (!nominal.object_repr_pattern.hasValue() ||
        nominal.canonical_entity.hasValue())
      continue;
    if ((nominal.flags & SemNominalTypePublic) != 0 &&
        sem_ir_.type(nominal.object_repr_pattern).kind ==
            SemTypeKind::Nominal &&
        (sem_ir_
             .nominalType(
                 NominalTypeId(sem_ir_.type(nominal.object_repr_pattern).arg0))
             .flags &
         SemNominalTypePublic) == 0) {
      emit(DiagnosticKind::InvalidObjectRepresentationProjection,
           nominal.declaration);
      continue;
    }
    if (nominal.generic.hasValue())
      continue;
    bool valid = true;
    struct Projection {
      PublicObjectProjectionKind kind;
      std::vector<std::uint32_t> path;
      std::uint32_t bit_begin;
      std::uint32_t bit_end;
    };
    std::vector<Projection> projections;
    for (const auto &logical_field : nominal.fields) {
      if (logical_field.projection_kind ==
          PublicObjectProjectionKind::Computed) {
        if (!logical_field.projector_name.hasValue()) {
          valid = false;
          break;
        }
        projections.push_back({logical_field.projection_kind, {}, 0, 0});
        continue;
      }
      auto current = nominal.object_repr_pattern;
      std::vector<std::uint32_t> projection;
      std::vector<NameId> names = logical_field.storage_path;
      if (names.empty())
        names.push_back(logical_field.name);
      for (const auto component : names) {
        if (sem_ir_.type(current).kind != SemTypeKind::Nominal) {
          valid = false;
          break;
        }
        const auto &owner =
            sem_ir_.nominalType(NominalTypeId(sem_ir_.type(current).arg0));
        const auto field =
            std::ranges::find(owner.fields, sem_ir_.name(component).text,
                              [&](const SemNominalField &candidate) {
                                return sem_ir_.name(candidate.name).text;
                              });
        if (field == owner.fields.end()) {
          valid = false;
          break;
        }
        const auto index = static_cast<std::uint32_t>(
            std::distance(owner.fields.begin(), field));
        projection.push_back(index);
        current = sem_ir_.nominalFieldType(current, index);
        if (!current.hasValue()) {
          valid = false;
          break;
        }
      }
      const auto bit_packed = logical_field.projection_kind ==
                              PublicObjectProjectionKind::BitPacked;
      if (!valid ||
          (!bit_packed && sem_ir_.canonicalType(current) !=
                              sem_ir_.canonicalType(logical_field.type)) ||
          (bit_packed && (current != sem_ir_.i32Type() ||
                          logical_field.type != sem_ir_.i32Type() ||
                          logical_field.bit_begin >= logical_field.bit_end ||
                          logical_field.bit_end > 32))) {
        valid = false;
        break;
      }
      for (const auto &other : projections) {
        if (other.kind == PublicObjectProjectionKind::Computed)
          continue;
        const auto same_path = other.path == projection;
        const auto overlapping_bits =
            same_path && other.kind == PublicObjectProjectionKind::BitPacked &&
            bit_packed &&
            std::max(other.bit_begin, logical_field.bit_begin) <
                std::min(other.bit_end, logical_field.bit_end);
        if (overlapping_bits ||
            (same_path && other.kind != logical_field.projection_kind) ||
            (!bit_packed &&
             other.kind != PublicObjectProjectionKind::BitPacked &&
             (prefix(other.path, projection) ||
              prefix(projection, other.path)))) {
          valid = false;
          break;
        }
      }
      projections.push_back({logical_field.projection_kind,
                             std::move(projection), logical_field.bit_begin,
                             logical_field.bit_end});
    }
    if (!valid)
      emit(DiagnosticKind::InvalidObjectRepresentationProjection,
           nominal.declaration);
  }
}
