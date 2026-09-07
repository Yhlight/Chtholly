// Included inside SemIRVerificationContext::verifyInstructions; the shared
// read-only context locals and diagnostics remain owned by the caller.

    case SemInstKind::CoroutineSuspend:
      if (instruction_type != void_type_ ||
          (type(first_inst_type()).kind != SemTypeKind::CallbackWake &&
           type(first_inst_type()).kind !=
               SemTypeKind::CoroutineTaskCompletion) ||
          (inst(InstId(value.arg0)).kind != SemInstKind::Move &&
           !is_moved_resource_projection(InstId(value.arg0)))) {
        error = "coroutine suspend has invalid types";
        return false;
      }
      break;
    case SemInstKind::CoroutineTaskCompletionArm:
      if (type(first_inst_type()).kind != SemTypeKind::CoroutineTask ||
          type(instruction_type).kind != SemTypeKind::CoroutineChecked ||
          coroutineCheckedPayloadType(instruction_type) !=
              coroutine_task_completion_type_) {
        error = "coroutine task completion arm has invalid types";
        return false;
      }
      break;
    case SemInstKind::CoroutineTaskCompletionReady:
      if (instruction_type != bool_type_ ||
          first_inst_type() != coroutine_task_completion_type_) {
        error = "coroutine task completion ready test has invalid types";
        return false;
      }
      break;
    case SemInstKind::CoroutineTaskCompletionDetach:
      if (instruction_type != void_type_ ||
          first_inst_type() != coroutine_task_completion_type_ ||
          inst(InstId(value.arg0)).kind != SemInstKind::Move) {
        error = "coroutine task completion detach has invalid types";
        return false;
      }
      break;
    case SemInstKind::CoroutineTaskCompletionSetCreate: {
      const auto operands = instBlock(InstBlockId(value.arg0));
      const auto set_type =
          type(instruction_type).kind == SemTypeKind::CoroutineChecked
              ? coroutineCheckedPayloadType(instruction_type)
              : TypeId::invalid();
      if (!set_type.hasValue() ||
          type(set_type).kind != SemTypeKind::CoroutineTaskCompletionSet ||
          !isCompletionAggregationProvider(
              completionSetElementType(set_type)) ||
          coroutineTaskCompletionCapacity(set_type) != operands.size() ||
          std::ranges::any_of(operands, [&](InstId operand) {
            return TypeId(inst(operand).type) !=
                       completionSetElementType(set_type) ||
                   inst(operand).kind != SemInstKind::Move;
          })) {
        error = "coroutine completion set has invalid moved operands";
        return false;
      }
      break;
    }
    case SemInstKind::CoroutineTaskCompletionWaitAll:
    case SemInstKind::CoroutineTaskCompletionSelect:
    case SemInstKind::CoroutineTaskCompletionRace: {
      const auto set_type = first_inst_type();
      const auto capacity =
          type(set_type).kind == SemTypeKind::CoroutineTaskCompletionSet
              ? coroutineTaskCompletionCapacity(set_type)
              : core::AnyId::InvalidIndex;
      const auto is_wait_all =
          value.kind == SemInstKind::CoroutineTaskCompletionWaitAll;
      const auto payload = !is_wait_all && type(instruction_type).kind ==
                                               SemTypeKind::CoroutineChecked
                               ? coroutineCheckedPayloadType(instruction_type)
                               : TypeId::invalid();
      const auto payload_matches =
          value.kind == SemInstKind::CoroutineTaskCompletionSelect
              ? payload.hasValue() &&
                    type(payload).kind == SemTypeKind::CoroutineTaskSelection &&
                    coroutineTaskCompletionCapacity(payload) == capacity &&
                    completionSetElementType(payload) ==
                        completionSetElementType(set_type)
              : payload == i32_type_;
      if (capacity == core::AnyId::InvalidIndex ||
          inst(InstId(value.arg0)).kind != SemInstKind::Move ||
          (is_wait_all && instruction_type != void_type_) ||
          (!is_wait_all &&
           (type(instruction_type).kind != SemTypeKind::CoroutineChecked ||
            !payload_matches))) {
        error = "coroutine completion combination has invalid types";
        return false;
      }
      break;
    }
    case SemInstKind::CoroutineTaskSelectionWinner:
      if (instruction_type != i32_type_ ||
          type(first_inst_type()).kind != SemTypeKind::CoroutineTaskSelection) {
        error = "coroutine task selection winner has invalid types";
        return false;
      }
      break;
    case SemInstKind::CoroutineTaskSelectionTakeRemaining:
      if (type(first_inst_type()).kind != SemTypeKind::CoroutineTaskSelection ||
          type(instruction_type).kind !=
              SemTypeKind::CoroutineTaskCompletionSet ||
          coroutineTaskCompletionCapacity(first_inst_type()) !=
              coroutineTaskCompletionCapacity(instruction_type) ||
          completionSetElementType(first_inst_type()) !=
              completionSetElementType(instruction_type) ||
          inst(InstId(value.arg0)).kind != SemInstKind::Move) {
        error = "coroutine task selection remaining take has invalid types";
        return false;
      }
      break;
    case SemInstKind::CoroutineCancellationCheck:
      if (instruction_type != void_type_) {
        error = "coroutine cancellation check has invalid type";
        return false;
      }
      break;
    case SemInstKind::CoroutineRuntimeFault: {
      const auto reason = integer(IntegerId(value.arg0));
      if (instruction_type != never_type_ || reason < 1 || reason > 7) {
        error = "coroutine runtime fault has invalid semantic facts";
        return false;
      }
      break;
    }
    case SemInstKind::CoroutineExecutorSwitch:
      if (instruction_type != i32_type_ ||
          first_inst_type() != coroutine_executor_type_) {
        error = "coroutine executor switch has invalid types";
        return false;
      }
      break;
    case SemInstKind::CoroutineTaskCreate:
    case SemInstKind::CoroutineChildTaskCreate: {
      const auto &callee = functionRef(FunctionRefId(value.arg0));
      const auto &callee_type = type(callee.local_type);
      if (callee_type.kind != SemTypeKind::AsyncFunction ||
          type(instruction_type).kind != SemTypeKind::CoroutineChecked) {
        error = "coroutine task creation has invalid typed operands";
        return false;
      }
      PublicEntityId constructor_entity;
      bool pending_source_constructor = false;
      if (callee.local_function.hasValue()) {
        const auto &target = function(callee.local_function);
        constructor_entity = coroutineConstructorEntity(callee.local_function);
        if ((target.flags & SemFunctionAsync) == 0 ||
            (target.flags & (SemFunctionTemplate | SemFunctionSpecific)) != 0) {
          error = "coroutine task creation requires a bound local scaffold";
          return false;
        }
        pending_source_constructor =
            !constructor_entity.hasValue() &&
            (target.flags & SemFunctionCoroutineScaffold) == 0;
        if (!constructor_entity.hasValue() && !pending_source_constructor) {
          error = "coroutine task creation requires a bound local scaffold";
          return false;
        }
      } else {
        constructor_entity = callee.public_entity;
      }
      const auto *constructor = imports_.tryGetEntity(constructor_entity);
      if (!pending_source_constructor &&
          (!constructor || constructor->kind != PublicEntityKind::Function ||
           constructor->execution_kind != PublicFunctionExecutionKind::Async ||
           constructor->coroutine_constructor !=
               PublicCoroutineConstructorABI{1, true, true, true, true})) {
        error = "coroutine task creation has no compatible constructor ABI";
        return false;
      }
      const auto checked_payload =
          coroutineCheckedPayloadType(instruction_type);
      if (type(checked_payload).kind != SemTypeKind::CoroutineTask ||
          coroutineTaskSuccessType(checked_payload) !=
              asyncSuccessType(callee.local_type) ||
          coroutineTaskErrorType(checked_payload) !=
              asyncErrorType(callee.local_type)) {
        error = "coroutine task creation result disagrees with its callee";
        return false;
      }
      const auto operands = instBlock(InstBlockId(value.arg1));
      const auto parameters = typeBlock(TypeBlockId(callee_type.arg0));
      const auto scope_prefix =
          value.kind == SemInstKind::CoroutineTaskCreate ? 1U : 0U;
      if (operands.size() != parameters.size() + scope_prefix ||
          (scope_prefix != 0 &&
           TypeId(inst(operands.front()).type) != coroutine_scope_type_)) {
        error = "coroutine task creation has invalid scope or arity";
        return false;
      }
      for (std::size_t argument = 0; argument < parameters.size(); ++argument) {
        const auto operand = operands[argument + scope_prefix];
        if (TypeId(inst(operand).type) != parameters[argument]) {
          error = "coroutine task creation argument type does not match";
          return false;
        }
        if (typeRepresentation(parameters[argument]).ownership ==
                OwnershipReprKind::Owned &&
            inst(operand).kind != SemInstKind::Move) {
          error =
              "coroutine task creation must explicitly move owned arguments";
          return false;
        }
      }
      break;
    }
    case SemInstKind::CoroutineTaskScope:
      if (instruction_type != void_type_ ||
          !InstBlockId(value.arg0).hasValue()) {
        error = "coroutine task scope has inconsistent semantic facts";
        return false;
      }
      break;
    case SemInstKind::CoroutineTaskJoin:
      if (instruction_type != i32_type_ ||
          type(first_inst_type()).kind != SemTypeKind::CoroutineTask) {
        error = "coroutine task join has invalid types";
        return false;
      }
      break;
    case SemInstKind::CoroutineTaskQuery: {
      if (type(first_inst_type()).kind != SemTypeKind::CoroutineTask ||
          type(instruction_type).kind != SemTypeKind::CoroutineChecked) {
        error = "coroutine task query has invalid types";
        return false;
      }
      const auto payload = coroutineCheckedPayloadType(instruction_type);
      if (type(payload).kind != SemTypeKind::CoroutineTaskOutcome ||
          coroutineTaskSuccessType(payload) !=
              coroutineTaskSuccessType(first_inst_type()) ||
          coroutineTaskErrorType(payload) !=
              coroutineTaskErrorType(first_inst_type())) {
        error = "coroutine task query outcome disagrees with its task";
        return false;
      }
      break;
    }
    case SemInstKind::CoroutineTaskTakeResult:
    case SemInstKind::CoroutineTaskTakeError: {
      const auto task_type = first_inst_type();
      if (type(task_type).kind != SemTypeKind::CoroutineTask ||
          type(instruction_type).kind != SemTypeKind::CoroutineChecked) {
        error = "coroutine task payload take has invalid types";
        return false;
      }
      const auto expected =
          value.kind == SemInstKind::CoroutineTaskTakeResult
              ? std::optional<TypeId>(coroutineTaskSuccessType(task_type))
              : coroutineTaskErrorType(task_type);
      if (!expected || *expected == void_type_ ||
          coroutineCheckedPayloadType(instruction_type) != *expected) {
        error = "coroutine task payload take disagrees with its task";
        return false;
      }
      break;
    }
    case SemInstKind::CoroutineCheckedStatus:
      if (instruction_type != i32_type_ ||
          type(first_inst_type()).kind != SemTypeKind::CoroutineChecked) {
        error = "coroutine checked status has invalid types";
        return false;
      }
      break;
    case SemInstKind::CoroutineCheckedTake:
      if (type(first_inst_type()).kind != SemTypeKind::CoroutineChecked ||
          instruction_type != coroutineCheckedPayloadType(first_inst_type())) {
        error = "coroutine checked extraction has invalid types";
        return false;
      }
      break;
    case SemInstKind::CoroutineOutcomeCompleted:
    case SemInstKind::CoroutineOutcomeFailed:
    case SemInstKind::CoroutineOutcomeCancelled:
      if (instruction_type != bool_type_ ||
          type(first_inst_type()).kind != SemTypeKind::CoroutineTaskOutcome ||
          (value.kind == SemInstKind::CoroutineOutcomeFailed &&
           !coroutineTaskErrorType(first_inst_type()))) {
        error = "coroutine task outcome predicate has invalid types";
        return false;
      }
      break;
    case SemInstKind::MakeCallbackWake: {
      if (type(instruction_type).kind != SemTypeKind::CallbackWake) {
        error = "callback wake construction has invalid result type";
        return false;
      }
      const auto operands = instBlock(InstBlockId(value.arg0));
      const auto wake_fields =
          typeBlock(TypeBlockId(type(instruction_type).arg0));
      if (operands.size() != 2 || wake_fields.size() != 1 ||
          type(TypeId(inst(operands[0]).type)).kind !=
              SemTypeKind::CallbackCompletion ||
          TypeId(inst(operands[0]).type) != wake_fields[0] ||
          type(TypeId(inst(operands[1]).type)).kind !=
              SemTypeKind::CallbackAdapter ||
          (inst(operands[0]).kind != SemInstKind::Move &&
           !is_moved_resource_projection(operands[0])) ||
          inst(operands[1]).kind != SemInstKind::Move) {
        error = "callback wake construction has invalid operands";
        return false;
      }
      break;
    }
    case SemInstKind::CallbackWakeReady:
      if (instruction_type != bool_type_ ||
          type(first_inst_type()).kind != SemTypeKind::CallbackWake) {
        error = "callback wake ready test has invalid types";
        return false;
      }
      break;
    case SemInstKind::CallbackDetach: {
      const auto operand_type = first_inst_type();
      const auto operand_kind = type(operand_type).kind;
      TypeId completion_type = operand_type;
      if (operand_kind == SemTypeKind::CallbackWake) {
        const auto fields = typeBlock(TypeBlockId(type(operand_type).arg0));
        if (fields.size() == 1)
          completion_type = fields[0];
      }
      if (instruction_type != void_type_ ||
          (operand_kind != SemTypeKind::CallbackCompletion &&
           operand_kind != SemTypeKind::CallbackWake) ||
          typeBlock(TypeBlockId(type(completion_type).arg0)).size() != 7 ||
          (inst(InstId(value.arg0)).kind != SemInstKind::Move &&
           !is_moved_resource_projection(InstId(value.arg0)))) {
        error = "callback detach has invalid types";
        return false;
      }
      break;
    }
