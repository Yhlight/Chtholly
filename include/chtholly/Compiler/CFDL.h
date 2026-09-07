#pragma once

#include "chtholly/Compiler/ForeignDeclaration.h"
#include "chtholly/Compiler/SemIR.h"
#include "chtholly/Compiler/Source.h"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chtholly::compiler {

class DiagnosticEmitter;

struct CFDLDiagnostic {
  enum class Kind : std::uint8_t {
    InvalidSyntax,
    InvalidModule,
    InvalidImport,
    DuplicateName,
    UnknownType,
    UnknownCallable,
    InvalidCallable,
    IncompleteCarrier,
    InvalidCarrier,
    CompletionFailed,
    InvalidProtocol,
  } kind = Kind::InvalidSyntax;
  std::uint32_t offset = 0;
  std::uint32_t length = 1;
  std::string message;

  CFDLDiagnostic() = default;
  CFDLDiagnostic(std::uint32_t offset, std::string message)
      : offset(offset), message(std::move(message)) {}
  CFDLDiagnostic(Kind kind, std::uint32_t offset, std::uint32_t length,
                 std::string message)
      : kind(kind), offset(offset), length(length),
        message(std::move(message)) {}
};

enum class CFDLFlowQualifier : std::uint8_t {
  Value,
  Owned,
  Ref,
  RefMut,
  View,
  ViewMut,
  Move,
  Out,
  InOut,
  Count,
};

enum class CFDLWhereFactKind : std::uint8_t {
  Escapes,
  Stores,
  Derives,
  Invokes,
  Obliges,
  Discharges,
  Requires,
  Count,
};

struct CFDLFlowTypeSyntax {
  CFDLFlowQualifier qualifier = CFDLFlowQualifier::Value;
  std::string physical_type;
};

struct CFDLWhereFactSyntax {
  CFDLWhereFactKind kind = CFDLWhereFactKind::Count;
  std::string subject;
  std::string target;
  std::string action;
  std::string predicate;
  std::uint32_t offset = 0;
};

struct CFDLCallableParameterSyntax {
  std::string name;
  CFDLFlowTypeSyntax type;
  std::uint32_t offset = 0;
};

struct CFDLForeignFieldSyntax {
  std::string name;
  std::string physical_type;
  std::uint32_t offset = 0;
};

enum class CFDLForeignCarrierKind : std::uint8_t {
  Incomplete,
  Scalar,
  Record,
  Enum,
  Union,
  Count,
};

struct CFDLEnumConstantSyntax {
  std::string name;
  std::int64_t value = 0;
  std::uint32_t offset = 0;

  friend bool operator==(const CFDLEnumConstantSyntax &,
                         const CFDLEnumConstantSyntax &) = default;
};

enum class CFDLForeignConstantKind : std::uint8_t {
  Integer,
  Bool,
  Count,
};

struct CFDLForeignConstantSyntax {
  std::string name;
  std::string physical_type;
  CFDLForeignConstantKind kind = CFDLForeignConstantKind::Count;
  std::uint64_t integer_payload = 0;
  bool integer_negative = false;
  bool bool_value = false;
  std::uint32_t offset = 0;
  std::uint32_t end_offset = 0;

  friend bool operator==(const CFDLForeignConstantSyntax &,
                         const CFDLForeignConstantSyntax &) = default;
};

enum class CFDLErrorPredicateKind : std::uint8_t {
  None,
  Equal,
  NotEqual,
  Less,
  Null,
  Invalid,
  InSet,
  NotInSet,
  Count,
};

struct CFDLErrorValueSyntax {
  std::string name;
  std::uint64_t magnitude = 0;
  bool negative = false;
  std::uint32_t offset = 0;

  friend bool operator==(const CFDLErrorValueSyntax &,
                         const CFDLErrorValueSyntax &) = default;
};

struct CFDLErrorRangeSyntax {
  CFDLErrorValueSyntax lower;
  std::optional<CFDLErrorValueSyntax> upper;

  friend bool operator==(const CFDLErrorRangeSyntax &,
                         const CFDLErrorRangeSyntax &) = default;
};

enum class CFDLErrorDomainKind : std::uint8_t {
  None,
  Code,
  Errno,
  Win32,
  Count,
};

struct CFDLErrorContractSyntax {
  CFDLErrorDomainKind domain = CFDLErrorDomainKind::None;
  std::string observed = "result";
  CFDLErrorPredicateKind predicate = CFDLErrorPredicateKind::None;
  std::vector<CFDLErrorRangeSyntax> ranges;
  std::uint32_t offset = 0;

  friend bool operator==(const CFDLErrorContractSyntax &,
                         const CFDLErrorContractSyntax &) = default;
};

enum class CFDLOutcomeKind : std::uint8_t {
  None,
  PosixRead,
  Win32Read,
  Fread,
  Count,
};

