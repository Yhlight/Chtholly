// Projection of an already evaluated receiver, shared by member access and calls.
InstId SemanticContext::checkMemberValue(NodeId node, InstId base,
                                       std::vector<InstId> &block) {
  const auto children = tree_.children(node);
    if (children.size() != 2)
      return appendInvalid(block, node);

    if (tree_.kind(children[1]) == NodeKind::IntegerLiteral) {
      const auto &base_type = sem_ir_.type(instType(base));
      if (base_type.kind != SemTypeKind::Tuple) {
        emit(DiagnosticKind::InvalidIndex, node);
        return appendInvalid(block, node);
      }
      const auto spelling = tree_.tokens().text(tree_.token(children[1]));
      std::uint32_t index = 0;
      const auto [end, error] = std::from_chars(
          spelling.data(), spelling.data() + spelling.size(), index);
      if (error != std::errc{} || end != spelling.data() + spelling.size() ||
          index >= sem_ir_.tupleArity(instType(base))) {
        emit(DiagnosticKind::InvalidIndex, children[1]);
        return appendInvalid(block, node);
      }
      return appendInst<SemStructFieldAccess>(
          block, node, sem_ir_.tupleElementType(instType(base), index), base,
          sem_ir_.addInteger(index));
    }
    const auto member = nameFor(children[1]);
    const auto member_text = member.hasValue()
                                 ? values_.identifier(sem_ir_.name(member).text)
                                 : std::string_view{};
    if ((instType(base) == sem_ir_.stringType() ||
         sem_ir_.type(instType(base)).kind == SemTypeKind::Slice) &&
        member_text == "len") {
      const auto length_type =
          sem_ir_.type(instType(base)).kind == SemTypeKind::Slice &&
                  usesWideSliceIndices(sem_ir_.languageVersion())
              ? sem_ir_.addIntegerType(64, false)
              : sem_ir_.i32Type();
      return appendInst<SemMemberAccess>(block, node, length_type, base,
                                         member);
    }
    if (sem_ir_.type(instType(base)).kind ==
            SemTypeKind::CallbackRegistration &&
        member_text == "active")
      return appendInst<SemCallbackRegistrationActive>(
          block, node, sem_ir_.boolType(), base);
    if (sem_ir_.type(instType(base)).kind == SemTypeKind::CallbackCompletion &&
        member_text == "pending")
      return appendInst<SemCallbackCompletionPending>(block, node,
                                                      sem_ir_.boolType(), base);
    if (sem_ir_.type(instType(base)).kind == SemTypeKind::CallbackWake &&
        member_text == "ready")
      return appendInst<SemCallbackWakeReady>(block, node, sem_ir_.boolType(),
                                              base);
    while (sem_ir_.type(instType(base)).kind == SemTypeKind::Reference)
      base = appendInst<SemDereference>(
          block, children[0], sem_ir_.referencePointee(instType(base)), base);
    if ((instType(base) == sem_ir_.stringType() ||
         sem_ir_.type(instType(base)).kind == SemTypeKind::Slice) &&
        member_text == "len") {
      const auto length_type =
          sem_ir_.type(instType(base)).kind == SemTypeKind::Slice &&
                  usesWideSliceIndices(sem_ir_.languageVersion())
              ? sem_ir_.addIntegerType(64, false)
              : sem_ir_.i32Type();
      return appendInst<SemMemberAccess>(block, node, length_type, base,
                                         member);
    }
    const auto &base_type = sem_ir_.type(instType(base));
    if (base_type.kind == SemTypeKind::Nominal) {
      const auto &nominal = sem_ir_.nominalType(NominalTypeId(base_type.arg0));
      for (std::size_t index = 0; index < nominal.fields.size(); ++index) {
        if (sem_ir_.name(nominal.fields[index].name).text ==
            sem_ir_.name(member).text) {
          if (nominal.canonical_entity.hasValue() &&
              (nominal.fields[index].flags & 1U) == 0) {
            emit(DiagnosticKind::PrivateMemberAccess, children[1]);
            return appendInvalid(block, node);
          }
          const auto encoded_index =
              nominal.kind == NominalKind::Union && hasUnsafeAuthority()
                  ? -static_cast<std::int64_t>(index) - 1
                  : static_cast<std::int64_t>(index);
          if (nominal.kind == NominalKind::Union)
            return appendInst<SemUnionFieldAccess>(
                block, node,
                nominalFieldType(instType(base), nominal.fields[index]), base,
                sem_ir_.addInteger(encoded_index));
          return appendInst<SemStructFieldAccess>(
              block, node,
              nominalFieldType(instType(base), nominal.fields[index]), base,
              sem_ir_.addInteger(encoded_index));
        }
      }
      return checkBoundMethodValue(node, base, children[0], member, block);
    }
    emit(DiagnosticKind::InvalidMember, node);
    return appendInvalid(block, node);
}
