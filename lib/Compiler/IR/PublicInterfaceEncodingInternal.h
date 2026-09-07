#pragma once

#include "chtholly/Compiler/PublicInterface.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace chtholly::compiler::internal {

void appendU32(std::string &out, std::uint32_t value);
void appendU64(std::string &out, std::uint64_t value);
void appendField(std::string &out, std::string_view value);
void appendEntityReference(std::string &out,
                           const PublicEntityReferenceArtifact &entity);
void appendType(std::string &out, const PublicType &type);
void appendSemanticContract(std::string &out,
                            const CallableSemanticContract &contract);
void appendRegion(std::string &out, const OwnershipRegion &region);
void appendOwnershipSummary(std::string &out,
                            const CallableOwnershipSummary &summary);
void appendForeignSignature(std::string &out,
                            const ForeignAbiSignature &signature);
void appendTemplate(std::string &out,
                    const GenericTemplateArtifact &generic_template);
void appendConstantValue(std::string &out, const PublicConstantValue &value);
[[nodiscard]] StableFingerprint entityFingerprint(
    std::string_view package, std::string_view module, std::string_view name,
    const std::optional<PublicEntityReferenceArtifact> &member_owner,
    PublicFunctionArtifact::MemberKind member_kind,
    std::uint32_t generic_parameter_count,
    std::span<const PublicType> parameters, PublicType return_type,
    const std::optional<PublicType> &error_type,
    PublicFunctionExecutionKind execution_kind,
    const PublicCoroutineConstructorABI &coroutine_constructor,
    const PublicNominalConstructorABI &nominal_constructor,
    const CallableSemanticContract &semantic_contract,
    CompilerIntrinsicRole intrinsic_role,
    const CallableOwnershipSummary &ownership_summary,
    const std::optional<GenericTemplateArtifact> &generic_template,
    PublicCallableDeclarationKind declaration_kind =
        PublicCallableDeclarationKind::Definition,
    bool is_unsafe = false, bool is_const = false,
    std::string_view foreign_abi = {},
    const std::optional<ForeignAbiSignature> &foreign_signature = {},
    std::span<const std::string> parameter_names = {},
    std::span<const std::optional<PublicConstantValue>> default_arguments = {},
    std::span<const PublicInterfaceConstraintArtifact> constraints = {},
    const std::optional<interop::ArtifactReference> &interop_artifact = {},
    std::string_view external_symbol = {});
[[nodiscard]] StableFingerprint valueFingerprint(
    const PublicValueArtifact &value);
[[nodiscard]] StableFingerprint interfaceFingerprint(
    const PublicInterface &interface_value,
    const PublicInterfaceRegistry &registry, const SharedValueStores &values);
[[nodiscard]] StableFingerprint artifactFingerprint(
    std::string_view package, std::string_view module,
    std::span<const PublicFunctionArtifact> functions,
    std::span<const PublicNominalTypeArtifact> nominal_types = {},
    std::span<const PublicValueArtifact> values = {},
    std::span<const PublicInterfaceDeclarationArtifact> interfaces = {},
    std::span<const PublicTypeAliasArtifact> type_aliases = {},
    std::span<const PublicInterfaceWitnessArtifact> interface_witnesses = {});

} // namespace chtholly::compiler::internal
