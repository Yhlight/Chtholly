// Included in SemanticContextFunctions.cpp; this fragment aliases the
// original SemanticContext state and preserves phase ordering.
[[nodiscard]] FunctionRefId
SemanticContext::functionReference(FunctionId function) const {
  for (std::uint32_t index = 0; index < sem_ir_.functionRefCount(); ++index) {
    const auto reference = FunctionRefId(index);
    if (sem_ir_.functionRef(reference).local_function == function)
      return reference;
  }
  return FunctionRefId::invalid();
}