struct CFDLOutcomeContractSyntax {
  CFDLOutcomeKind kind = CFDLOutcomeKind::None;
  std::string element_type;
  std::string buffer;
  std::string capacity;
  std::string element_size;
  std::string element_count;
  std::string stream;
  std::string count;
  std::string context;
  std::string eof_symbol;
  std::string ferror_symbol;
  std::uint32_t offset = 0;

  friend bool operator==(const CFDLOutcomeContractSyntax &,
                         const CFDLOutcomeContractSyntax &) = default;
};

enum class CFDLForeignInvalidKind : std::uint8_t {
  None,
  Null,
  Integer,
  Count,
};

struct CFDLForeignTypeSyntax {
  std::string name;
  CFDLForeignCarrierKind carrier_kind = CFDLForeignCarrierKind::Incomplete;
  std::string scalar_carrier;
  std::vector<CFDLForeignFieldSyntax> fields;
  std::vector<CFDLEnumConstantSyntax> enum_constants;
  CFDLForeignInvalidKind invalid_kind = CFDLForeignInvalidKind::None;
  std::int64_t invalid_integer = 0;
  std::uint32_t offset = 0;
  std::uint32_t end_offset = 0;
};

struct CFDLCallableSyntax {
  std::string name;
  std::vector<CFDLCallableParameterSyntax> parameters;
  CFDLFlowTypeSyntax result;
  std::vector<CFDLWhereFactSyntax> where_facts;
  std::optional<CFDLOutcomeContractSyntax> outcome_contract;
  std::optional<CFDLErrorContractSyntax> error_contract;
  std::string external_symbol;
  ForeignCallingConvention calling_convention = ForeignCallingConvention::C;
  bool explicit_calling_convention = false;
  std::uint32_t offset = 0;
  std::uint32_t end_offset = 0;
};

struct CFDLSyntaxFile {
  std::string module_name;
  std::uint32_t module_offset = 0;
  std::vector<std::string> imports;
  std::vector<std::uint32_t> import_offsets;
  std::vector<CFDLForeignTypeSyntax> foreign_types;
  std::vector<CFDLForeignConstantSyntax> foreign_constants;
  std::vector<CFDLCallableSyntax> callables;
};

struct CFDLPackageDiagnostic {
  std::uint32_t unit_index = 0;
  CFDLDiagnostic diagnostic;
};

[[nodiscard]] bool parseCFDL(const SourceBuffer &source, CFDLSyntaxFile &file,
                             std::vector<CFDLDiagnostic> &diagnostics);
[[nodiscard]] std::string printCFDLTokens(const SourceBuffer &source);
[[nodiscard]] std::string printCFDLSyntax(const CFDLSyntaxFile &file);
[[nodiscard]] std::string
renderCFDLForeignType(const CFDLForeignTypeSyntax &type);
[[nodiscard]] std::string
renderCFDLForeignConstant(const CFDLForeignConstantSyntax &constant);
[[nodiscard]] std::string
renderCFDLCallable(const CFDLCallableSyntax &callable);
[[nodiscard]] std::string renderCFDLSource(const CFDLSyntaxFile &file);
[[nodiscard]] std::vector<std::string>
cfdlCompletionCandidates(const CFDLSyntaxFile &file);

// Validates names and finite resource-flow relations before Interop lowering.
// Leaving all flow qualifiers and contracts absent is valid: it denotes a Raw
// CFFI callable whose ABI is checked but which has no inferred ownership,
// borrowing, cleanup, error, or outcome projection.
[[nodiscard]] bool
validateCFDLCallable(const CFDLCallableSyntax &callable,
                     std::vector<CFDLDiagnostic> &diagnostics);

// Closes obligation/action protocols across all CFDL units in one package.
// Dependency packages enter through already-verified artifacts and are not
// re-opened by this pass.
[[nodiscard]] bool
validateCFDLPackageProtocol(std::span<const CFDLSyntaxFile *const> files,
                            std::vector<CFDLPackageDiagnostic> &diagnostics);

struct CFDLBindingEnvironment {
  std::function<CanonicalTypeId(std::string_view)> resolve_callable;
};

struct ElaboratedCFDLResourceFlow {
  std::string name;
  CanonicalTypeId callable_type;
  ResourceFlowDeclaration declaration;
  NormalizedResourceFlow normalized;
};

[[nodiscard]] bool
elaborateCFDLResourceFlow(const CFDLCallableSyntax &callable,
                          const CFDLBindingEnvironment &environment,
                          ElaboratedCFDLResourceFlow &flow,
                          std::vector<CFDLDiagnostic> &diagnostics);

[[nodiscard]] SemIR
buildCFDLSemIR(const CFDLSyntaxFile &file, core::Arena &arena,
               SharedValueStores &values, DiagnosticEmitter &diagnostics,
               const PublicInterfaceRegistry &public_interfaces,
               interop::ArtifactRegistry &interop_registry,
               CheckIRId check_ir_id, std::string_view canonical_package,
               IdentifierId module_name, std::string_view target_triple,
               std::span<const ImportIR> imports = {},
               LanguageVersion language_version = DefaultLanguageVersion);

} // namespace chtholly::compiler
