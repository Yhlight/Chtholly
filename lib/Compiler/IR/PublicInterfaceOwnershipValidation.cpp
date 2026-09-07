#include "PublicInterfaceServices.h"

#include "PublicInterfaceEncodingInternal.h"

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chtholly::compiler::internal {

namespace {

enum class PublicLoanCarrierCapability : std::uint8_t {
  None,
  Shared,
  Mutable,
  Unknown,
};

} // namespace

std::optional<PublicType>
substituteOwnershipType(const PublicType &type,
                        std::span<const PublicType> arguments) {
  if (type.kind == PublicTypeKind::TypeParameter) {
    if (type.binding_index >= arguments.size())
      return std::nullopt;
    return arguments[type.binding_index];
  }
  PublicType result = type;
  for (auto &argument : result.arguments) {
    auto substituted = substituteOwnershipType(argument, arguments);
    if (!substituted)
      return std::nullopt;
    argument = std::move(*substituted);
  }
  return result;
}

PublicLoanCarrierCapability
loanCarrierCapability(const PublicType &root,
                      const OwnershipNominalResolver &resolve_nominal) {
  constexpr std::size_t MaxVisitedTypes = 4096;
  std::vector<PublicType> worklist{root};
  std::unordered_set<std::string> visited;
  auto capability = PublicLoanCarrierCapability::None;
  bool unresolved = false;
  const auto observe = [&](bool is_mutable) {
    capability =
        is_mutable ? PublicLoanCarrierCapability::Mutable
                   : std::max(capability, PublicLoanCarrierCapability::Shared);
  };

  while (!worklist.empty()) {
    auto current = std::move(worklist.back());
    worklist.pop_back();
    std::string key;
    appendType(key, current);
    if (!visited.insert(std::move(key)).second)
      continue;
    if (visited.size() > MaxVisitedTypes)
      return PublicLoanCarrierCapability::Mutable;

    if (current.kind == PublicTypeKind::Reference) {
      observe(current.reference_mutability ==
              PublicReferenceMutability::Mutable);
      if (capability == PublicLoanCarrierCapability::Mutable)
        return capability;
      continue;
    }
    if (current.kind == PublicTypeKind::Slice) {
      observe(current.slice_mutable);
      if (capability == PublicLoanCarrierCapability::Mutable)
        return capability;
      continue;
    }
    if (current.kind == PublicTypeKind::Array ||
        current.kind == PublicTypeKind::Tuple) {
      for (const auto &element : current.arguments)
        worklist.push_back(element);
      continue;
    }
    if (current.kind == PublicTypeKind::TypeParameter ||
        current.kind == PublicTypeKind::TypeProjection) {
      unresolved = true;
      continue;
    }
    if (current.kind != PublicTypeKind::Nominal)
      continue;
    const auto definition = resolve_nominal(current.nominal_entity);
    if (!definition ||
        definition->fingerprint !=
            current.nominal_entity.expected_fingerprint ||
        current.arguments.size() != definition->generic_parameter_count) {
      unresolved = true;
      continue;
    }
    const auto push_field = [&](const PublicNominalFieldArtifact &field) {
      if (auto type = substituteOwnershipType(field.type, current.arguments))
        worklist.push_back(std::move(*type));
    };
    for (const auto &field : definition->fields)
      push_field(field);
    for (const auto &variant : definition->variants)
      for (const auto &field : variant.fields)
        push_field(field);
  }
  return unresolved ? PublicLoanCarrierCapability::Unknown : capability;
}

bool validReturnCarrierPath(
    const PublicType &root,
    std::span<const CallableReturnSource::CarrierStep> path,
    const OwnershipNominalResolver &resolve_nominal) {
  PublicType current = root;
  std::optional<std::uint32_t> variant;
  for (const auto &step : path) {
    if (current.kind != PublicTypeKind::Nominal)
      return false;
    const auto definition = resolve_nominal(current.nominal_entity);
    if (!definition ||
        current.arguments.size() != definition->generic_parameter_count)
      return false;
    if (step.kind == CallableReturnSource::CarrierStepKind::EnumVariant) {
      if (variant || step.index >= definition->variants.size())
        return false;
      variant = step.index;
      continue;
    }
    if (step.kind != CallableReturnSource::CarrierStepKind::Field)
      return false;
    const PublicNominalFieldArtifact *field = nullptr;
    if (variant) {
      if (step.index >= definition->variants[*variant].fields.size())
        return false;
      field = &definition->variants[*variant].fields[step.index];
      variant.reset();
    } else {
      if (step.index >= definition->fields.size())
        return false;
      field = &definition->fields[step.index];
    }
    auto substituted = substituteOwnershipType(field->type, current.arguments);
    if (!substituted)
      return false;
    current = std::move(*substituted);
  }
  return !variant && (current.kind == PublicTypeKind::Reference ||
                      current.kind == PublicTypeKind::Slice);
}

bool validReturnLoanTypes(std::span<const PublicType> parameters,
                          const PublicType &return_type,
                          const CallableOwnershipSummary &summary,
                          const OwnershipNominalResolver &resolve_nominal) {
  const auto return_capability =
      loanCarrierCapability(return_type, resolve_nominal);
  if (return_capability == PublicLoanCarrierCapability::Unknown)
    return true;
  if ((return_capability == PublicLoanCarrierCapability::None) !=
      summary.return_provenance.empty())
    return false;
  return std::ranges::all_of(
      summary.return_provenance, [&](const CallableReturnSource &source) {
        if (source.region.parameter_index >= parameters.size())
          return false;
        if (!validReturnCarrierPath(return_type, source.carrier_path,
                                    resolve_nominal))
          return false;
        const auto source_capability = loanCarrierCapability(
            parameters[source.region.parameter_index], resolve_nominal);
        return source_capability != PublicLoanCarrierCapability::None &&
               (return_capability != PublicLoanCarrierCapability::Mutable ||
                source_capability == PublicLoanCarrierCapability::Mutable ||
                source_capability == PublicLoanCarrierCapability::Unknown);
      });
}

bool validOwnershipRegionType(std::span<const PublicType> parameters,
                              const OwnershipRegion &region,
                              const OwnershipNominalResolver &resolve_nominal,
                              bool *ends_in_slice_element = nullptr) {
  if (ends_in_slice_element)
    *ends_in_slice_element = false;
  if (region.parameter_index >= parameters.size())
    return false;
  PublicType current = parameters[region.parameter_index];
  bool has_expected_bits = false;
  std::uint32_t expected_begin = 0;
  std::uint32_t expected_end = 0;
  if (current.kind == PublicTypeKind::Reference) {
    auto pointee = current.arguments.front();
    current = std::move(pointee);
  }
  for (const auto &step : region.path) {
    has_expected_bits = false;
    expected_begin = 0;
    expected_end = 0;
    if (step.kind == OwnershipRegionStepKind::Dereference) {
      if (step.index != 0 ||
          (current.kind != PublicTypeKind::Reference &&
           current.kind != PublicTypeKind::RawPointer) ||
          current.arguments.size() != 1)
        return false;
      auto pointee = current.arguments.front();
      current = std::move(pointee);
    } else if (step.kind == OwnershipRegionStepKind::Field) {
      if (current.kind != PublicTypeKind::Nominal)
        return false;
      const auto definition = resolve_nominal(current.nominal_entity);
      if (!definition ||
          definition->fingerprint !=
              current.nominal_entity.expected_fingerprint ||
          current.arguments.size() != definition->generic_parameter_count ||
          step.index >= definition->fields.size())
        return false;
      const auto &field = definition->fields[step.index];
      if (field.projection_kind == PublicObjectProjectionKind::BitPacked) {
        has_expected_bits = true;
        expected_begin = field.bit_begin;
        expected_end = field.bit_end;
      }
      auto field_type = substituteOwnershipType(field.type, current.arguments);
      if (!field_type)
        return false;
      current = std::move(*field_type);
    } else if (step.kind == OwnershipRegionStepKind::StaticElement ||
               step.kind == OwnershipRegionStepKind::AnyElement) {
      if ((current.kind != PublicTypeKind::Array &&
           current.kind != PublicTypeKind::Tuple &&
           current.kind != PublicTypeKind::Slice) ||
          (current.kind == PublicTypeKind::Tuple &&
           step.kind == OwnershipRegionStepKind::AnyElement) ||
          (current.kind == PublicTypeKind::Slice &&
           step.kind != OwnershipRegionStepKind::AnyElement) ||
          (current.kind == PublicTypeKind::Array &&
           step.kind == OwnershipRegionStepKind::StaticElement &&
           step.index >= current.array_bound) ||
          (current.kind == PublicTypeKind::Tuple &&
           step.index >= current.arguments.size()))
        return false;
      const bool slice_element = current.kind == PublicTypeKind::Slice;
      auto element = current.kind == PublicTypeKind::Array
                         ? current.arguments.front()
                         : current.arguments[step.index];
      current = std::move(element);
      if (ends_in_slice_element)
        *ends_in_slice_element = slice_element;
    } else {
      return false;
    }
  }
  return region.has_bit_range == has_expected_bits &&
         (!has_expected_bits || (region.bit_begin == expected_begin &&
                                 region.bit_end == expected_end));
}

bool validOwnershipSummaryTypes(std::span<const PublicType> parameters,
                                const CallableOwnershipSummary &summary,
                                const OwnershipNominalResolver &resolve_nominal) {
  const auto valid_region = [&](const OwnershipRegion &region,
                                bool *slice_element = nullptr) {
    return validOwnershipRegionType(parameters, region, resolve_nominal,
                                    slice_element);
  };
  return std::ranges::all_of(summary.effects,
                             [&](const auto &effect) {
                               bool slice_element = false;
                               if (!valid_region(effect.region, &slice_element))
                                 return false;
                               return !(slice_element &&
                                        effect.kind ==
                                            CallableEffectKind::Initialize);
                             }) &&
         std::ranges::all_of(
             summary.postconditions,
             [&](const auto &postcondition) {
               bool slice_element = false;
               if (!valid_region(postcondition.region, &slice_element))
                 return false;
               return !(slice_element &&
                        (postcondition.outcomes & CallableOutcomeInitialize));
             }) &&
         std::ranges::all_of(
             summary.return_provenance,
             [&](const auto &source) { return valid_region(source.region); });
}

bool validCallbackOwnershipTypes(const PublicType &type,
                                 const OwnershipNominalResolver &resolve_nominal) {
  for (const auto &argument : type.arguments)
    if (!validCallbackOwnershipTypes(argument, resolve_nominal))
      return false;
  if (type.kind != PublicTypeKind::CFunctionPointer)
    return type.callable_contract == CallableOwnershipSummary{};
  if (type.arguments.empty())
    return false;
  return validOwnershipSummaryTypes(
      std::span(type.arguments).first(type.arguments.size() - 1),
      type.callable_contract, resolve_nominal);
}

bool templateMatchesSignature(const GenericTemplateArtifact &generic_template,
                              std::uint32_t generic_parameter_count,
                              std::span<const PublicType> parameters,
                              PublicType return_type) {
  if (generic_template.generic_parameter_count != generic_parameter_count ||
      generic_template.parameter_count != parameters.size() ||
      generic_template.declaration.results.size() != parameters.size() + 1 ||
      !std::equal(parameters.begin(), parameters.end(),
                  generic_template.local_types.begin()) ||
      !std::equal(parameters.begin(), parameters.end(),
                  generic_template.declaration.results.begin()))
    return false;
  return generic_template.declaration.results.back() == return_type;
}



} // namespace chtholly::compiler::internal
