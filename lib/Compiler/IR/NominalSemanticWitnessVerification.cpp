#include "NominalSemanticWitnessVerificationInternal.h"

#include <limits>
#include <ranges>
#include <unordered_set>

namespace chtholly::compiler::internal {

bool NominalSemanticWitnessVerificationService::verify(
    NominalSemanticWitnessVerificationState &state, std::string &error) {
  const auto &artifact = state.artifact;
  error.clear();
  const auto enum_valid = [](auto value) {
    return value < decltype(value)::Count;
  };
  if (!state.valid_entity(artifact.nominal_template,
                          PublicEntityKind::NominalType) ||
      !artifact.semantic_options_fingerprint.hasValue() ||
      !artifact.structural_specific_fingerprint.hasValue() ||
      !enum_valid(artifact.representation.value_repr) ||
      !enum_valid(artifact.representation.init_repr) ||
      !enum_valid(artifact.representation.ownership) ||
      !enum_valid(artifact.representation.copy) ||
      !enum_valid(artifact.representation.move) ||
      !enum_valid(artifact.representation.destroy) ||
      !enum_valid(artifact.representation.object_repr) ||
      artifact.kind >= NominalKind::Count ||
      artifact.copy_target.has_value() !=
          (artifact.representation.copy == CopyReprKind::Custom) ||
      artifact.destroy_target.has_value() !=
          (artifact.representation.destroy == DestroyReprKind::Custom) ||
      artifact.value_repr_carrier.has_value() !=
          (artifact.representation.value_repr == ValueReprKind::Custom) ||
      artifact.object_repr_carrier.has_value() !=
          (artifact.representation.object_repr == ObjectReprKind::Custom) ||
      artifact.pack_target.has_value() != artifact.init_target.has_value() ||
      ((artifact.pack_target || artifact.init_target) &&
       artifact.representation.value_repr != ValueReprKind::Custom) ||
      (artifact.copy_target &&
       !state.valid_entity(*artifact.copy_target, PublicEntityKind::Function)) ||
      (artifact.destroy_target &&
       !state.valid_entity(*artifact.destroy_target,
                           PublicEntityKind::Function)) ||
      (artifact.pack_target &&
       !state.valid_entity(*artifact.pack_target, PublicEntityKind::Function)) ||
      (artifact.init_target &&
       !state.valid_entity(*artifact.init_target, PublicEntityKind::Function)) ||
      (artifact.representation.object_repr !=
           ObjectReprKind::NominalAggregate &&
       artifact.representation.object_repr != ObjectReprKind::Custom) ||
      (artifact.representation.object_repr == ObjectReprKind::NominalAggregate &&
       !artifact.object_field_projections.empty()) ||
      (artifact.representation.value_repr == ValueReprKind::Custom &&
       artifact.representation.init_repr != InitReprKind::ByConversion) ||
      (artifact.representation.value_repr == ValueReprKind::Pointer &&
       artifact.representation.init_repr != InitReprKind::InPlace) ||
      artifact.request_fingerprint != state.request_fingerprint(artifact) ||
      artifact.result_fingerprint != state.result_fingerprint(artifact)) {
    error =
        "nominal semantic witness has invalid identity or representation facts";
    return false;
  }
  const auto valid_body = [](std::span<const LifecycleBodyOp> body,
                             LifecycleBodyOpKind expected) {
    for (std::size_t index = 0; index < body.size(); ++index)
      if (body[index].kind >= LifecycleBodyOpKind::Count ||
          body[index].kind != expected || body[index].field_index != index ||
          !body[index].child_witness.hasValue())
        return false;
    return true;
  };
  if (!valid_body(artifact.copy_body, LifecycleBodyOpKind::CopyField) ||
      !valid_body(artifact.drop_body, LifecycleBodyOpKind::DropField)) {
    error = "nominal semantic witness has an invalid generated body";
    return false;
  }
  if ((artifact.representation.copy != CopyReprKind::Trivial &&
       !artifact.copy_body.empty()) ||
      (artifact.representation.destroy != DestroyReprKind::Trivial &&
       !artifact.drop_body.empty())) {
    error = "non-trivial nominal semantic witness has a generated field body";
    return false;
  }
  for (const auto &argument : artifact.arguments)
    if (!state.verify_public_type(argument, 0, true, error))
      return false;
  if (artifact.value_repr_carrier &&
      !state.verify_public_type(*artifact.value_repr_carrier, 0, false, error))
    return false;
  if (artifact.object_repr_carrier &&
      (!state.verify_public_type(*artifact.object_repr_carrier, 0, false,
                                 error) ||
       artifact.object_repr_carrier->kind != PublicTypeKind::Nominal))
    return false;
  for (const auto &projection : artifact.object_field_projections) {
    constexpr auto BaseCapabilities =
        ProjectionLoad | ProjectionStore | ProjectionTake | ProjectionInit;
    constexpr auto AllCapabilities =
        BaseCapabilities | ProjectionBorrow | ProjectionBorrowMut;
    if (projection.kind >= ObjectFieldProjectionKind::Count ||
        (projection.kind != ObjectFieldProjectionKind::Computed &&
         projection.field_indices.empty()) ||
        (projection.kind == ObjectFieldProjectionKind::Computed &&
         !projection.field_indices.empty()) ||
        (projection.kind != ObjectFieldProjectionKind::Computed &&
         projection.region_indices != projection.field_indices) ||
        (projection.capabilities & ~AllCapabilities) != 0 ||
        (projection.capabilities & BaseCapabilities) != BaseCapabilities ||
        (projection.kind == ObjectFieldProjectionKind::BitPacked &&
         projection.bit_begin >= projection.bit_end) ||
        (projection.kind != ObjectFieldProjectionKind::BitPacked &&
         (projection.bit_begin != 0 || projection.bit_end != 0))) {
      error = "custom object representation has an invalid field projection";
      return false;
    }
    const auto valid_target = [&](const auto &target) {
      return !target || state.valid_entity(*target, PublicEntityKind::Function);
    };
    if (!valid_target(projection.load_target) ||
        !valid_target(projection.store_target) ||
        !valid_target(projection.take_target) ||
        !valid_target(projection.init_target) ||
        !valid_target(projection.borrow_target) ||
        !valid_target(projection.borrow_mut_target)) {
      error = "custom object projector has an invalid canonical target";
      return false;
    }
    const auto target_matches = [&](const auto &target,
                                    ObjectProjectionCapability capability) {
      return target.has_value() ==
             ((projection.capabilities & capability) != 0);
    };
    const auto has_any_target =
        projection.load_target || projection.store_target ||
        projection.take_target || projection.init_target ||
        projection.borrow_target || projection.borrow_mut_target;
    if ((projection.kind == ObjectFieldProjectionKind::Computed &&
         (!target_matches(projection.load_target, ProjectionLoad) ||
          !target_matches(projection.store_target, ProjectionStore) ||
          !target_matches(projection.take_target, ProjectionTake) ||
          !target_matches(projection.init_target, ProjectionInit) ||
          !target_matches(projection.borrow_target, ProjectionBorrow) ||
          !target_matches(projection.borrow_mut_target,
                          ProjectionBorrowMut))) ||
        (projection.kind == ObjectFieldProjectionKind::StableAddress &&
         (projection.capabilities != AllCapabilities || has_any_target)) ||
        (projection.kind == ObjectFieldProjectionKind::BitPacked &&
         (projection.capabilities != BaseCapabilities || has_any_target ||
          projection.bit_end > 32))) {
      error = "custom object projector capabilities disagree with its targets";
      return false;
    }
  }
  const auto valid_shell_target = [&](const auto &target) {
    return !target || state.valid_entity(*target, PublicEntityKind::Function);
  };
  const auto shell_count =
      static_cast<unsigned>(artifact.object_init_target.has_value()) +
      static_cast<unsigned>(artifact.object_copy_init_target.has_value()) +
      static_cast<unsigned>(artifact.object_move_init_target.has_value()) +
      static_cast<unsigned>(artifact.object_drop_target.has_value());
  const auto has_computed = std::ranges::any_of(
      artifact.object_field_projections, [](const auto &projection) {
        return projection.kind == ObjectFieldProjectionKind::Computed;
      });
  if ((shell_count != 0 && shell_count != 4) ||
      (has_computed && shell_count != 4) ||
      !valid_shell_target(artifact.object_init_target) ||
      !valid_shell_target(artifact.object_copy_init_target) ||
      !valid_shell_target(artifact.object_move_init_target) ||
      !valid_shell_target(artifact.object_drop_target)) {
    error = "custom object witness has an invalid shell lifecycle";
    return false;
  }
  if (!std::ranges::all_of(artifact.transitive_specific_fingerprints,
                           &StableFingerprint::hasValue)) {
    error =
        "nominal semantic witness has an invalid transitive specific closure";
    return false;
  }
  if (!std::ranges::is_sorted(
          artifact.transitive_specific_fingerprints, {},
          [](const StableFingerprint &fingerprint) { return fingerprint.hex(); }) ||
      std::adjacent_find(artifact.transitive_specific_fingerprints.begin(),
                         artifact.transitive_specific_fingerprints.end()) !=
          artifact.transitive_specific_fingerprints.end()) {
    error = "nominal semantic witness closure must be unique and sorted";
    return false;
  }
  return true;
}

std::string NominalSemanticWitnessEncodingService::encode(
    NominalSemanticWitnessEncodingState &state) {
  const auto &artifact = state.artifact;
  std::string out("CHNXWIT27");
  auto append_u8 = [&](std::uint8_t value) {
    out.push_back(static_cast<char>(value));
  };
  auto append_u32 = [&](std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
      out.push_back(static_cast<char>((value >> shift) & 0xFFU));
  };
  state.append_entity(out, artifact.nominal_template);
  append_u32(static_cast<std::uint32_t>(artifact.arguments.size()));
  for (const auto &argument : artifact.arguments)
    state.append_type(out, argument);
  state.append_fingerprint(out, artifact.semantic_options_fingerprint);
  state.append_fingerprint(out, artifact.structural_specific_fingerprint);
  append_u8(static_cast<std::uint8_t>(artifact.kind));
  append_u8(static_cast<std::uint8_t>(artifact.representation.value_repr));
  append_u8(static_cast<std::uint8_t>(artifact.representation.init_repr));
  append_u8(static_cast<std::uint8_t>(artifact.representation.ownership));
  append_u8(static_cast<std::uint8_t>(artifact.representation.copy));
  append_u8(static_cast<std::uint8_t>(artifact.representation.move));
  append_u8(static_cast<std::uint8_t>(artifact.representation.destroy));
  append_u8(static_cast<std::uint8_t>(artifact.representation.object_repr));
  append_u8(artifact.concurrency.transferable ? 1U : 0U);
  append_u8(artifact.concurrency.shareable ? 1U : 0U);
  state.append_optional_entity(out, artifact.copy_target);
  state.append_optional_entity(out, artifact.destroy_target);
  state.append_optional_entity(out, artifact.pack_target);
  state.append_optional_entity(out, artifact.init_target);
  append_u8(artifact.value_repr_carrier.has_value() ? 1U : 0U);
  if (artifact.value_repr_carrier)
    state.append_type(out, *artifact.value_repr_carrier);
  append_u8(artifact.object_repr_carrier.has_value() ? 1U : 0U);
  if (artifact.object_repr_carrier)
    state.append_type(out, *artifact.object_repr_carrier);
  append_u32(static_cast<std::uint32_t>(
      artifact.object_field_projections.size()));
  for (const auto &projection : artifact.object_field_projections)
    state.append_projection(out, projection);
  state.append_optional_entity(out, artifact.object_init_target);
  state.append_optional_entity(out, artifact.object_copy_init_target);
  state.append_optional_entity(out, artifact.object_move_init_target);
  state.append_optional_entity(out, artifact.object_drop_target);
  state.append_lifecycle_body(out, artifact.copy_body);
  state.append_lifecycle_body(out, artifact.drop_body);
  append_u32(static_cast<std::uint32_t>(
      artifact.transitive_specific_fingerprints.size()));
  for (const auto &fingerprint : artifact.transitive_specific_fingerprints)
    state.append_fingerprint(out, fingerprint);
  state.append_fingerprint(out, artifact.request_fingerprint);
  state.append_fingerprint(out, artifact.result_fingerprint);
  return out;
}

} // namespace chtholly::compiler::internal
