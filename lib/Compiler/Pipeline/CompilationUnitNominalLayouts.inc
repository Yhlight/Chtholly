// Included inside buildNominalArtifacts; this fragment aliases the shared
// SemIR/public-interface and nominal-layout state.
  const auto is_concrete = [&](auto &&self, const PublicType &type) -> bool {
    if (type.kind == PublicTypeKind::TypeParameter ||
        type.kind == PublicTypeKind::Never ||
        type.kind == PublicTypeKind::Count)
      return false;
    return std::ranges::all_of(type.arguments, [&](const PublicType &argument) {
      return self(self, argument);
    });
  };
  std::vector<PublicType> witness_work_queue;
  for (std::uint32_t index = 0; index < sem_ir.typeCount(); ++index) {
    const auto type_id = TypeId(index);
    if (sem_ir.type(type_id).kind != SemTypeKind::Nominal)
      continue;
    const auto nominal = NominalTypeId(sem_ir.type(type_id).arg0);
    const auto &definition = sem_ir.nominalType(nominal);
    if (definition.kind == NominalKind::ForeignHandle ||
        (definition.kind == NominalKind::ForeignResource &&
         !definition.foreign_registration_storage_type.hasValue()))
      continue;
    auto type = map_type(type_id);
    if (type && is_concrete(is_concrete, *type))
      witness_work_queue.push_back(std::move(*type));
  }
  std::ranges::sort(witness_work_queue, {}, [](const PublicType &type) {
    return canonicalPublicTypeBytes(type);
  });
  witness_work_queue.erase(
      std::ranges::unique(
          witness_work_queue, {},
          [](const PublicType &type) { return canonicalPublicTypeBytes(type); })
          .begin(),
      witness_work_queue.end());
  if (witness_work_queue.size() > 4096) {
    error = "nominal semantic witness work queue exceeds its safety limit";
    return std::nullopt;
  }
  for (std::size_t index = 0; index < witness_work_queue.size(); ++index)
    if (!build_specific(witness_work_queue[index], error)) {
      if (error.empty())
        error = "failed to build a nominal type specific or semantic witness";
      return std::nullopt;
    }

  const llvm::Triple triple(llvm::Triple::normalize(target_triple));
  const auto pointer_width = triple.isArch64Bit()   ? 64U
                             : triple.isArch32Bit() ? 32U
                                                    : 0U;
  TargetLayoutConfig target{triple.str(), pointer_width, NominalLayoutAbiEpoch};
  if (!target.verify(error))
    return std::nullopt;
  std::unordered_map<std::string, std::size_t> layout_by_type;
  std::unordered_set<std::string> layout_in_progress;
  std::function<std::optional<NominalTypeLayoutArtifact>(const PublicType &,
                                                         std::string &)>
      build_layout;
  build_layout = [&](const PublicType &type, std::string &nested_error)
      -> std::optional<NominalTypeLayoutArtifact> {
    const auto key = canonicalPublicTypeBytes(type);
    if (const auto found = layout_by_type.find(key);
        found != layout_by_type.end()) {
      const auto &layout = result.layouts[found->second];
      return layout;
    }
    if (!layout_in_progress.insert(key).second) {
      nested_error = "nominal type layout has a recursive value dependency";
      return std::nullopt;
    }
    const auto specific_found = specific_by_type.find(key);
    if (specific_found == specific_by_type.end()) {
      if (!build_specific(type, nested_error)) {
        layout_in_progress.erase(key);
        return std::nullopt;
      }
    }
    const auto completed_specific = specific_by_type.find(key);
    if (completed_specific == specific_by_type.end()) {
      layout_in_progress.erase(key);
      nested_error =
          "nominal type specific builder did not register its requested type";
      return std::nullopt;
    }
    const auto specific_index = completed_specific->second;
    auto layout = buildNominalTypeLayout(result.specifics[specific_index],
                                         target, build_layout, nested_error);
    layout_in_progress.erase(key);
    if (!layout)
      return std::nullopt;
    const auto index = result.layouts.size();
    result.layouts.push_back(std::move(*layout));
    layout_by_type.emplace(key, index);
    return result.layouts.back();
  };
  for (const auto &root : witness_work_queue)
    if (!build_layout(root, error)) {
      if (error.empty())
        error = "failed to build a nominal type layout";
      return std::nullopt;
    }
  result.nominal_semantic_witnesses.reserve(result.specifics.size());
  for (const auto &specific : result.specifics)
    result.nominal_semantic_witnesses.push_back(
        specific.nominal_semantic_witness);
  for (std::uint32_t index = 0; index < sem_ir.typeCount(); ++index) {
    const auto type_id = TypeId(index);
    if (sem_ir.type(type_id).kind != SemTypeKind::Nominal)
      continue;
    const auto nominal = NominalTypeId(sem_ir.type(type_id).arg0);
    const auto &definition = sem_ir.nominalType(nominal);
    if (definition.kind == NominalKind::ForeignHandle ||
        (definition.kind == NominalKind::ForeignResource &&
         !definition.foreign_registration_storage_type.hasValue()))
      continue;
    auto type = map_type(type_id);
    if (!type || !is_concrete(is_concrete, *type))
      continue;
    const auto found = specific_by_type.find(canonicalPublicTypeBytes(*type));
    if (found == specific_by_type.end()) {
      error = "concrete nominal type has no nominal semantic witness";
      return std::nullopt;
    }
    result.type_witness_bindings.push_back({type_id, found->second});
    const auto layout = layout_by_type.find(canonicalPublicTypeBytes(*type));
    if (layout == layout_by_type.end()) {
      error = "concrete nominal type has no target layout";
      return std::nullopt;
    }
    result.type_layout_bindings.push_back(
        {type_id, static_cast<std::uint32_t>(layout->second),
         result.specifics[found->second].result_fingerprint});
  }
  return result;
