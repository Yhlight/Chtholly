// Included inside SemIRVerificationContext::verifyInstructions; the shared
// read-only context locals and diagnostics remain owned by the caller.

    case SemInstKind::DiscardValue:
      if (instruction_type != void_type_ || !InstId(value.arg0).hasValue() ||
          value.arg0 >= insts_.size()) {
        error = "discarded value has invalid semantic facts";
        return false;
      }
      break;
    case SemInstKind::Yield:
      if (instruction_type != void_type_ || !InstId(value.arg0).hasValue() ||
          value.arg0 >= insts_.size()) {
        error = "yield has invalid semantic facts";
        return false;
      }
      break;
    case SemInstKind::IndirectForeignCall: {
      const auto callee_type = first_inst_type();
      const auto &callback = type(callee_type);
      if (callback.kind != SemTypeKind::CFunctionPointer &&
          callback.kind != SemTypeKind::CVariadicFunctionPointer) {
        error = "indirect foreign call has a non-callback callee";
        return false;
      }
      const auto arguments = instBlock(InstBlockId(value.arg1));
      const auto parameters = typeBlock(TypeBlockId(callback.arg0));
      const auto variadic =
          callback.kind == SemTypeKind::CVariadicFunctionPointer;
      if (instruction_type.index != callback.arg1 ||
          (!variadic && arguments.size() != parameters.size()) ||
          (variadic && arguments.size() < parameters.size())) {
        error = "indirect foreign call does not match its callback type";
        return false;
      }
      for (std::size_t argument_index = 0; argument_index < parameters.size();
           ++argument_index) {
        if (arguments[argument_index].index >= insts_.size() ||
            TypeId(inst(arguments[argument_index]).type) !=
                parameters[argument_index]) {
          error = "indirect foreign call argument has the wrong type";
          return false;
        }
      }
      for (std::size_t argument_index = parameters.size();
           argument_index < arguments.size(); ++argument_index) {
        if (arguments[argument_index].index >= insts_.size()) {
          error = "indirect variadic call has an invalid suffix argument";
          return false;
        }
        const auto kind =
            type(TypeId(inst(arguments[argument_index]).type)).kind;
        if (kind != SemTypeKind::Bool && kind != SemTypeKind::Integer &&
            kind != SemTypeKind::Float && kind != SemTypeKind::RawPointer &&
            kind != SemTypeKind::CFunctionPointer &&
            kind != SemTypeKind::CVariadicFunctionPointer) {
          error = "indirect variadic call has an unsupported suffix argument";
          return false;
        }
      }
      break;
    }
    case SemInstKind::Return:
      if (instruction_type != void_type_) {
        error = "return instruction must have void result type";
        return false;
      }
      break;
    case SemInstKind::Assert:
      if (instruction_type != void_type_ || first_inst_type() != bool_type_ ||
          integer(IntegerId(value.arg1)) !=
              static_cast<std::uint32_t>(
                  UnrecoverableFailureReason::Assertion)) {
        error = "assert instruction has inconsistent semantic facts";
        return false;
      }
      break;
    case SemInstKind::UnrecoverableFailure: {
      const auto reason = integer(IntegerId(value.arg0));
      if (instruction_type != never_type_ ||
          !isValidUnrecoverableFailureReason(reason)) {
        error = "unrecoverable failure has inconsistent semantic facts";
        return false;
      }
      break;
    }
    case SemInstKind::CoroutineReturnSuccess:
    case SemInstKind::CoroutineReturnError:
      if (instruction_type != never_type_) {
        error = "coroutine payload terminal must have never result type";
        return false;
      }
      break;
    case SemInstKind::CoroutineReturnCancelled:
      if (instruction_type != never_type_) {
        error = "coroutine cancellation terminal must have never result type";
        return false;
      }
      break;
    case SemInstKind::Defer:
      if (instruction_type != void_type_ ||
          !InstBlockId(value.arg0).hasValue()) {
        error = "defer instruction has inconsistent semantic facts";
        return false;
      }
      break;
    case SemInstKind::If: {
      const auto arms = instBlock(InstBlockId(value.arg1));
      if (first_inst_type() != bool_type_ || arms.empty() || arms.size() > 2 ||
          (instruction_type != void_type_ && arms.size() != 2) ||
          std::ranges::any_of(arms, [&](InstId arm) {
            if (inst(arm).kind != SemInstKind::IfArm ||
                TypeId(inst(arm).type) != instruction_type)
              return true;
            if (instruction_type == void_type_)
              return false;
            const auto body = instBlock(InstBlockId(inst(arm).arg0));
            if (body.empty())
              return true;
            const auto &terminal = inst(body.back());
            return terminal.kind != SemInstKind::Return &&
                   terminal.kind != SemInstKind::Break &&
                   terminal.kind != SemInstKind::Continue &&
                   expressionCategory(sem_ir_, body.back()) !=
                       SemExprCategory::Diverging &&
                   (terminal.kind != SemInstKind::Yield ||
                    TypeId(inst(InstId(terminal.arg0)).type) !=
                        instruction_type);
          })) {
        error = "if instruction has inconsistent semantic facts";
        return false;
      }
      break;
    }
    case SemInstKind::ScopedBlock: {
      if (!InstBlockId(value.arg0).hasValue() ||
          value.arg0 >= inst_blocks_.size()) {
        error = "scoped block has an invalid body";
        return false;
      }
      const auto body = instBlock(InstBlockId(value.arg0));
      if (instruction_type != void_type_ && instruction_type != never_type_ &&
          (body.empty() || (inst(body.back()).kind != SemInstKind::Yield &&
                            expressionCategory(sem_ir_, body.back()) !=
                                SemExprCategory::Diverging))) {
        error = "scoped block has inconsistent semantic facts";
        return false;
      }
      if (!body.empty() && inst(body.back()).kind == SemInstKind::Yield &&
          TypeId(inst(InstId(inst(body.back()).arg0)).type) !=
              instruction_type) {
        error = "scoped block yield does not match its result type";
        return false;
      }
      break;
    }
    case SemInstKind::IfArm:
      if (!InstBlockId(value.arg0).hasValue()) {
        error = "if arm has inconsistent semantic facts";
        return false;
      }
      break;
    case SemInstKind::While: {
      const auto condition = instBlock(InstBlockId(value.arg0));
      const auto condition_value =
          !condition.empty() &&
                  inst(condition.back()).kind == SemInstKind::EndFullExpression
              ? condition.size() >= 2 ? condition[condition.size() - 2]
                                      : InstId::invalid()
          : condition.empty() ? InstId::invalid()
                              : condition.back();
      if (instruction_type != void_type_ || condition.empty() ||
          !condition_value.hasValue() ||
          inst(condition_value).type != bool_type_.index) {
        error = "while instruction has inconsistent semantic facts";
        return false;
      }
      break;
    }
    case SemInstKind::For: {
      const auto clauses = instBlock(InstBlockId(value.arg0));
      if (instruction_type != void_type_ || clauses.size() != 3) {
        error = "for instruction has inconsistent semantic facts";
        return false;
      }
      for (std::size_t clause_index = 0; clause_index < clauses.size();
           ++clause_index) {
        const auto &clause = inst(clauses[clause_index]);
        if (clause.kind != SemInstKind::ForClause ||
            integer(IntegerId(clause.arg0)) !=
                static_cast<std::int64_t>(clause_index)) {
          error = "for instruction has invalid clauses";
          return false;
        }
      }
      const auto condition = instBlock(InstBlockId(inst(clauses[1]).arg1));
      const auto condition_value =
          !condition.empty() &&
                  inst(condition.back()).kind == SemInstKind::EndFullExpression
              ? condition.size() >= 2 ? condition[condition.size() - 2]
                                      : InstId::invalid()
          : condition.empty() ? InstId::invalid()
                              : condition.back();
      if (condition.empty() || !condition_value.hasValue() ||
          TypeId(inst(condition_value).type) != bool_type_) {
        error = "for condition is not bool";
        return false;
      }
      break;
    }
    case SemInstKind::ForClause:
      if (instruction_type != void_type_ ||
          integer(IntegerId(value.arg0)) < 0 ||
          integer(IntegerId(value.arg0)) > 2) {
        error = "for clause has inconsistent semantic facts";
        return false;
      }
      break;
    case SemInstKind::DoWhile: {
      const auto condition = instBlock(InstBlockId(value.arg0));
      const auto condition_value =
          !condition.empty() &&
                  inst(condition.back()).kind == SemInstKind::EndFullExpression
              ? condition.size() >= 2 ? condition[condition.size() - 2]
                                      : InstId::invalid()
          : condition.empty() ? InstId::invalid()
                              : condition.back();
      if (instruction_type != void_type_ || condition.empty() ||
          !condition_value.hasValue() ||
          TypeId(inst(condition_value).type) != bool_type_) {
        error = "do-while instruction has inconsistent semantic facts";
        return false;
      }
      break;
    }
    case SemInstKind::Break:
    case SemInstKind::Continue:
      if (instruction_type != void_type_) {
        error = "loop control instruction must have void type";
        return false;
      }
      if (value.arg0 >= values_->integerCount() ||
          integer(IntegerId(value.arg0)) < 0) {
        error = "loop control instruction has an invalid target";
        return false;
      }
      break;
    case SemInstKind::SwitchArm: {
      const auto body = instBlock(InstBlockId(value.arg1));
      if (body.empty()) {
        error = "switch arm has an invalid result block";
        return false;
      }
      const auto &terminal = inst(body.back());
      if (terminal.kind != SemInstKind::Return &&
          expressionCategory(sem_ir_, body.back()) !=
              SemExprCategory::Diverging &&
          (terminal.kind != SemInstKind::Yield ||
           TypeId(inst(InstId(terminal.arg0)).type) != instruction_type)) {
        error = "switch arm result does not match its control expression";
        return false;
      }
      break;
    }
    case SemInstKind::Switch: {
      const auto scrutinee = InstId(value.arg0);
      const auto scrutinee_type = TypeId(inst(scrutinee).type);
      if (type(scrutinee_type).kind != SemTypeKind::Nominal) {
        error = "switch scrutinee is not an enum";
        return false;
      }
      const auto &nominal =
          nominalType(NominalTypeId(type(scrutinee_type).arg0));
      if (nominal.kind != NominalKind::Enum) {
        error = "switch scrutinee is not an enum";
        return false;
      }
      std::vector<bool> covered(nominal.variants.size());
      bool wildcard = false;
      for (const auto arm_id : instBlock(InstBlockId(value.arg1))) {
        const auto &arm = inst(arm_id);
        if (arm.kind != SemInstKind::SwitchArm ||
            TypeId(arm.type) != instruction_type) {
          error = "switch has an invalid arm record";
          return false;
        }
        const auto variant = integer(IntegerId(arm.arg0));
        if (variant == -1) {
          if (wildcard) {
            error = "switch contains duplicate wildcard arms";
            return false;
          }
          wildcard = true;
        } else if (variant < 0 ||
                   static_cast<std::size_t>(variant) >= covered.size() ||
                   covered[variant]) {
          error = "switch contains an invalid or duplicate variant arm";
          return false;
        } else {
          covered[variant] = true;
        }
        for (const auto body_id : instBlock(InstBlockId(arm.arg1))) {
          const auto &body_inst = inst(body_id);
          if (body_inst.kind != SemInstKind::EnumPayloadAccess)
            continue;
          const auto encoded =
              static_cast<std::uint64_t>(integer(IntegerId(body_inst.arg1)));
          if (variant < 0 || body_inst.arg0 != scrutinee.index ||
              static_cast<std::uint32_t>(encoded >> 32U) != variant) {
            error = "payload projection is not refined by its switch arm";
            return false;
          }
        }
      }
      if (!wildcard &&
          std::ranges::any_of(covered, [](bool value) { return !value; })) {
        error = "switch is not exhaustive";
        return false;
      }
      break;
    }
    case SemInstKind::TypeQuery: {
      const auto query_index =
          static_cast<std::uint32_t>(integer(IntegerId(value.arg0)));
      if (query_index >= type_queries_.size()) {
        error = "type query instruction has an invalid descriptor";
        return false;
      }
      const auto kind = type_queries_[query_index].kind;
      const auto integer_query =
          kind == SemTypeQueryArtifact::Kind::ArrayExtent ||
          kind == SemTypeQueryArtifact::Kind::TupleArity;
      const auto &result_type = type(instruction_type);
      if ((!integer_query && instruction_type != bool_type_) ||
          (integer_query && (result_type.kind != SemTypeKind::Integer ||
                             result_type.arg1 != 0))) {
        error = "type query instruction has an invalid result type";
        return false;
      }
      break;
    }
