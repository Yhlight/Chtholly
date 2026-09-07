// Included inside SemanticContext::checkExpression; this fragment retains
// the shared expression conversion and diagnostic state.
  case NodeKind::IntegerLiteral: {
    const auto text = tree_.tokens().text(tree_.token(node));
    const auto parsed = parseIntegerLiteral(text);
    if (!parsed) {
      emit(DiagnosticKind::InvalidNumericLiteral, node);
      return appendInvalid(block, node);
    }
    TypeId type;
    if (parsed->suffix != NumericSuffix::None) {
      type = typeForSuffix(parsed->suffix);
      if (!integerMagnitudeFits(parsed->magnitude, type)) {
        emit(DiagnosticKind::InvalidNumericLiteral, node);
        return appendInvalid(block, node);
      }
    } else if (expected_type.hasValue() &&
               sem_ir_.type(expected_type).kind == SemTypeKind::Integer &&
               integerMagnitudeFits(parsed->magnitude, expected_type)) {
      type = expected_type;
    } else if (expected_type.hasValue() &&
               sem_ir_.type(expected_type).kind == SemTypeKind::Float &&
               integerMagnitudeExactlyRepresentableAsFloat(
                   parsed->magnitude, sem_ir_.type(expected_type).arg0)) {
      const auto width = sem_ir_.type(expected_type).arg0;
      const auto bits =
          width == 32 ? static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(
                            static_cast<float>(parsed->magnitude)))
                      : std::bit_cast<std::uint64_t>(
                            static_cast<double>(parsed->magnitude));
      return appendInst<SemFloatLiteral>(
          block, node, expected_type,
          sem_ir_.addInteger(std::bit_cast<std::int64_t>(bits)));
    } else if (parsed->magnitude <= static_cast<std::uint64_t>(INT32_MAX)) {
      type = sem_ir_.i32Type();
    } else if (parsed->magnitude <= static_cast<std::uint64_t>(INT64_MAX)) {
      type = sem_ir_.addIntegerType(64, true);
    } else {
      emit(DiagnosticKind::InvalidNumericLiteral, node);
      return appendInvalid(block, node);
    }
    const auto bits = std::bit_cast<std::int64_t>(parsed->magnitude);
    const auto result = appendInst<SemIntegerLiteral>(block, node, type,
                                                      sem_ir_.addInteger(bits));
    if (parsed->suffix == NumericSuffix::None)
      conversions_.recordInteger(result,
                                 SemanticIntegerLiteral{parsed->magnitude});
    return result;
  }
  case NodeKind::FloatLiteral: {
    const auto text = tree_.tokens().text(tree_.token(node));
    NumericSuffix suffix = NumericSuffix::None;
    const auto parsed = parseFloatLiteral(text, suffix);
    if (!parsed) {
      emit(DiagnosticKind::InvalidNumericLiteral, node);
      return appendInvalid(block, node);
    }
    auto type = suffix == NumericSuffix::F32   ? sem_ir_.addFloatType(32)
                : suffix == NumericSuffix::F64 ? sem_ir_.f64Type()
                : expected_type.hasValue() &&
                        sem_ir_.type(expected_type).kind == SemTypeKind::Float
                    ? expected_type
                    : sem_ir_.f64Type();
    const auto width = sem_ir_.type(type).arg0;
    if (width == 32 && !std::isfinite(static_cast<float>(*parsed))) {
      emit(DiagnosticKind::InvalidNumericLiteral, node);
      return appendInvalid(block, node);
    }
    const auto bits =
        width == 32 ? static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(
                          static_cast<float>(*parsed)))
                    : std::bit_cast<std::uint64_t>(*parsed);
    const auto result = appendInst<SemFloatLiteral>(
        block, node, type,
        sem_ir_.addInteger(std::bit_cast<std::int64_t>(bits)));
    if (suffix == NumericSuffix::None)
      conversions_.recordFloat(result, SemanticFloatLiteral{*parsed});
    return result;
  }
  case NodeKind::BoolLiteral: {
    const auto value =
        tree_.tokens().get(tree_.token(node)).kind == TokenKind::KwTrue;
    return appendInst<SemBoolLiteral>(block, node, sem_ir_.boolType(),
                                      sem_ir_.addInteger(value ? 1 : 0));
  }
  case NodeKind::CharLiteral: {
    const auto value = decodeChar(node);
    if (!value) {
      emit(DiagnosticKind::InvalidCharLiteral, node);
      return appendInvalid(block, node);
    }
    return appendInst<SemCharLiteral>(block, node, sem_ir_.charType(),
                                      sem_ir_.addInteger(*value));
  }
  case NodeKind::StringLiteral: {
    const auto value = decodeString(node);
    return appendInst<SemStringLiteral>(block, node, sem_ir_.stringType(),
                                        value);
  }
  case NodeKind::CStringLiteral: {
    const auto value = decodeString(node);
    const auto type =
        sem_ir_.addRawPointerType(sem_ir_.addIntegerType(8, true), true);
    return appendInst<SemStringLiteral>(block, node, type, value);
  }
  case NodeKind::NullLiteral:
    return appendInst<SemNullPointer>(
        block, node, sem_ir_.addRawPointerType(sem_ir_.voidType(), true));
  case NodeKind::CastExpr: {
    if (children.size() != 2)
      return appendInvalid(block, node);
    const auto target_type = checkType(children.back());
    const auto target_kind = sem_ir_.type(target_type).kind;
    if ((tree_.kind(children.front()) != NodeKind::Name &&
         tree_.kind(children.front()) != NodeKind::QualifiedName) ||
        (target_kind != SemTypeKind::CFunctionPointer &&
         target_kind != SemTypeKind::CVariadicFunctionPointer)) {
      auto source = checkExpression(children.front(), block);
      const auto source_kind = sem_ir_.type(instType(source)).kind;
      if (source_kind == SemTypeKind::Nominal &&
          target_type == sem_ir_.i32Type()) {
        const auto &nominal = sem_ir_.nominalType(
            NominalTypeId(sem_ir_.type(instType(source)).arg0));
        if (nominal.kind == NominalKind::Enum && nominal.is_value_enum)
          return appendInst<SemEnumTag>(block, node, target_type, source);
      }
      if ((source_kind == SemTypeKind::Integer ||
           source_kind == SemTypeKind::Float ||
           source_kind == SemTypeKind::Char) &&
          (target_kind == SemTypeKind::Integer ||
           target_kind == SemTypeKind::Float ||
           target_kind == SemTypeKind::Char)) {
        if (const auto implicit = queryConversion(source, target_type);
            implicit.valid())
          (void)applyConversion(source, target_type, node, block, implicit);
        else
          source =
              appendInst<SemNumericConvert>(block, node, target_type, source);
        return source;
      }
      const auto safe = queryConversion(source, target_type);
      if (safe.valid() && (safe.kind == ConversionKind::ReferenceAuthority ||
                           safe.kind == ConversionKind::PointerAuthority ||
                           safe.kind == ConversionKind::Identity)) {
        (void)applyConversion(source, target_type, node, block, safe);
        return source;
      }
      emit(DiagnosticKind::InvalidCast, node);
      return appendInvalid(block, node);
    }
    std::vector<FunctionRefId> candidates;
    if (tree_.kind(children.front()) == NodeKind::Name) {
      const auto source_name = nameFor(children.front());
      if (const auto found = function_names_.find(source_name.index);
          found != function_names_.end())
        candidates = found->second;
    } else {
      candidates = resolveImportedFunctions(children.front());
    }
    if ((target_kind != SemTypeKind::CFunctionPointer &&
         target_kind != SemTypeKind::CVariadicFunctionPointer) ||
        candidates.empty() ||
        !requireUnsafe(UnsafeOperationKind::CallbackAdapterConversion, node)) {
      emit(DiagnosticKind::InvalidCallbackAdapter, node);
      return appendInvalid(block, node);
    }
    if (candidates.size() != 1) {
      emit(DiagnosticKind::AmbiguousCall, children.front());
      return appendInvalid(block, node);
    }
    const auto source = candidates.front();
    const auto &reference = sem_ir_.functionRef(source);
    if (reference.generic.hasValue()) {
      emit(DiagnosticKind::InvalidCallbackAdapter, node);
      return appendInvalid(block, node);
    }
    const auto &source_type = sem_ir_.type(reference.local_type);
    if (source_type.kind != SemTypeKind::Function) {
      emit(DiagnosticKind::InvalidCallbackAdapter, node);
      return appendInvalid(block, node);
    }
    const auto target_parameters =
        sem_ir_.typeBlock(TypeBlockId(sem_ir_.type(target_type).arg0));
    const auto source_parameters =
        sem_ir_.typeBlock(TypeBlockId(source_type.arg0));
    const auto valid =
        sem_ir_.isConcreteReverseTarget(source) &&
        target_kind == SemTypeKind::CFunctionPointer &&
        source_type.arg1 == sem_ir_.type(target_type).arg1 &&
        std::ranges::equal(source_parameters, target_parameters) &&
        std::ranges::all_of(target_parameters,
                            [&](TypeId parameter) {
                              return isCallbackTransportType(parameter);
                            }) &&
        isCallbackTransportType(TypeId(source_type.arg1), true);
    if (!valid) {
      emit(DiagnosticKind::InvalidCallbackAdapter, node);
      return appendInvalid(block, node);
    }
    sem_ir_.addFunctionOccurrence(children.front(),
                                  SemSymbolOccurrenceKind::Reference, source);
    return appendInst<SemCallbackAdapter>(block, node, target_type, source);
  }
  case NodeKind::CheckedCastExpr: {
    if (children.size() != 2)
      return appendInvalid(block, node);
    const auto target = checkType(children.back());
    auto source = checkExpression(children.front(), block);
    const auto shape = checkedCastShape(target, node);
    if (!shape)
      return appendInvalid(block, node);
    const auto source_kind = sem_ir_.type(instType(source)).kind;
    const auto target_kind = sem_ir_.type(target).kind;
    if ((source_kind == SemTypeKind::Integer ||
         source_kind == SemTypeKind::Float ||
         source_kind == SemTypeKind::Char) &&
        (target_kind == SemTypeKind::Integer ||
         target_kind == SemTypeKind::Float ||
         target_kind == SemTypeKind::Char))
      return appendInst<SemCheckedNumericCast>(block, node, shape->result,
                                               source);
    const auto safe = queryConversion(source, target);
    if (safe.valid() && (safe.kind == ConversionKind::Identity ||
                         safe.kind == ConversionKind::ReferenceAuthority ||
                         safe.kind == ConversionKind::PointerAuthority)) {
      (void)applyConversion(source, target, node, block, safe);
      const std::array payload{source};
      return appendInst<SemEnumInit>(block, node, shape->result,
                                     sem_ir_.addInstBlock(payload, true),
                                     sem_ir_.addInteger(shape->ok_variant));
    }
    emit(DiagnosticKind::InvalidCast, node);
    return appendInvalid(block, node);
  }
  case NodeKind::Name: {
    const auto name = nameFor(node);
    if (name.hasValue() && name == initializing_closure_capture_) {
      emit(DiagnosticKind::InvalidClosureCapture, node);
      return appendInvalid(block, node);
    }
    if (const auto *capture = lexical_scopes_.lookup(name);
        capture && capture->kind == SemanticBindingKind::ClosureCapture &&
        active_closure_environment_.hasValue()) {
      const auto environment_reference =
          sem_ir_.local(active_closure_environment_).type;
      const auto reference = appendInst<SemNameRef>(
          block, node, environment_reference, active_closure_environment_);
      const auto environment = appendInst<SemDereference>(
          block, node, active_closure_environment_type_, reference);
      const auto &nominal = sem_ir_.nominalType(
          NominalTypeId(sem_ir_.type(active_closure_environment_type_).arg0));
      if (capture->target >= nominal.fields.size())
        return appendInvalid(block, node);
      return appendInst<SemStructFieldAccess>(
          block, node, nominal.fields[capture->target].type, environment,
          sem_ir_.addInteger(capture->target));
    }
    if (active_closure_environment_.hasValue()) {
      const auto *outside = lexical_scopes_.lookupOutsideIsolation(name);
      if (outside && (outside->kind == SemanticBindingKind::Local ||
                      outside->kind == SemanticBindingKind::ClosureCapture)) {
        emit(DiagnosticKind::ClosureCaptureRequired, node);
        return appendInvalid(block, node);
      }
    }
    const auto binding = lookupValue(name);
    if (binding.constant.hasValue()) {
      const auto &constant = sem_ir_.constantEntity(binding.constant);
      sem_ir_.addConstantOccurrence(node, SemSymbolOccurrenceKind::Reference,
                                    binding.constant);
      return (constant.flags & SemConstantStatic) != 0
                 ? appendInst<SemStaticRef>(block, node, constant.type,
                                            binding.constant)
                 : appendInst<SemConstantRef>(block, node, constant.type,
                                              binding.constant);
    }
    const auto local = binding.local;
    if (!local.hasValue()) {
      const auto found = function_names_.find(name.index);
      if (found == function_names_.end()) {
        emit(DiagnosticKind::UnknownName, node);
        return appendInvalid(block, node);
      }
      FunctionRefId function_ref;
      for (const auto candidate : found->second) {
        if (expected_type.hasValue() &&
            sem_ir_.functionRef(candidate).local_type != expected_type)
          continue;
        if (function_ref.hasValue()) {
          emit(DiagnosticKind::AmbiguousCall, node);
          return appendInvalid(block, node);
        }
        function_ref = candidate;
      }
      if (!function_ref.hasValue()) {
        emit(found->second.size() > 1 ? DiagnosticKind::AmbiguousCall
                                      : DiagnosticKind::TypeMismatch,
             node);
        return appendInvalid(block, node);
      }
      return makeFunctionValue(node, node, function_ref, block);
    }
    sem_ir_.addLocalOccurrence(node, SemSymbolOccurrenceKind::Reference, local);
    return appendInst<SemNameRef>(block, node, sem_ir_.local(local).type,
                                  local);
  }
  case NodeKind::QualifiedName: {
    if (const auto variant = resolveQualifiedEnumVariant(node))
      return checkEnumConstruction(node, *variant, block);
    if (const auto associated =
            resolveAssociatedFunctionValue(node, expected_type))
      return associated->function.hasValue()
                 ? makeFunctionValue(node, associated->occurrence,
                                     associated->function, block)
                 : appendInvalid(block, node);
    if (const auto value = resolveImportedValue(node, block))
      return *value;
    const auto functions = resolveImportedFunctions(node);
    FunctionRefId function_ref;
    for (const auto candidate : functions) {
      if (expected_type.hasValue() &&
          sem_ir_.functionRef(candidate).local_type != expected_type)
        continue;
      if (function_ref.hasValue()) {
        emit(DiagnosticKind::AmbiguousCall, node);
        return appendInvalid(block, node);
      }
      function_ref = candidate;
    }
    if (!function_ref.hasValue()) {
      if (!functions.empty())
        emit(functions.size() > 1 ? DiagnosticKind::AmbiguousCall
                                  : DiagnosticKind::TypeMismatch,
             node);
      return appendInvalid(block, node);
    }
    const auto names = tree_.children(node);
    return makeFunctionValue(node, names.empty() ? node : names.back(),
                             function_ref, block);
  }
  case NodeKind::TypeMemberExpr: {
    const auto associated = resolveAssociatedFunctionValue(node, expected_type);
    if (!associated || !associated->function.hasValue())
      return appendInvalid(block, node);
    return makeFunctionValue(node, associated->occurrence, associated->function,
                             block);
  }
  case NodeKind::EnumLiteral: {
    const auto variant = resolveEnumLiteral(node);
    if (!variant) {
      emit(DiagnosticKind::InvalidEnumVariant, node);
      return appendInvalid(block, node);
    }
    return checkEnumConstruction(node, *variant, block);
  }
  case NodeKind::MoveExpr: {
    if (children.size() != 1)
      return appendInvalid(block, node);
    const auto operand = checkExpression(children.front(), block);
    if (containsRawPointerDereference(operand)) {
      emit(DiagnosticKind::RawPointerCapabilityUnavailable, node);
      return appendInvalid(block, node);
    }
    return appendInst<SemMove>(block, node, instType(operand), operand);
  }
  case NodeKind::UnsafeExpr: {
    if (children.size() != 1)
      return appendInvalid(block, node);
    ++unsafe_depth_;
    const auto result =
        tree_.kind(children.front()) == NodeKind::ValueBlock
            ? checkScopedBlockExpression(node, children.front(), block,
                                         expected_type)
            : checkExpression(children.front(), block, expected_type);
    --unsafe_depth_;
    return result;
  }
  case NodeKind::CopyExpr: {
    if (children.size() != 1)
      return appendInvalid(block, node);
    const auto operand = checkExpression(children.front(), block);
    if (containsRawPointerDereference(operand)) {
      emit(DiagnosticKind::RawPointerCapabilityUnavailable, node);
      return appendInvalid(block, node);
    }
    return appendInst<SemCopy>(block, node, instType(operand), operand);
  }
