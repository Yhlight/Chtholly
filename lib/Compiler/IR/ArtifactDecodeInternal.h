#pragma once

#include "ArtifactDecode.h"

#include "chtholly/Compiler/NominalTypeArtifact.h"
#include "chtholly/Compiler/PublicInterface.h"

#include <optional>
#include <string>
#include <string_view>

namespace chtholly::compiler::internal {

[[nodiscard]] std::optional<PublicNominalTypeArtifact>
decodePublicNominalTypeArtifact(std::string_view bytes, std::string &error,
                                ArtifactDecodeContext &context);

[[nodiscard]] std::optional<NominalSemanticWitnessArtifact>
decodeNominalSemanticWitnessArtifact(std::string_view bytes,
                                     std::string &error,
                                     ArtifactDecodeContext &context);

[[nodiscard]] std::optional<ForeignResourceProtocol>
decodeForeignResourceProtocol(std::string_view bytes, std::uint32_t type_count,
                              std::string &error,
                              ArtifactDecodeContext &context);

} // namespace chtholly::compiler::internal
