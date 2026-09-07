#pragma once

#include "chtholly/Driver/DeploymentManifest.h"

#include <string>
#include <vector>

namespace chtholly {

struct ComponentGenerationInfo {
  std::string id;
  DeploymentManifest manifest;
};

[[nodiscard]] bool installComponentGeneration(
    const std::string &root, const std::string &manifest_path,
    ComponentGenerationInfo &generation, std::string &error);
[[nodiscard]] bool activateComponentGeneration(const std::string &root,
                                               std::string_view generation_id,
                                               std::string &error);
[[nodiscard]] bool rollbackComponentGeneration(const std::string &root,
                                               std::string &error);
[[nodiscard]] bool activeComponentGeneration(const std::string &root,
                                             ComponentGenerationInfo &generation,
                                             std::string &error);
[[nodiscard]] bool removeComponentGeneration(const std::string &root,
                                             std::string_view generation_id,
                                             std::string &error);

} // namespace chtholly
