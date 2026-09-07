#include "chtholly/Compiler/IncrementalDependencies.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <tuple>

namespace chtholly::compiler {
namespace {

bool validModuleDependencies(const std::vector<ModuleIdentity> &values,
                             std::string_view owner, std::string &error) {
  for (std::size_t index = 0; index < values.size(); ++index) {
    const auto &value = values[index];
    if (value.package_name.empty() || value.module_name.empty() ||
        (index != 0 && values[index - 1] >= value)) {
      error = std::string(owner) + " has invalid module dependencies";
      return false;
    }
  }
  return true;
}

bool validForeignSymbols(const std::vector<ForeignSymbolRequirement> &values,
                         std::string_view owner, std::string &error) {
  if (!std::ranges::is_sorted(values) ||
      std::adjacent_find(values.begin(), values.end()) != values.end() ||
      std::ranges::any_of(values, [](const auto &value) {
        return value.logical_name.empty() || value.external_symbol.empty() ||
               !value.signature_fingerprint.hasValue() ||
               value.calling_convention >= ForeignCallingConvention::Count;
      })) {
    error = std::string(owner) + " has invalid foreign symbol requirements";
    return false;
  }
  for (std::size_t index = 1; index < values.size(); ++index)
    if (values[index - 1].logical_name == values[index].logical_name) {
      error = std::string(owner) +
              " has duplicate foreign symbol requirements";
      return false;
    }
  return true;
}

auto observationKey(const DependencyObservation &value) {
  return std::tuple(value.kind, std::string_view(value.provider.package_name),
                    std::string_view(value.provider.module_name),
                    std::string_view(value.binding_name),
                    std::string_view(value.canonical_provider.package_name),
                    std::string_view(value.canonical_provider.module_name),
                    std::string_view(value.canonical_name),
                    value.lifecycle_role, value.expected_fingerprint.hex());
}

} // namespace

bool PackageModuleArtifact::verify(std::string &error) const {
  PackageModuleCheckArtifact check{
      .source_fingerprint = source_fingerprint,
      .unit_kind = unit_kind,
      .public_interface = public_interface,
      .module_dependencies = module_dependencies,
      .required_foreign_symbols = required_foreign_symbols,
      .observations = observations,
      .specific_fingerprint = specific_fingerprint};
  if (!check.verify(error) || unit_kind >= CompilationUnitKind::Count ||
      emission_role >= ModuleEmissionRole::Count ||
      !object_fingerprint.hasValue() ||
      specializations.size() > std::numeric_limits<std::uint32_t>::max() ||
      !std::ranges::is_sorted(
          specializations, {},
          [](const ConcreteSpecializationReference &ref) {
            return ref.request_fingerprint.hex();
          }) ||
      std::adjacent_find(
          specializations.begin(), specializations.end(),
          [](const auto &lhs, const auto &rhs) {
            return lhs.request_fingerprint == rhs.request_fingerprint;
          }) != specializations.end() ||
      std::ranges::any_of(specializations, [](const auto &reference) {
        return !reference.request_fingerprint.hasValue() ||
               !reference.component_fingerprint.hasValue();
      })) {
    if (error.empty())
      error = "incremental module artifact has invalid content";
    return false;
  }
  const auto valid_nominal_references = [](const auto &references) {
    return references.size() <= std::numeric_limits<std::uint32_t>::max() &&
           std::ranges::is_sorted(
               references, {}, [](const auto &reference) {
                 return reference.request_fingerprint.hex();
               }) &&
           std::adjacent_find(
               references.begin(), references.end(),
               [](const auto &lhs, const auto &rhs) {
                 return lhs.request_fingerprint == rhs.request_fingerprint;
               }) == references.end() &&
           std::ranges::none_of(references, [](const auto &reference) {
             return !reference.request_fingerprint.hasValue() ||
                    !reference.result_fingerprint.hasValue();
           });
  };
  if (nominal_type_specifics.size() != nominal_semantic_witnesses.size() ||
      !valid_nominal_references(nominal_type_specifics) ||
      !valid_nominal_references(nominal_semantic_witnesses) ||
      !valid_nominal_references(nominal_type_layouts)) {
    error = "incremental module has invalid nominal artifact references";
    return false;
  }
  return true;
}

bool PackageModuleCheckArtifact::verify(std::string &error) const {
  if (!source_fingerprint.hasValue() ||
      unit_kind >= CompilationUnitKind::Count ||
      !specific_fingerprint.hasValue() || !public_interface.verify(error) ||
      observations.size() > std::numeric_limits<std::uint32_t>::max()) {
    if (error.empty())
      error = "package check module artifact has invalid content";
    return false;
  }
  if (!validModuleDependencies(module_dependencies, "package check module",
                               error) ||
      !validForeignSymbols(required_foreign_symbols, "package check module",
                           error))
    return false;
  for (std::size_t index = 0; index < observations.size(); ++index) {
    const auto &observation = observations[index];
    if (observation.kind >= DependencyObservationKind::Count ||
        observation.provider.package_name.empty() ||
        observation.provider.module_name.empty() ||
        observation.provider.package_name.size() >
            std::numeric_limits<std::uint32_t>::max() ||
        observation.provider.module_name.size() >
            std::numeric_limits<std::uint32_t>::max() ||
        observation.binding_name.size() >
            std::numeric_limits<std::uint32_t>::max() ||
        observation.canonical_provider.package_name.size() >
            std::numeric_limits<std::uint32_t>::max() ||
        observation.canonical_provider.module_name.size() >
            std::numeric_limits<std::uint32_t>::max() ||
        observation.canonical_name.size() >
            std::numeric_limits<std::uint32_t>::max() ||
        !observation.expected_fingerprint.hasValue() ||
        ((observation.kind == DependencyObservationKind::EntityBinding ||
          observation.kind == DependencyObservationKind::LifecycleCallable ||
          observation.kind == DependencyObservationKind::NominalBinding ||
          observation.kind == DependencyObservationKind::RelocationClosure) !=
         !observation.binding_name.empty()) ||
        ((observation.kind == DependencyObservationKind::EntityBinding ||
          observation.kind == DependencyObservationKind::LifecycleCallable ||
          observation.kind == DependencyObservationKind::NominalBinding ||
          observation.kind == DependencyObservationKind::RelocationClosure) !=
         (!observation.canonical_provider.package_name.empty() &&
          !observation.canonical_provider.module_name.empty() &&
          !observation.canonical_name.empty())) ||
        ((observation.kind == DependencyObservationKind::LifecycleCallable) !=
         (observation.lifecycle_role < LifecycleObservationRole::Count &&
          observation.witness_fingerprint.hasValue() &&
          observation.specific_closure_fingerprint.hasValue())) ||
        (index != 0 && observationKey(observations[index - 1]) >=
                           observationKey(observation))) {
      error = "package check module artifact has an invalid observation: " +
          std::string(moduleName()) + " kind=" + std::to_string(static_cast<unsigned>(observation.kind)) +
          " provider=" + observation.provider.package_name + "/" + observation.provider.module_name +
          " binding=" + observation.binding_name + " witness=" + observation.witness_fingerprint.hex() +
          " specific=" + observation.specific_closure_fingerprint.hex();
      return false;
    }
  }
  return true;
}

} // namespace chtholly::compiler
