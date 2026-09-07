#pragma once

#include "chtholly/Compiler/ComponentABI2Protocol.h"

namespace chtholly::compiler {

inline constexpr std::size_t ComponentAbi2ArtifactMaxBytes = 1024 * 1024;

// The same verified envelope is used for disk replay and provider query.
[[nodiscard]] std::string
encodeComponentAbi2Artifact(const ComponentAbi2Descriptor &descriptor,
                            std::string &error);
[[nodiscard]] std::optional<ComponentAbi2Descriptor>
decodeComponentAbi2Artifact(std::string_view bytes,
                            ComponentAbi2DescriptorError &kind,
                            std::string &error);
[[nodiscard]] std::optional<ComponentAbi2Descriptor>
readComponentAbi2Artifact(std::string_view path,
                          ComponentAbi2DescriptorError &kind,
                          std::string &error);
[[nodiscard]] bool writeComponentAbi2Artifact(
    std::string_view path, const ComponentAbi2Descriptor &descriptor,
    std::string &error);

} // namespace chtholly::compiler
