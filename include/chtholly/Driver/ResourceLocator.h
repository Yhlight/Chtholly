#pragma once

#include <optional>
#include <string>
#include <vector>

namespace chtholly {

struct CompilerInvocation;

struct RuntimeSymbolMapping {
  std::string source_symbol;
  std::string runtime_symbol;
};

struct ResourceLayout {
  std::string resource_dir;
  std::string standard_library_dir;
  std::string runtime_library_path;
  std::string runtime_link_manifest_path;
  std::vector<std::string> runtime_link_libraries;
  std::vector<RuntimeSymbolMapping> runtime_symbol_mappings;
};

std::optional<ResourceLayout>
locateCompilerResources(const CompilerInvocation &invocation,
                        std::string &error);

} // namespace chtholly
