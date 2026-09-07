#pragma once

#include "chtholly/Basic/TargetInfo.h"
#include "chtholly/Driver/CompilerInvocation.h"

#include <optional>
#include <string>

namespace chtholly {

struct TargetConfig {
  TargetInfo info;
  std::string sysroot_path;
  std::string linker_path;
  std::string object_extension = "o";
  bool is_host_compatible = true;
  bool debug_info = false;
};

struct TargetConfigInput {
  std::string root_directory;
  std::string triple;
  std::string sysroot;
  std::string linker;
};

std::optional<TargetConfig>
resolveTargetConfig(const CompilerInvocation &invocation,
                    const TargetConfigInput &manifest, std::string &error);

std::string hostTargetTriple();
bool isMsvcStyleLinker(const std::string &linker);

} // namespace chtholly
