#pragma once

#include "chtholly/Compiler/NominalTypeArtifact.h"
#include "chtholly/Compiler/SemIR.h"

#include <cstdint>
#include <optional>
#include <string>

namespace chtholly::compiler {

struct SemanticTypeLayout {
  std::uint64_t size = 0;
  std::uint64_t alignment = 0;
};

[[nodiscard]] std::optional<SemanticTypeLayout>
querySemanticTypeLayout(const SemIR &sem_ir, TypeId type,
                        const TargetLayoutConfig &target, std::string &error);

} // namespace chtholly::compiler
