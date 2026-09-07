#include "CompilerPipelineInternal.h"

#include "chtholly/Compiler/CFDL.h"
#include "chtholly/Compiler/Lexer.h"
#include "chtholly/Compiler/Parser.h"
#include "chtholly/Driver/CompilerInputFileSystem.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>

namespace chtholly {

std::optional<CompilerSourceInventory>
CompilerPipelinePlanningService::inspectSourceInventory(
    std::span<const std::string> source_paths, LanguageVersion language_version,
    const CompilerInputFileSystem &file_system, std::string &error) {
  CompilerSourceInventory inventory;
  compiler::SharedValueStores values;
  for (const auto &path : source_paths) {
    CompilerInputFile file;
    if (!file_system.readText(path, file, error) || !file.exists || !file.text) {
      if (error.empty())
        error = "input is missing";
      error = "failed to inspect compiler source '" + path + "': " + error;
      return std::nullopt;
    }
    compiler::SourceBuffer source(compiler::SourceInput(path, *file.text));
    if (pathForFileSystem(path).extension() == ".cfdl") {
      compiler::CFDLSyntaxFile syntax;
      std::vector<compiler::CFDLDiagnostic> diagnostics;
      (void)compiler::parseCFDL(source, syntax, diagnostics);
      if (!syntax.module_name.empty())
        inventory.declared_modules.push_back(syntax.module_name);
      continue;
    }
    compiler::DiagnosticEmitter diagnostics;
    auto tokens = compiler::lex(source, values, diagnostics, language_version);
    auto tree = compiler::parse(tokens, diagnostics);
    if (diagnostics.hasError())
      continue;
    const auto root = compiler::NodeId(static_cast<std::uint32_t>(tree.size() - 1));
    for (const auto declaration : tree.children(root)) {
      const auto kind = tree.kind(declaration);
      if (kind == compiler::NodeKind::FunctionDecl &&
          std::ranges::any_of(tree.children(declaration), [&](const auto child) {
            return tree.kind(child) == compiler::NodeKind::AsyncModifier;
          }))
        inventory.uses_candidate_async = true;
      if (kind != compiler::NodeKind::ImportDecl &&
          kind != compiler::NodeKind::ModuleDecl)
        continue;
      const auto children = tree.children(declaration);
      if (children.size() != 1 ||
          tree.kind(children[0]) != compiler::NodeKind::ModulePath)
        continue;
      std::string module_name;
      for (const auto component : tree.children(children[0])) {
        if (tree.kind(component) != compiler::NodeKind::Name) {
          module_name.clear();
          break;
        }
        if (!module_name.empty())
          module_name += "::";
        module_name += tree.tokens().text(tree.token(component));
      }
      if (kind == compiler::NodeKind::ImportDecl) {
        inventory.imports_standard_library =
            inventory.imports_standard_library || module_name == "std" ||
            module_name.starts_with("std::");
        inventory.imported_modules.push_back(std::move(module_name));
      } else {
        inventory.declared_modules.push_back(std::move(module_name));
      }
    }
  }
  std::ranges::sort(inventory.declared_modules);
  std::ranges::sort(inventory.imported_modules);
  inventory.imported_modules.erase(
      std::unique(inventory.imported_modules.begin(),
                  inventory.imported_modules.end()),
      inventory.imported_modules.end());
  return inventory;
}

} // namespace chtholly
