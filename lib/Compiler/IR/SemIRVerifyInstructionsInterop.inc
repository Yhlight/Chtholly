// Included inside SemIRVerificationContext::verifyInstructions; the shared
// read-only context locals and diagnostics remain owned by the caller.

    case SemInstKind::Call:
    case SemInstKind::ForeignOperationCall: {
      const auto &callee = functionRef(FunctionRefId(value.arg0));
      const auto &callee_type = type(callee.local_type);
      if (callee_type.kind != SemTypeKind::Function) {
        error = "call target does not have a function type";
        return false;
      }
      const auto callee_domain = [&] {
        if (callee.local_function.hasValue())
          return functionSemanticContract(callee.local_function).domain;
        const auto *entity = imports_.tryGetEntity(callee.public_entity);
        return entity ? entity->semantic_contract.domain
                      : CallableSemanticDomain::Count;
      }();
      if (callee_domain != CallableSemanticDomain::Ordinary &&
          callee_domain != CallableSemanticDomain::NominalConstruction) {
        error = "ordinary call targets a representation helper";
        return false;
      }
      const auto arguments = instBlock(InstBlockId(value.arg1));
      const auto parameters = typeBlock(TypeBlockId(callee_type.arg0));
      const auto *foreign_signature = [&]() -> const ForeignAbiSignature * {
        if (callee.local_function.hasValue()) {
          const auto &declaration = functionDeclaration(callee.local_function);
          return declaration.foreign_signature ? &*declaration.foreign_signature
                                               : nullptr;
        }
        const auto *entity = imports_.tryGetEntity(callee.public_entity);
        return entity && entity->foreign_signature ? &*entity->foreign_signature
                                                   : nullptr;
      }();
      const auto is_variadic =
          foreign_signature && foreign_signature->is_variadic;
      bool result_matches = instruction_type.index == callee_type.arg1;
      if (!result_matches && value.kind == SemInstKind::ForeignOperationCall) {
        const auto *operation =
            [&]() -> const interop::ForeignOperationArtifact * {
          if (callee.local_function.hasValue()) {
            const auto &declaration =
                functionDeclaration(callee.local_function);
            return declaration.interop_artifact
                       ? imports_.interopRegistry().resolve(
                             *declaration.interop_artifact)
                       : nullptr;
          }
          const auto *entity = imports_.tryGetEntity(callee.public_entity);
          return entity && entity->interop_artifact
                     ? imports_.interopRegistry().resolve(
                           *entity->interop_artifact)
                     : nullptr;
        }();
        const auto shape = canonicalResultShape(instruction_type);
        if (operation && shape &&
            operation->error_extractor !=
                interop::ForeignOperationArtifact::ErrorExtractor::None) {
          const auto raw = TypeId(callee_type.arg1);
          auto code_error = foreignRepresentationType(raw);
          if (!code_error.hasValue())
            code_error = raw;
          auto expected_success = operation->error_success_payload ==
                                          interop::ForeignOperationArtifact::
                                              ErrorSuccessPayload::Void
                                      ? voidType()
                                      : raw;
          bool outcome_matches = true;
          if (operation->outcome_projection ==
                  interop::ForeignOperationArtifact::OutcomeProjection::
                      PosixRead ||
              operation->outcome_projection ==
                  interop::ForeignOperationArtifact::OutcomeProjection::
                      Win32Read ||
              operation->outcome_projection ==
                  interop::ForeignOperationArtifact::OutcomeProjection::Fread) {
            const auto outcome = canonicalReadOutcomeShape(shape->success);
            outcome_matches = outcome &&
                              type(outcome->data).kind == SemTypeKind::Slice &&
                              type(TypeId(type(outcome->data).arg0)).kind ==
                                  SemTypeKind::Integer &&
                              type(TypeId(type(outcome->data).arg0)).arg0 == 8;
            expected_success = shape->success;
          }
          const auto win32_error =
              operation->error_extractor ==
              interop::ForeignOperationArtifact::ErrorExtractor::Win32LastError;
          const auto error_matches =
              operation->error_extractor == interop::ForeignOperationArtifact::
                                                ErrorExtractor::ReturnedCode
                  ? shape->error == code_error
              : win32_error ? type(shape->error).kind == SemTypeKind::Integer &&
                                  type(shape->error).arg0 == 32 &&
                                  type(shape->error).arg1 == 0
                            : shape->error == i32Type();
          result_matches = outcome_matches &&
                           shape->success == expected_success && error_matches;
        }
      }
      if (!result_matches ||
          (!is_variadic && arguments.size() != parameters.size()) ||
          (is_variadic && arguments.size() < parameters.size())) {
        error = "call does not match its function signature";
        return false;
      }
      for (std::size_t argument = 0; argument < parameters.size(); ++argument) {
        if (arguments[argument].index >= insts_.size() ||
            inst(arguments[argument]).type != parameters[argument].index) {
          error = "call argument type does not match its parameter";
          return false;
        }
      }
      for (std::size_t argument = parameters.size();
           argument < arguments.size(); ++argument) {
        if (arguments[argument].index >= insts_.size()) {
          error = "variadic call has an invalid suffix argument";
          return false;
        }
        const auto kind = type(TypeId(inst(arguments[argument]).type)).kind;
        if (kind != SemTypeKind::Bool && kind != SemTypeKind::Integer &&
            kind != SemTypeKind::Float && kind != SemTypeKind::Reference &&
            kind != SemTypeKind::RawPointer &&
            kind != SemTypeKind::CFunctionPointer &&
            kind != SemTypeKind::CVariadicFunctionPointer) {
          error = "variadic call has an unsupported suffix argument";
          return false;
        }
      }
      break;
    }
    case SemInstKind::ForeignOperationProjectCompletion:
    case SemInstKind::ForeignOperationProjectWake: {
      const auto &callee = functionRef(FunctionRefId(value.arg0));
      const auto *operation =
          [&]() -> const interop::ForeignOperationArtifact * {
        if (callee.local_function.hasValue()) {
          const auto &declaration = functionDeclaration(callee.local_function);
          return declaration.interop_artifact
                     ? imports_.interopRegistry().resolve(
                           *declaration.interop_artifact)
                     : nullptr;
        }
        const auto *entity = imports_.tryGetEntity(callee.public_entity);
        return entity && entity->interop_artifact
                   ? imports_.interopRegistry().resolve(
                         *entity->interop_artifact)
                   : nullptr;
      }();
      const auto operands = instBlock(InstBlockId(value.arg1));
      if (!operation || operands.empty()) {
        error = "foreign operation projection has no verified operation";
        return false;
      }
      if (value.kind == SemInstKind::ForeignOperationProjectWake) {
        const auto fields = typeBlock(TypeBlockId(type(instruction_type).arg0));
        if (operation->completion_projection !=
                interop::ForeignCompletionProjectionKind::ScalarToWake ||
            type(instruction_type).kind != SemTypeKind::CallbackWake ||
            fields.size() != 1 || operands.size() != 3 ||
            type(TypeId(inst(operands[0]).type)).kind !=
                SemTypeKind::CallbackCompletion ||
            TypeId(inst(operands[0]).type) != fields[0] ||
            type(TypeId(inst(operands[1]).type)).kind !=
                SemTypeKind::CallbackAdapter ||
            (type(TypeId(inst(operands[2]).type)).kind != SemTypeKind::Bool &&
             type(TypeId(inst(operands[2]).type)).kind !=
                 SemTypeKind::Integer)) {
          error = "foreign operation wake projection has invalid operands";
          return false;
        }
      } else if (operation->completion_projection !=
                     interop::ForeignCompletionProjectionKind::
                         ScalarToCompletion ||
                 type(instruction_type).kind !=
                     SemTypeKind::CallbackCompletion ||
                 operands.size() != 2 ||
                 !foreignOperationStateOwner(TypeId(inst(operands[0]).type)) ||
                 foreignOperationStateOwner(TypeId(inst(operands[0]).type))
                         ->state != ForeignOperationStateKind::Subscription ||
                 TypeId(inst(operands[1]).type) !=
                     TypeId(type(callee.local_type).arg1)) {
        error = "foreign operation completion projection has invalid operands";
        return false;
      }
      break;
    }
    case SemInstKind::ForeignOperationPortProject: {
      const auto &callee = functionRef(FunctionRefId(value.arg0));
      const auto *operation =
          [&]() -> const interop::ForeignOperationArtifact * {
        if (callee.local_function.hasValue()) {
          const auto &declaration = functionDeclaration(callee.local_function);
          return declaration.interop_artifact
                     ? imports_.interopRegistry().resolve(
                           *declaration.interop_artifact)
                     : nullptr;
        }
        const auto *entity = imports_.tryGetEntity(callee.public_entity);
        return entity && entity->interop_artifact
                   ? imports_.interopRegistry().resolve(
                         *entity->interop_artifact)
                   : nullptr;
      }();
      const auto operands = instBlock(InstBlockId(value.arg1));
      if (!operation || (operands.size() != 2 && operands.size() != 5) ||
          inst(operands.back()).kind != SemInstKind::IntegerLiteral) {
        error = "foreign operation port projection has invalid operands";
        return false;
      }
      const auto lane_value = integer(IntegerId(inst(operands.back()).arg0));
      const auto parameters =
          typeBlock(TypeBlockId(type(callee.local_type).arg0));
      if (lane_value < 0 ||
          static_cast<std::uint64_t>(lane_value) > parameters.size()) {
        error = "foreign operation port projection has invalid lane";
        return false;
      }
      const auto lane = static_cast<std::uint32_t>(lane_value);
      const auto source_kind = type(TypeId(inst(operands[0]).type)).kind;
      const auto source_owner =
          foreignOperationStateOwner(TypeId(inst(operands[0]).type));
      const bool callback_lane =
          source_kind == SemTypeKind::CallbackAdapter &&
          std::ranges::find(operation->wake_callback_lanes, lane) !=
              operation->wake_callback_lanes.end();
      const bool carrier_lane =
          (source_kind == SemTypeKind::CallbackCompletion ||
           source_kind == SemTypeKind::CallbackWake ||
           (source_owner &&
            source_owner->state == ForeignOperationStateKind::Subscription)) &&
          operation->completion_carrier_lane == lane;
      const auto result_owner = foreignOperationStateOwner(instruction_type);
      const bool subscription_result =
          lane == parameters.size() &&
          operation->completion_projection ==
              interop::ForeignCompletionProjectionKind::ScalarToSubscription &&
          result_owner &&
          result_owner->state == ForeignOperationStateKind::Subscription &&
          operands.size() == 5 &&
          TypeId(inst(operands[3]).type) ==
              TypeId(type(callee.local_type).arg1);
      if ((!callback_lane && !carrier_lane && !subscription_result) ||
          (!subscription_result && (lane >= parameters.size() ||
                                    instruction_type != parameters[lane]))) {
        error = "foreign operation port projection disagrees with operation";
        return false;
      }
      break;
    }
    case SemInstKind::CompilerIntrinsicCall: {
      if (!verifyCompilerIntrinsicInstruction(
              value, instruction_type,
              [&](InstId operand) {
                return constant_enum_discriminant(
                    constant_enum_discriminant, operand);
              },
              error))
        return false;
      break;
    }
    case SemInstKind::InterfaceCall:
      if (!InstBlockId(value.arg1).hasValue()) {
        error = "interface call has no argument block";
        return false;
      }
      break;
    case SemInstKind::Placement: {
      const auto destination = InstId(value.arg0);
      const auto call = InstId(value.arg1);
      if (expressionCategory(sem_ir_, destination) != SemExprCategory::Place ||
          call.index >= insts_.size() ||
          (inst(call).kind != SemInstKind::Call &&
           inst(call).kind != SemInstKind::ForeignOperationCall)) {
        error = "placement instruction has no writable destination or call";
        return false;
      }
      const auto &callee = functionRef(FunctionRefId(inst(call).arg0));
      const auto constructor =
          callee.local_function.hasValue()
              ? functionSemanticContract(callee.local_function).role ==
                    CallableSemanticRole::Constructor
              : [&] {
                  const auto *entity =
                      imports_.tryGetEntity(callee.public_entity);
                  return entity && entity->semantic_contract.role ==
                                       CallableSemanticRole::Constructor;
                }();
      if (!constructor) {
        error = "placement call does not target a constructor";
        return false;
      }
      const auto destination_type = TypeId(inst(destination).type);
      const auto call_type = TypeId(inst(call).type);
      if (call_type == destination_type) {
        if (instruction_type != void_type_) {
          error = "infallible placement has a non-void outcome";
          return false;
        }
        break;
      }
      const auto &result = type(call_type);
      const auto &outcome = type(instruction_type);
      if (result.kind != SemTypeKind::Nominal ||
          outcome.kind != SemTypeKind::Nominal || result.arg0 != outcome.arg0) {
        error = "fallible placement does not use one canonical result type";
        return false;
      }
      const auto result_arguments = typeBlock(TypeBlockId(result.arg1));
      const auto outcome_arguments = typeBlock(TypeBlockId(outcome.arg1));
      if (result_arguments.size() != 2 || outcome_arguments.size() != 2 ||
          result_arguments[0] != destination_type ||
          outcome_arguments[0] != void_type_ ||
          result_arguments[1] != outcome_arguments[1]) {
        error = "fallible placement result does not match its destination";
        return false;
      }
      break;
    }
    case SemInstKind::FunctionValue: {
      const auto &callee = functionRef(FunctionRefId(value.arg0));
      if (instruction_type != callee.local_type ||
          type(instruction_type).kind != SemTypeKind::Function ||
          (callee.generic.hasValue() &&
           callee.specific ==
               values_->generics().generic(callee.generic).self_specific)) {
        error = "function value has an invalid callable target";
        return false;
      }
      bool valid = false;
      if (callee.local_function.hasValue()) {
        const auto function = callee.local_function;
        const auto &declaration = functionDeclaration(function);
        valid = declaration.kind == SemCallableDeclarationKind::Definition &&
                !declaration.is_unsafe &&
                functionSemanticContract(function).domain ==
                    CallableSemanticDomain::Ordinary;
      } else if (const auto *entity =
                     imports_.tryGetEntity(callee.public_entity)) {
        valid =
            entity->kind == PublicEntityKind::Function &&
            entity->generic_parameter_count == 0 &&
            entity->declaration_kind ==
                PublicCallableDeclarationKind::Definition &&
            !entity->is_unsafe &&
            entity->semantic_contract.domain ==
                CallableSemanticDomain::Ordinary &&
            entity->member_kind != PublicFunctionArtifact::MemberKind::Instance;
      }
      if (!valid) {
        error =
            "function value target is not a safe concrete ordinary function";
        return false;
      }
      break;
    }
    case SemInstKind::IndirectCall: {
      const auto callee_type = first_inst_type();
      const auto &callable = type(callee_type);
      const auto arguments = instBlock(InstBlockId(value.arg1));
      if (callable.kind != SemTypeKind::Function ||
          instruction_type.index != callable.arg1) {
        error = "indirect call target does not have a matching function type";
        return false;
      }
      const auto parameters = typeBlock(TypeBlockId(callable.arg0));
      if (arguments.size() != parameters.size()) {
        error = "indirect call does not match its function signature";
        return false;
      }
      for (std::size_t argument_index = 0; argument_index < arguments.size();
           ++argument_index) {
        if (arguments[argument_index].index >= insts_.size() ||
            inst(arguments[argument_index]).type !=
                parameters[argument_index].index) {
          error = "indirect call argument type does not match its parameter";
          return false;
        }
      }
      break;
    }
    case SemInstKind::ForeignResourceWrap: {
      const auto input_type = first_inst_type();
      const auto &result = type(instruction_type);
      if (result.kind == SemTypeKind::Nominal) {
        const auto &nominal = nominalType(NominalTypeId(result.arg0));
        if (nominal.kind != NominalKind::ForeignResource ||
            (input_type != nominal.foreign_handle_type &&
             input_type != nominal.foreign_registration_storage_type)) {
          error = "foreign resource wrap has inconsistent handle facts";
          return false;
        }
      } else if (result.kind == SemTypeKind::ForeignCompletion) {
        const auto &nominal = nominalType(NominalTypeId(result.arg0));
        if (input_type != nominal.foreign_completion_handle_type &&
            input_type != nominal.foreign_completion_storage_type) {
          error = "foreign completion wrap has inconsistent handle facts";
          return false;
        }
      } else if (result.kind == SemTypeKind::ForeignWake) {
        const auto &nominal = nominalType(NominalTypeId(result.arg0));
        if (input_type != nominal.foreign_wake_storage_type) {
          error = "foreign wake wrap has inconsistent storage facts";
          return false;
        }
      } else {
        error = "foreign resource wrap has an invalid result type";
        return false;
      }
      break;
    }
    case SemInstKind::ForeignResourceUnwrap: {
      const auto &source = type(first_inst_type());
      if (source.kind == SemTypeKind::Nominal) {
        const auto &nominal = nominalType(NominalTypeId(source.arg0));
        if (nominal.kind != NominalKind::ForeignResource ||
            (instruction_type != nominal.foreign_handle_type &&
             instruction_type != nominal.foreign_registration_storage_type)) {
          error = "foreign resource unwrap has inconsistent handle facts";
          return false;
        }
      } else if (source.kind == SemTypeKind::ForeignCompletion) {
        const auto &nominal = nominalType(NominalTypeId(source.arg0));
        if (instruction_type != nominal.foreign_completion_handle_type &&
            instruction_type != nominal.foreign_completion_storage_type) {
          error = "foreign completion unwrap has inconsistent handle facts";
          return false;
        }
      } else if (source.kind == SemTypeKind::ForeignWake) {
        const auto &nominal = nominalType(NominalTypeId(source.arg0));
        if (instruction_type != nominal.foreign_wake_storage_type) {
          error = "foreign wake unwrap has inconsistent storage facts";
          return false;
        }
      } else {
        error = "foreign resource unwrap has an invalid source type";
        return false;
      }
      break;
    }
    case SemInstKind::ForeignResourceValid: {
      const auto &source = type(first_inst_type());
      if (source.kind != SemTypeKind::Nominal ||
          nominalType(NominalTypeId(source.arg0)).kind !=
              NominalKind::ForeignResource ||
          instruction_type != bool_type_) {
        error = "foreign resource valid has inconsistent semantic facts";
        return false;
      }
      break;
    }
    case SemInstKind::CallbackAdapter: {
      const auto &callback = type(instruction_type);
      const auto &source = functionRef(FunctionRefId(value.arg0));
      if (callback.kind != SemTypeKind::CFunctionPointer ||
          source.generic.hasValue()) {
        error = "callback adapter has an invalid source or target";
        return false;
      }
      const auto &source_type = type(source.local_type);
      const auto callback_parameters = typeBlock(TypeBlockId(callback.arg0));
      const auto source_parameters = typeBlock(TypeBlockId(source_type.arg0));
      if (!isConcreteReverseTarget(FunctionRefId(value.arg0)) ||
          source_type.kind != SemTypeKind::Function ||
          source_type.arg1 != callback.arg1 ||
          !std::ranges::equal(source_parameters, callback_parameters)) {
        error = "callback adapter does not exactly match its source function";
        return false;
      }
      break;
    }
    case SemInstKind::MakeCallbackAdapter: {
      const auto &adapter = type(instruction_type);
      const auto operands = instBlock(InstBlockId(value.arg0));
      if (adapter.kind != SemTypeKind::CallbackAdapter ||
          operands.size() != 3) {
        error = "callback adapter construction has invalid semantic facts";
        return false;
      }
      const auto fields = typeBlock(TypeBlockId(adapter.arg0));
      for (std::size_t field = 0; field < fields.size(); ++field)
        if (TypeId(inst(operands[field]).type) != fields[field]) {
          error = "callback adapter construction has a mismatched field";
          return false;
        }
      break;
    }
    case SemInstKind::CallbackAdapterCall: {
      const auto adapter_type = first_inst_type();
      if (type(adapter_type).kind != SemTypeKind::CallbackAdapter) {
        error = "callback adapter call has a non-adapter callee";
        return false;
      }
      const auto fields = typeBlock(TypeBlockId(type(adapter_type).arg0));
      const auto entry_type = fields[0];
      const auto parameters = typeBlock(TypeBlockId(type(entry_type).arg0));
      const auto context = callbackContextParameter(entry_type);
      const auto arguments = instBlock(InstBlockId(value.arg1));
      if (context >= parameters.size() ||
          arguments.size() + 1 != parameters.size() ||
          instruction_type != TypeId(type(entry_type).arg1)) {
        error = "callback adapter call does not match its entry signature";
        return false;
      }
      for (std::size_t argument = 0; argument < arguments.size(); ++argument) {
        const auto parameter = argument < context ? argument : argument + 1;
        if (TypeId(inst(arguments[argument]).type) != parameters[parameter]) {
          error = "callback adapter call argument has the wrong type";
          return false;
        }
      }
      break;
    }
    case SemInstKind::ForeignFunctionRef: {
      const auto &callback = type(instruction_type);
      const auto &source = functionRef(FunctionRefId(value.arg0));
      if (source.generic.hasValue() ||
          (callback.kind != SemTypeKind::CFunctionPointer &&
           callback.kind != SemTypeKind::CVariadicFunctionPointer)) {
        error = "foreign function reference has an invalid source or type";
        return false;
      }
      const auto &source_type = type(source.local_type);
      const auto callback_parameters = typeBlock(TypeBlockId(callback.arg0));
      const auto source_parameters = typeBlock(TypeBlockId(source_type.arg0));
      CallableOwnershipSummary ownership;
      bool has_ownership = false;
      const ForeignAbiSignature *signature = nullptr;
      bool valid_source = false;
      if (source.local_function.hasValue()) {
        const auto &declaration = functionDeclaration(source.local_function);
        valid_source =
            declaration.kind == SemCallableDeclarationKind::Foreign &&
            declaration.is_unsafe && declaration.foreign_abi.hasValue() &&
            identifier(declaration.foreign_abi) == "C" &&
            declaration.foreign_signature.has_value();
        if (declaration.foreign_signature)
          signature = &*declaration.foreign_signature;
        ownership = declaration.declared_contract
                        ? effectiveCallableOwnershipSummary(
                              *declaration.declared_contract)
                        : functionOwnership(source.local_function);
        has_ownership = true;
      } else if (const auto *entity =
                     importIRs().tryGetEntity(source.public_entity)) {
        valid_source = entity->kind == PublicEntityKind::Function &&
                       entity->generic_parameter_count == 0 &&
                       entity->declaration_kind ==
                           PublicCallableDeclarationKind::Foreign &&
                       entity->is_unsafe && entity->foreign_abi.hasValue() &&
                       identifier(entity->foreign_abi) == "C" &&
                       entity->foreign_signature.has_value();
        if (entity->foreign_signature)
          signature = &*entity->foreign_signature;
        ownership = entity->ownership_summary;
        has_ownership = true;
      }
      const auto physical = [&](TypeId source_type) {
        const auto representation = foreignRepresentationType(source_type);
        return representation.hasValue() ? representation : source_type;
      };
      const auto source_result = TypeId(source_type.arg1);
      if (physical(source_result) != source_result)
        ownership.returns_owned = true;
      const auto equivalent_parameters =
          callback_parameters.size() == source_parameters.size() &&
          std::ranges::equal(
              callback_parameters, source_parameters,
              [&](TypeId callback_parameter, TypeId source_parameter) {
                return callback_parameter == physical(source_parameter);
              });
      if (!valid_source || !signature || !has_ownership ||
          source_type.kind != SemTypeKind::Function ||
          TypeId(callback.arg1) != physical(source_result) ||
          !equivalent_parameters ||
          (callback.kind == SemTypeKind::CVariadicFunctionPointer) !=
              signature->is_variadic ||
          callbackContextParameter(instruction_type) !=
              core::AnyId::InvalidIndex ||
          callbackContract(instruction_type) != ownership) {
        error = "foreign function reference disagrees with its declaration";
        return false;
      }
      break;
    }
    case SemInstKind::CallbackRegistrationBinding:
      if (!NameId(value.arg0).hasValue() ||
          instruction_type != TypeId(inst(InstId(value.arg1)).type)) {
        error = "callback registration binding has invalid semantic facts";
        return false;
      }
      break;
    case SemInstKind::MakeCallbackRegistration: {
      const auto &registration = type(instruction_type);
      const auto operands = instBlock(InstBlockId(value.arg0));
      if (registration.kind != SemTypeKind::CallbackRegistration) {
        error = "callback registration construction has invalid semantic facts";
        return false;
      }
      const auto bindings = callbackRegistrationBindings(instruction_type);
      const auto fields = typeBlock(TypeBlockId(registration.arg0));
      const auto fixed_fields = fields.size() == 5 ? 4U : fields.size() - 1U;
      if ((fields.size() != 5 && fields.size() != 7 && fields.size() != 8 &&
           fields.size() != 10) ||
          operands.size() != fixed_fields + bindings.size()) {
        error = "callback registration construction has invalid fields";
        return false;
      }
      constexpr std::array<std::size_t, 9> field_indices{0, 2, 3, 4, 5,
                                                         6, 7, 8, 9};
      for (std::size_t operand = 0; operand < fixed_fields; ++operand) {
        if (TypeId(inst(operands[operand]).type) !=
            fields[field_indices[operand]]) {
          error = "callback registration construction has a mismatched field";
          return false;
        }
      }
      const auto register_parameters =
          typeBlock(TypeBlockId(type(fields[2]).arg0));
      std::vector<bool> seen(bindings.size());
      for (const auto operand : operands.subspan(fixed_fields)) {
        const auto &binding_inst = inst(operand);
        if (binding_inst.kind != SemInstKind::CallbackRegistrationBinding) {
          error = "callback registration construction has an invalid binding";
          return false;
        }
        const auto label = identifier(name(NameId(binding_inst.arg0)).text);
        const auto found = std::ranges::find(
            bindings, label, &CallbackRegistrationBinding::name);
        if (found == bindings.end()) {
          error = "callback registration construction has an unknown binding";
          return false;
        }
        const auto binding_index =
            static_cast<std::size_t>(found - bindings.begin());
        if (seen[binding_index] ||
            found->parameter_index >= register_parameters.size() ||
            TypeId(binding_inst.type) !=
                register_parameters[found->parameter_index]) {
          error = "callback registration construction has a duplicate or "
                  "mistyped binding";
          return false;
        }
        seen[binding_index] = true;
      }
      if (std::ranges::any_of(seen, [](bool value) { return !value; })) {
        error = "callback registration construction has a missing binding";
        return false;
      }
      break;
    }
    case SemInstKind::CallbackRegistrationActive:
      if (instruction_type != bool_type_ ||
          type(first_inst_type()).kind != SemTypeKind::CallbackRegistration) {
        error = "callback registration active test has invalid types";
        return false;
      }
      break;
    case SemInstKind::CallbackUnregister:
    case SemInstKind::CallbackCancel:
      if (instruction_type != void_type_ ||
          type(first_inst_type()).kind != SemTypeKind::CallbackRegistration ||
          (inst(InstId(value.arg0)).kind != SemInstKind::Move &&
           !is_moved_resource_projection(InstId(value.arg0)))) {
        error = "callback registration terminal operation has invalid types";
        return false;
      }
      break;
    case SemInstKind::CallbackCancelAsync: {
      const auto registration_type = first_inst_type();
      if ((inst(InstId(value.arg0)).kind != SemInstKind::Move &&
           !is_moved_resource_projection(InstId(value.arg0))) ||
          type(registration_type).kind != SemTypeKind::CallbackRegistration ||
          type(instruction_type).kind != SemTypeKind::CallbackCompletion) {
        error = "asynchronous callback cancellation has invalid types";
        return false;
      }
      const auto registration_fields =
          typeBlock(TypeBlockId(type(registration_type).arg0));
      const auto completion_fields =
          typeBlock(TypeBlockId(type(instruction_type).arg0));
      if ((registration_fields.size() != 7 && registration_fields.size() != 8 &&
           registration_fields.size() != 10) ||
          completion_fields.size() != (registration_fields.size() == 10
                                           ? 7U
                                           : registration_fields.size() - 3U) ||
          completion_fields[0] != registration_fields[0] ||
          completion_fields[1] != registration_fields[1] ||
          completion_fields[2] != registration_fields[1] ||
          completion_fields[3] != registration_fields[6] ||
          (registration_fields.size() == 8 &&
           completion_fields[4] != registration_fields[7]) ||
          (registration_fields.size() == 10 &&
           (completion_fields[4] != registration_fields[7] ||
            completion_fields[5] != registration_fields[8] ||
            completion_fields[6] != registration_fields[9] ||
            callbackArmParameters(instruction_type) !=
                callbackArmParameters(registration_type) ||
            callbackDetachParameters(instruction_type) !=
                callbackDetachParameters(registration_type))) ||
          callbackCompletionAuthority(instruction_type) !=
              callbackRegistrationAuthority(registration_type)) {
        error = "asynchronous callback cancellation disagrees with its type";
        return false;
      }
      break;
    }
    case SemInstKind::CallbackCompletionPending:
      if (instruction_type != bool_type_ ||
          type(first_inst_type()).kind != SemTypeKind::CallbackCompletion) {
        error = "callback completion pending test has invalid types";
        return false;
      }
      break;
    case SemInstKind::CallbackCompletionPoll: {
      const auto operand = InstId(value.arg0);
      const auto operand_kind = inst(operand).kind;
      const auto is_place = operand_kind == SemInstKind::NameRef ||
                            operand_kind == SemInstKind::StructFieldAccess ||
                            operand_kind == SemInstKind::UnionFieldAccess ||
                            operand_kind == SemInstKind::Index ||
                            operand_kind == SemInstKind::Dereference;
      if (instruction_type != bool_type_ ||
          type(first_inst_type()).kind != SemTypeKind::CallbackCompletion ||
          (typeBlock(TypeBlockId(type(first_inst_type()).arg0)).size() != 5 &&
           typeBlock(TypeBlockId(type(first_inst_type()).arg0)).size() != 7) ||
          (!is_place && !is_resource_place_projection(operand))) {
        error = "callback completion poll has invalid types";
        return false;
      }
      break;
    }
    case SemInstKind::CallbackWait:
      if (instruction_type != void_type_ ||
          (type(first_inst_type()).kind != SemTypeKind::CallbackCompletion &&
           type(first_inst_type()).kind != SemTypeKind::CallbackWake) ||
          (inst(InstId(value.arg0)).kind != SemInstKind::Move &&
           !is_moved_resource_projection(InstId(value.arg0)))) {
        error = "callback wait has invalid types";
        return false;
      }
      break;
