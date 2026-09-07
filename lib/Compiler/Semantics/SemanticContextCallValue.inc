// One semantic call path for already evaluated callable values.
InstId SemanticContext::checkCallableValue(NodeId node, InstId callee,
                                         std::vector<InstId> &block) {
  const auto children = tree_.children(node);
  const auto callee_node = children.front();
  for (std::size_t index = 1; index < children.size(); ++index) {
    if (tree_.kind(children[index]) == NodeKind::NamedArgument) {
      emit(DiagnosticKind::InvalidNamedArgument, children[index]);
      return appendInvalid(block, node);
    }
  }
      const auto callee_type = instType(callee);
      const auto kind = sem_ir_.type(callee_type).kind;
      if (const auto *closure = sem_ir_.tryGetCallableEnvironment(callee_type);
          closure) {
        const auto target = closure->target;
        const auto reference = sem_ir_.functionRef(target);
        const auto signature = sem_ir_.type(reference.local_type);
        const auto parameter_view = sem_ir_.typeBlock(TypeBlockId(signature.arg0));
        const std::vector<TypeId> parameters(parameter_view.begin(), parameter_view.end());
        auto receiver = callee;
        bool temporary_receiver = false;
        if (!parameters.empty() && sem_ir_.type(parameters.front()).kind == SemTypeKind::Reference &&
            !isPlace(receiver) && sem_ir_.type(instType(receiver)).kind != SemTypeKind::Reference) {
          receiver = materializeTemporary(receiver, callee_node, block);
          if (!receiver.hasValue()) return appendInvalid(block, node);
          receiver = appendInst<SemBorrowPlace>(block, callee_node, parameters.front(), receiver);
          temporary_receiver = true;
        }
        if (parameters.empty() ||
            (!temporary_receiver && !adjustMethodReceiver(receiver, parameters.front(), callee_node,
                                  block)))
          return appendInvalid(block, node);
        std::vector<InstId> arguments{receiver};
        for (std::size_t index = 1; index < children.size(); ++index)
          arguments.push_back(checkExpression(children[index], block));
        if (arguments.size() != parameters.size())
          emit(DiagnosticKind::ArgumentCountMismatch, node);
        for (std::size_t index = 1;
             index < arguments.size() && index < parameters.size(); ++index)
          if (!adjustExpression(arguments[index], parameters[index],
                                children[index], block))
            emit(DiagnosticKind::TypeMismatch, children[index]);
        return appendInst<SemCall>(block, node, TypeId(signature.arg1),
                                   target,
                                   sem_ir_.addInstBlock(arguments));
      }
      if (kind == SemTypeKind::CallbackAdapter) {
        (void)requireUnsafe(UnsafeOperationKind::CallbackAdapterCall,
                            callee_node);
        const auto adapter_fields =
            sem_ir_.typeBlock(TypeBlockId(sem_ir_.type(callee_type).arg0));
        if (adapter_fields.size() != 3) {
          emit(DiagnosticKind::InvalidCallbackAdapter, callee_node);
          return appendInvalid(block, node);
        }
        const auto entry_type = adapter_fields[0];
        const auto entry_parameters =
            sem_ir_.typeBlock(TypeBlockId(sem_ir_.type(entry_type).arg0));
        const auto context_index = sem_ir_.callbackContextParameter(entry_type);
        std::vector<InstId> arguments;
        for (std::size_t index = 1; index < children.size(); ++index)
          arguments.push_back(checkExpression(children[index], block));
        if (context_index >= entry_parameters.size() ||
            arguments.size() + 1 != entry_parameters.size())
          emit(DiagnosticKind::ArgumentCountMismatch, node);
        for (std::size_t argument = 0; argument < arguments.size();
             ++argument) {
          const auto parameter =
              argument < context_index ? argument : argument + 1;
          if (parameter >= entry_parameters.size() ||
              !adjustExpression(arguments[argument],
                                entry_parameters[parameter],
                                children[argument + 1], block))
            emit(DiagnosticKind::TypeMismatch, children[argument + 1]);
        }
        return appendInst<SemCallbackAdapterCall>(
            block, node, TypeId(sem_ir_.type(entry_type).arg1), callee,
            sem_ir_.addInstBlock(arguments));
      }
      if (kind == SemTypeKind::CFunctionPointer ||
          kind == SemTypeKind::CVariadicFunctionPointer) {
        (void)requireUnsafe(UnsafeOperationKind::CFunctionPointerCall,
                            callee_node);
        const auto parameter_view =
            sem_ir_.typeBlock(TypeBlockId(sem_ir_.type(callee_type).arg0));
        const std::vector<TypeId> parameters(parameter_view.begin(), parameter_view.end());
        std::vector<InstId> arguments;
        for (std::size_t index = 1; index < children.size(); ++index)
          arguments.push_back(checkExpression(children[index], block));
        const auto variadic = kind == SemTypeKind::CVariadicFunctionPointer;
        if ((!variadic && arguments.size() != parameters.size()) ||
            (variadic && arguments.size() < parameters.size()))
          emit(DiagnosticKind::ArgumentCountMismatch, node);
        for (std::size_t index = 0;
             index < arguments.size() && index < parameters.size(); ++index)
          if (!adjustExpression(arguments[index], parameters[index],
                                children[index + 1], block))
            emit(DiagnosticKind::TypeMismatch, children[index + 1]);
        for (std::size_t index = parameters.size(); index < arguments.size();
             ++index) {
          const auto argument_kind =
              sem_ir_.type(instType(arguments[index])).kind;
          if (argument_kind != SemTypeKind::Bool &&
              argument_kind != SemTypeKind::Char &&
              argument_kind != SemTypeKind::Integer &&
              argument_kind != SemTypeKind::Float &&
              argument_kind != SemTypeKind::RawPointer &&
              argument_kind != SemTypeKind::CFunctionPointer &&
              argument_kind != SemTypeKind::CVariadicFunctionPointer)
            emit(DiagnosticKind::InvalidVariadicArgument, children[index + 1]);
        }
        return appendInst<SemIndirectForeignCall>(
            block, node, TypeId(sem_ir_.type(callee_type).arg1), callee,
            sem_ir_.addInstBlock(arguments));
      }
      if (kind == SemTypeKind::Function) {
        const auto parameter_view =
            sem_ir_.typeBlock(TypeBlockId(sem_ir_.type(callee_type).arg0));
        const std::vector<TypeId> parameters(parameter_view.begin(), parameter_view.end());
        std::vector<InstId> arguments;
        for (std::size_t index = 1; index < children.size(); ++index)
          arguments.push_back(checkExpression(children[index], block));
        if (arguments.size() != parameters.size())
          emit(DiagnosticKind::ArgumentCountMismatch, node);
        for (std::size_t index = 0;
             index < arguments.size() && index < parameters.size(); ++index)
          if (!adjustExpression(arguments[index], parameters[index],
                                children[index + 1], block))
            emit(DiagnosticKind::TypeMismatch, children[index + 1]);
        return appendInst<SemIndirectCall>(
            block, node, TypeId(sem_ir_.type(callee_type).arg1), callee,
            sem_ir_.addInstBlock(arguments));
      }
      emit(DiagnosticKind::NotCallable, callee_node);
      return appendInvalid(block, node);
}
