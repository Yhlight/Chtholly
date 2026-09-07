#include "PublicInterfaceServices.h"

#include <limits>
#include <ranges>
#include <unordered_set>

namespace chtholly::compiler::internal {

bool PublicInterfaceArtifactVerificationService::verifyFunctionContract(
    const PublicFunctionArtifact &function,
    const FunctionValidationCallbacks &callbacks, std::string &error) {
  const auto invalid =
      function.name.empty() || function.canonical_package.empty() ||
      function.canonical_module.empty() || function.canonical_name.empty() ||
      function.name == "main" || function.canonical_name == "main" ||
      function.name.size() > std::numeric_limits<std::uint32_t>::max() ||
      function.canonical_package.size() >
          std::numeric_limits<std::uint32_t>::max() ||
      function.canonical_module.size() >
          std::numeric_limits<std::uint32_t>::max() ||
      function.canonical_name.size() >
          std::numeric_limits<std::uint32_t>::max() ||
      function.parameters.size() >
          std::numeric_limits<std::uint32_t>::max() ||
      !callbacks.valid_type(function.return_type,
                            function.generic_parameter_count, true) ||
      function.execution_kind >= PublicFunctionExecutionKind::Count ||
      function.intrinsic_role >= CompilerIntrinsicRole::Count ||
      (function.intrinsic_role != CompilerIntrinsicRole::None &&
       (function.canonical_package != "std" ||
        (function.canonical_module != "std" &&
         !function.canonical_module.starts_with("std::")))) ||
      !callbacks.valid_coroutine_constructor(function.execution_kind,
                                              function.coroutine_constructor) ||
      !callbacks.valid_nominal_constructor(function.semantic_contract,
                                            function.return_type,
                                            function.nominal_constructor) ||
      (function.execution_kind == PublicFunctionExecutionKind::Immediate &&
       (function.error_type.has_value() ||
        function.coroutine_constructor != PublicCoroutineConstructorABI{})) ||
      (function.execution_kind == PublicFunctionExecutionKind::Async &&
       function.coroutine_constructor !=
           PublicCoroutineConstructorABI{1, true, true, true, true}) ||
      (function.error_type &&
       !callbacks.valid_type(*function.error_type,
                             function.generic_parameter_count, true)) ||
      !callbacks.valid_reference_provenance(
          function.return_type, function.parameters.size(), true) ||
      (function.error_type &&
       !callbacks.valid_reference_provenance(
           *function.error_type, function.parameters.size(), true)) ||
      !function.semantic_contract.verify(function.generic_parameter_count,
                                         error) ||
      !callbacks.semantic_contract_matches_signature(
          function.parameters, function.return_type,
          function.semantic_contract) ||
      !callbacks.semantic_contract_matches_effects(
          function.semantic_contract, function.ownership_summary) ||
      !function.ownership_summary.verify(
          static_cast<std::uint32_t>(function.parameters.size()), error) ||
      !callbacks.ownership_matches_signature(
          function.parameters, function.return_type,
          function.ownership_summary) ||
      !callbacks.valid_return_loan_types(
          function.parameters, function.return_type,
          function.ownership_summary) ||
      function.declaration_kind >= PublicCallableDeclarationKind::Count ||
      (function.declaration_kind == PublicCallableDeclarationKind::Foreign
           ? (function.execution_kind !=
                  PublicFunctionExecutionKind::Immediate ||
              !function.is_unsafe || function.foreign_abi != "C" ||
              function.external_symbol.empty() ||
              !callbacks.foreign_signature_matches(
                  function.parameters, function.return_type,
                  function.foreign_signature,
                  function.interop_artifact.has_value()))
           : ((function.is_unsafe &&
               function.intrinsic_role == CompilerIntrinsicRole::None) ||
              !function.foreign_abi.empty() ||
              !function.external_symbol.empty() ||
              function.foreign_signature.has_value())) ||
      (function.is_const &&
       (function.declaration_kind !=
            PublicCallableDeclarationKind::Definition ||
        function.execution_kind != PublicFunctionExecutionKind::Immediate ||
        function.is_unsafe)) ||
      function.parameter_names.size() != function.parameters.size() ||
      function.default_arguments.size() != function.parameters.size() ||
      std::ranges::any_of(function.parameter_names,
                          [](const auto &name) { return name.empty(); }) ||
      ([&] {
        std::unordered_set<std::string_view> names;
        return std::ranges::any_of(function.parameter_names,
                                   [&](const auto &name) {
                                     return !names.insert(name).second;
                                   });
      }()) ||
      ([&] {
        bool saw_default = false;
        for (std::size_t index = 0;
             index < function.default_arguments.size(); ++index) {
          const auto &value = function.default_arguments[index];
          if (!value) {
            if (saw_default)
              return true;
            continue;
          }
          saw_default = true;
          if (value->type != function.parameters[index] ||
              !callbacks.valid_constant_shape(*value))
            return true;
        }
        return false;
      }()) ||
      !function.entity_fingerprint.hasValue() ||
      (function.interop_artifact &&
       (function.declaration_kind != PublicCallableDeclarationKind::Foreign ||
        !callbacks.valid_foreign_operation(
            *function.interop_artifact, function.parameters,
            function.return_type))) ||
      (function.member_owner &&
       (function.member_owner->kind != PublicEntityKind::NominalType ||
        function.member_owner->canonical_package.empty() ||
        function.member_owner->canonical_module.empty() ||
        function.member_owner->canonical_name.empty() ||
        !function.member_owner->expected_fingerprint.hasValue())) ||
      !callbacks.member_signature_matches_owner(
          function.member_owner, function.member_kind, function.parameters) ||
      std::ranges::any_of(function.parameters, [&](const PublicType &type) {
        return !callbacks.valid_type(type, function.generic_parameter_count,
                                     false) ||
               !callbacks.valid_reference_provenance(
                   type, function.parameters.size(), true);
      }) ||
      (function.generic_template.has_value() !=
       ((function.generic_parameter_count != 0 || function.is_const) &&
        function.declaration_kind ==
            PublicCallableDeclarationKind::Definition)) ||
      (function.generic_template &&
       (!function.generic_template->verify(error) ||
        !callbacks.template_matches_signature(
            *function.generic_template, function.generic_parameter_count,
            function.parameters, function.return_type))) ||
      std::ranges::any_of(function.constraints, [&](const auto &constraint) {
        return !callbacks.valid_constraint(constraint,
                                           function.generic_parameter_count);
      });
  if (!invalid)
    return true;
  const auto detail = std::move(error);
  error = "public interface artifact has an invalid function binding `" +
          function.name + "`" +
          (detail.empty() ? std::string{} : ": " + detail);
  return false;
}

} // namespace chtholly::compiler::internal
