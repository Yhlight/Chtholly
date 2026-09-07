#include "chtholly/Compiler/IncrementalDependencies.h"

#include <algorithm>
#include <ranges>
#include <sstream>
#include <tuple>
#include <string_view>

namespace chtholly::compiler {
namespace {
auto invalidationKey(const UnitInvalidation &value) {
  return std::tuple(value.reason, std::string_view(value.provider.package_name),
                    std::string_view(value.provider.module_name),
                    std::string_view(value.binding_name));
}
} // namespace

bool IncrementalCompilationPlan::verify(std::string &error) const {
  error.clear();
  std::string_view previous_module;
  for (const auto &decision : decisions_) {
    const auto is_removed = decision.action == UnitCompilationAction::Removed;
    const auto has_only_removal = decision.invalidations.size() == 1 &&
                                  decision.invalidations.front().reason ==
                                      UnitInvalidationReason::ModuleRemoved;
    if (decision.module_name.empty() ||
        decision.action >= UnitCompilationAction::Count ||
        (!previous_module.empty() && previous_module >= decision.module_name) ||
        ((decision.action == UnitCompilationAction::Reuse) !=
         decision.invalidations.empty()) ||
        is_removed != has_only_removal ||
        (!is_removed &&
         std::ranges::any_of(decision.invalidations, [](const auto &reason) {
           return reason.reason == UnitInvalidationReason::ModuleRemoved;
         }))) {
      error = "incremental compilation plan has an invalid decision";
      return false;
    }
    for (std::size_t index = 0; index < decision.invalidations.size();
         ++index) {
      const auto &invalidation = decision.invalidations[index];
      const auto has_provider = !invalidation.provider.package_name.empty() ||
                                !invalidation.provider.module_name.empty();
      if (invalidation.reason >= UnitInvalidationReason::Count ||
          ((invalidation.reason ==
                UnitInvalidationReason::ImportedModuleRemoved ||
            invalidation.reason ==
                UnitInvalidationReason::EntityBindingChanged ||
            invalidation.reason ==
                UnitInvalidationReason::LifecycleCallableChanged ||
            invalidation.reason == UnitInvalidationReason::ExportSetChanged ||
            invalidation.reason ==
                UnitInvalidationReason::NominalBindingChanged ||
            invalidation.reason ==
                UnitInvalidationReason::RelocationClosureChanged) !=
           has_provider) ||
          (has_provider && (invalidation.provider.package_name.empty() ||
                            invalidation.provider.module_name.empty())) ||
          (((invalidation.reason ==
                 UnitInvalidationReason::EntityBindingChanged ||
             invalidation.reason ==
                 UnitInvalidationReason::LifecycleCallableChanged ||
             invalidation.reason ==
                 UnitInvalidationReason::NominalBindingChanged ||
             invalidation.reason ==
                 UnitInvalidationReason::RelocationClosureChanged)) !=
           !invalidation.binding_name.empty()) ||
          (index != 0 && invalidationKey(decision.invalidations[index - 1]) >=
                             invalidationKey(invalidation))) {
        error = "incremental compilation plan has an invalid reason: " +
            std::string(unitInvalidationReasonName(invalidation.reason)) +
            " provider=" + invalidation.provider.package_name + "/" +
            invalidation.provider.module_name + " binding=" + invalidation.binding_name;
        return false;
      }
    }
    previous_module = decision.module_name;
  }
  return true;
}

std::string IncrementalCompilationPlan::print() const {
  std::ostringstream out;
  for (const auto &decision : decisions_) {
    out << decision.module_name << ' '
        << unitCompilationActionName(decision.action);
    if (decision.invalidations.empty()) {
      out << '\n';
      continue;
    }
    out << '\n';
    for (const auto &invalidation : decision.invalidations) {
      out << "  " << unitInvalidationReasonName(invalidation.reason);
      if (!invalidation.provider.package_name.empty())
        out << " provider=" << invalidation.provider.package_name << '/'
            << invalidation.provider.module_name;
      if (!invalidation.binding_name.empty())
        out << " binding=" << invalidation.binding_name;
      out << '\n';
    }
  }
  return out.str();
}

void IncrementalCompilationPlan::collectMetrics(core::CompilerMetrics &metrics,
                                                std::string_view label) const {
  std::size_t invalidation_count = 0;
  std::size_t string_size = 0;
  for (const auto &decision : decisions_) {
    string_size += decision.module_name.size();
    invalidation_count += decision.invalidations.size();
    for (const auto &invalidation : decision.invalidations)
      string_size += invalidation.provider.package_name.size() +
                     invalidation.provider.module_name.size() +
                     invalidation.binding_name.size();
  }
  metrics.addMemory(core::CompilerMetrics::childLabel(label, "decisions"),
                    decisions_.size() * sizeof(UnitCompilationDecision),
                    decisions_.capacity() * sizeof(UnitCompilationDecision));
  metrics.addMemory(core::CompilerMetrics::childLabel(label, "invalidations"),
                    invalidation_count * sizeof(UnitInvalidation),
                    invalidation_count * sizeof(UnitInvalidation));
  metrics.addMemory(core::CompilerMetrics::childLabel(label, "strings"),
                    string_size, string_size);
}


} // namespace chtholly::compiler

