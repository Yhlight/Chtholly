#include "chtholly/Compiler/PublicInterface.h"
#include "chtholly/Compiler/SharedValueStores.h"

#include "PublicInterfaceServices.h"

#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace chtholly::compiler {

void PublicInterface::collectMetrics(core::CompilerMetrics &metrics,
                                     std::string_view label) const {
  functions_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "bindings"));
  std::size_t parameter_memory = 0;
  for (const auto &block : parameter_blocks_)
    parameter_memory += block.capacity() * sizeof(PublicType);
  metrics.addMemory(
      core::CompilerMetrics::childLabel(label, "parameter_blocks"),
      parameter_memory,
      parameter_memory + parameter_blocks_.capacity() *
                             sizeof(decltype(parameter_blocks_)::value_type));
  nominal_types_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "nominal_bindings"));
  std::size_t value_memory = 0;
  for (const auto &value : value_artifacts_)
    value_memory += sizeof(value) + value.name.capacity() +
                    value.canonical_package.capacity() +
                    value.canonical_module.capacity() +
                    value.canonical_name.capacity();
  metrics.addMemory(core::CompilerMetrics::childLabel(label, "values"),
                    value_memory,
                    value_artifacts_.capacity() * sizeof(PublicValueArtifact));
  metrics.addMemory(core::CompilerMetrics::childLabel(label, "name_index"),
                    function_names_.size() *
                        sizeof(decltype(function_names_)::value_type),
                    function_names_.bucket_count() * sizeof(void *) +
                        function_names_.size() *
                            sizeof(decltype(function_names_)::value_type));
}


void PublicInterfaceRegistry::collectMetrics(core::CompilerMetrics &metrics,
                                             std::string_view label) const {
  metrics.addMemory(
      core::CompilerMetrics::childLabel(label, "interfaces"),
      interfaces_.size() *
          (sizeof(decltype(interfaces_)::value_type) + sizeof(PublicInterface)),
      interfaces_.capacity() * sizeof(decltype(interfaces_)::value_type) +
          interfaces_.size() * sizeof(PublicInterface));
  entities_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "entities"));
  arena_.collectMetrics(metrics,
                        core::CompilerMetrics::childLabel(label, "arena"));
  const auto collect_index = [&](std::string_view name, const auto &index) {
    metrics.addMemory(
        core::CompilerMetrics::childLabel(label, name),
        index.size() *
            sizeof(typename std::decay_t<decltype(index)>::value_type),
        index.bucket_count() * sizeof(void *) +
            index.size() *
                sizeof(typename std::decay_t<decltype(index)>::value_type));
  };
  collect_index("module_index", modules_);
  collect_index("check_ir_index", check_irs_);
  collect_index("entity_index", entity_keys_);
  for (std::uint32_t index = 0; index < interfaces_.size(); ++index)
    interfaces_[index]->collectMetrics(
        metrics, core::CompilerMetrics::childLabel(
                     label, "interface" + std::to_string(index)));
}

} // namespace chtholly::compiler

namespace chtholly::compiler::internal {

} // namespace chtholly::compiler::internal
