#include "CompilerPipelineInternal.h"

#include "chtholly/Compiler/CompilationUnit.h"

#include <set>

namespace chtholly {

namespace {
std::string moduleIdentityKey(std::string_view package,
                              std::string_view module) {
  return std::string(package) + "\n" + std::string(module);
}
} // namespace

bool CompilerLinkClosureService::collect(
    const CompilerDriverPlan &plan, std::size_t root_package,
    const compiler::CompilationUnit &root_unit,
    const std::map<std::string, std::string> &object_paths_by_module,
    const std::map<std::string, const compiler::PackageModuleArtifact *>
        &artifacts_by_module,
    bool component,
    const std::function<bool(std::string_view)> &is_hosted_async_symbol,
    std::vector<std::string> &object_paths, std::string &error) {
  std::set<std::string> reachable_modules;
  std::vector<std::string> pending_modules;
  const auto root_key = moduleIdentityKey(
      plan.packages[root_package].package_name,
      std::string(root_unit.moduleName()));
  reachable_modules.insert(root_key);
  pending_modules.push_back(root_key);
  if (component) {
    for (const auto &component_export : plan.packages[root_package].component_exports) {
      const auto split = component_export.rfind("::");
      if (split == std::string::npos || split == 0 ||
          split + 2 == component_export.size()) {
        error = "component export requires module::function spelling: '" +
                component_export + "'";
        return false;
      }
      const auto key = moduleIdentityKey(
          plan.packages[root_package].package_name,
          component_export.substr(0, split));
      if (!artifacts_by_module.contains(key)) {
        error = "component export names missing module '" +
                component_export.substr(0, split) + "'";
        return false;
      }
      if (reachable_modules.insert(key).second)
        pending_modules.push_back(key);
    }
  }
  for (std::size_t index = 0; index < pending_modules.size(); ++index) {
    const auto &key = pending_modules[index];
    const auto artifact = artifacts_by_module.find(key);
    if (artifact == artifacts_by_module.end()) {
      error = "compiler link closure is missing compiled module '" + key + "'";
      return false;
    }
    for (const auto &symbol : artifact->second->required_foreign_symbols) {
      if (symbol.external_symbol.starts_with(
              "chtholly_compiler_hosted_async_v1_") &&
          !is_hosted_async_symbol(symbol.external_symbol)) {
        error = "compiler hosted async runtime does not provide foreign symbol '" +
                symbol.external_symbol + "'";
        return false;
      }
    }
    for (const auto &dependency : artifact->second->module_dependencies) {
      const auto dependency_key =
          moduleIdentityKey(dependency.package_name, dependency.module_name);
      if (!artifacts_by_module.contains(dependency_key)) {
        error = "compiler link closure references missing module '" +
                dependency.package_name + "::" + dependency.module_name + "'";
        return false;
      }
      if (reachable_modules.insert(dependency_key).second)
        pending_modules.push_back(dependency_key);
    }
  }
  object_paths.clear();
  for (const auto &[key, path] : object_paths_by_module)
    if (reachable_modules.contains(key))
      object_paths.push_back(path);
  return true;
}

} // namespace chtholly
