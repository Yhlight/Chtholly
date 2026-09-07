#include "chtholly/Compiler/CFDL.h"
#include "chtholly/Compiler/ForeignDeclaration.h"
#include "chtholly/Compiler/PublicInterface.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using chtholly::compiler::CanonicalTypeId;
using chtholly::compiler::CFDLCallableParameterSyntax;
using chtholly::compiler::CFDLCallableSyntax;
using chtholly::compiler::CFDLBindingEnvironment;
using chtholly::compiler::CFDLDiagnostic;
using chtholly::compiler::CFDLFlowQualifier;
using chtholly::compiler::NormalizedResourceFlow;
using chtholly::compiler::normalizeResourceFlow;
using chtholly::compiler::parseCFDL;
using chtholly::compiler::ResourceFlowAction;
using chtholly::compiler::ResourceFlowCategory;
using chtholly::compiler::ResourceFlowDeclaration;
using chtholly::compiler::ResourceFlowEndpoint;
using chtholly::compiler::ResourceFlowEndpointKind;
using chtholly::compiler::ResourceFlowFact;
using chtholly::compiler::ResourceFlowPlace;
using chtholly::compiler::ResourceFlowPlaceKind;
using chtholly::compiler::ResourceFlowPredicate;
using chtholly::compiler::ResourceFlowRelationKind;
using chtholly::compiler::SourceBuffer;
using chtholly::compiler::SourceInput;
using chtholly::compiler::ElaboratedCFDLResourceFlow;
using chtholly::compiler::validateCFDLCallable;
using chtholly::compiler::validateCFDLPackageProtocol;
using chtholly::compiler::interop::ArtifactRegistry;
using chtholly::compiler::interop::ForeignCapability;
using chtholly::compiler::interop::ForeignOperationArtifact;
using chtholly::compiler::interop::ForeignOperationKind;
using chtholly::compiler::interop::ForeignProtocolEdge;
using chtholly::compiler::interop::ForeignProtocolEdgeKind;
using chtholly::compiler::interop::ForeignProtocolIdentity;
using chtholly::compiler::interop::ForeignProtocolIdentityKind;

namespace {

[[noreturn]] void failCheck(const char *expression, int line) {
  std::fprintf(stderr, "CHECK failed at line %d: %s\n", line, expression);
  std::abort();
}

inline void checkCondition(bool condition, const char *expression, int line) {
  if (!condition)
    failCheck(expression, line);
}

#define CHECK(condition) checkCondition((condition), #condition, __LINE__)

ResourceFlowFact fact(ResourceFlowCategory category,
                      ResourceFlowRelationKind relation,
                      ResourceFlowPlace subject) {
  ResourceFlowFact result;
  result.category = category;
  result.relation = relation;
  result.subject = subject;
  return result;
}

void resource_facts_are_canonicalized() {
  ResourceFlowDeclaration declaration;
  declaration.callable_name = "session_start";
  declaration.callable_type = CanonicalTypeId(7);
  declaration.parameter_names = {"buffer", "source"};
  declaration.event_names = {"complete"};
  declaration.facts = {
      [&] {
        auto value =
            fact(ResourceFlowCategory::Value, ResourceFlowRelationKind::Derives,
                 ResourceFlowPlace{ResourceFlowPlaceKind::Result, 0});
        value.target_place =
            ResourceFlowPlace{ResourceFlowPlaceKind::Parameter, 1};
        return value;
      }(),
      [&] {
        auto value =
            fact(ResourceFlowCategory::Resource,
                 ResourceFlowRelationKind::LoanEscapes,
                 ResourceFlowPlace{ResourceFlowPlaceKind::Parameter, 0});
        value.endpoint =
            ResourceFlowEndpoint{ResourceFlowEndpointKind::Event, 0};
        return value;
      }(),
  };

  NormalizedResourceFlow normalized;
  std::string error;
  CHECK(normalizeResourceFlow(declaration, normalized, error));
  CHECK(error.empty());
  CHECK(normalized.plan.category == ResourceFlowCategory::Resource);
  CHECK(normalized.facts[0].subject.kind == ResourceFlowPlaceKind::Result);
  CHECK(normalized.facts[1].subject.kind == ResourceFlowPlaceKind::Parameter);
  CHECK(normalized.fingerprint.hasValue());
}

void duplicate_facts_are_rejected() {
  ResourceFlowDeclaration declaration;
  declaration.callable_name = "release";
  declaration.callable_type = CanonicalTypeId(8);
  declaration.parameter_names = {"value"};
  declaration.action_names = {"close"};
  declaration.facts = {
      [&] {
        auto value =
            fact(ResourceFlowCategory::Resource,
                 ResourceFlowRelationKind::ObligationDischarges,
                 ResourceFlowPlace{ResourceFlowPlaceKind::Parameter, 0});
        value.action = ResourceFlowAction{0};
        return value;
      }(),
      [&] {
        auto value =
            fact(ResourceFlowCategory::Resource,
                 ResourceFlowRelationKind::ObligationDischarges,
                 ResourceFlowPlace{ResourceFlowPlaceKind::Parameter, 0});
        value.action = ResourceFlowAction{0};
        return value;
      }(),
  };

  NormalizedResourceFlow normalized;
  std::string error;
  CHECK(!normalizeResourceFlow(declaration, normalized, error));
  CHECK(error.find("duplicate") != std::string::npos);
}

void projection_is_more_specific_than_value() {
  ResourceFlowDeclaration declaration;
  declaration.callable_name = "slice";
  declaration.callable_type = CanonicalTypeId(9);
  declaration.parameter_names = {"source"};
  declaration.action_names = {"source"};
  declaration.facts = {
      [&] {
        auto value =
            fact(ResourceFlowCategory::Value,
                 ResourceFlowRelationKind::Requires, ResourceFlowPlace{});
        value.subject_action = ResourceFlowAction{0};
        value.predicate = ResourceFlowPredicate::Valid;
        return value;
      }(),
      [&] {
        auto value = fact(ResourceFlowCategory::Projection,
                          ResourceFlowRelationKind::Derives,
                          ResourceFlowPlace{ResourceFlowPlaceKind::Result, 0});
        value.target_place =
            ResourceFlowPlace{ResourceFlowPlaceKind::Parameter, 0};
        return value;
      }(),
  };

  NormalizedResourceFlow normalized;
  std::string error;
  CHECK(normalizeResourceFlow(declaration, normalized, error));
  CHECK(normalized.plan.category == ResourceFlowCategory::Projection);
}

void interop_registry_uses_stable_reference_identity() {
  ArtifactRegistry registry;
  ForeignOperationArtifact artifact;
  artifact.kind = ForeignOperationKind::Resource;
  artifact.capabilities.push_back(
      ForeignCapability{.path = "obligation.discharges",
                        .literals = {"action=0:close", "subject=parameter:0"}});
  ForeignProtocolIdentity action;
  action.kind = ForeignProtocolIdentityKind::Action;
  action.canonical_package = "pkg";
  action.canonical_module = "ffi";
  action.canonical_resource = "Session";
  action.canonical_name = "close";
  action.fingerprint = chtholly::compiler::StableFingerprint::fromCanonicalBytes(
      "action-pkg-ffi-session-close");
  artifact.protocol_identities.push_back(action);
  artifact.protocol_edges.push_back(
      ForeignProtocolEdge{.kind = ForeignProtocolEdgeKind::Discharges,
                          .source = action.fingerprint,
                          .target = action.fingerprint,
                          .owner_callable = "release"});
  artifact.fingerprint =
      chtholly::compiler::StableFingerprint::fromCanonicalBytes("resource-v1");
  std::string error;
  const auto reference =
      registry.publish("pkg", "ffi", "release", artifact, error);
  CHECK(reference.verify(error));
  CHECK(registry.resolve(reference) != nullptr);
  CHECK(registry.size() == 1);
  CHECK(registry.verify(error));
  const auto *resolved_action =
      registry.findProtocolIdentity(ForeignProtocolIdentityKind::Action, "pkg",
                                    "ffi", "Session", "close", {}, error);
  CHECK(resolved_action != nullptr);
  CHECK(resolved_action->fingerprint == action.fingerprint);
  CHECK(registry.findProtocolIdentity(ForeignProtocolIdentityKind::Action,
                                      "other", "ffi", "Session", "close", {},
                                      error) == nullptr);

  ArtifactRegistry unresolved_registry;
  ForeignOperationArtifact unresolved;
  unresolved.kind = ForeignOperationKind::Resource;
  unresolved.capabilities.push_back(
      ForeignCapability{.path = "obligation.discharges"});
  unresolved.protocol_edges.push_back(
      ForeignProtocolEdge{.kind = ForeignProtocolEdgeKind::Discharges,
                          .source = action.fingerprint,
                          .target = action.fingerprint,
                          .owner_callable = "release"});
  unresolved.fingerprint =
      chtholly::compiler::StableFingerprint::fromCanonicalBytes(
          "external-action-reference");
  const auto unresolved_reference = unresolved_registry.publish(
      "consumer", "ffi", "release", unresolved, error);
  CHECK(unresolved_reference.verify(error));
  CHECK(!unresolved_registry.verify(error));

  auto legacy = artifact;
  legacy.cfdl_semantic_epoch = 3;
  CHECK(!legacy.verify(error));

  chtholly::compiler::interop::ArtifactBundle bundle;
  bundle.records.push_back({reference, artifact});
  const auto encoded = bundle.encode(error);
  CHECK(!encoded.empty());
  const auto decoded =
      chtholly::compiler::interop::ArtifactBundle::decode(encoded, error);
  CHECK(decoded.has_value());
  chtholly::compiler::interop::ArtifactRegistry restored;
  CHECK(restored.registerBundle(*decoded, error));
  CHECK(restored.resolve(reference) != nullptr);
  CHECK(restored.resolve(reference)->capabilities.front().literals ==
        artifact.capabilities.front().literals);
  CHECK(restored.resolve(reference)->protocol_identities ==
        artifact.protocol_identities);
  CHECK(restored.resolve(reference)->protocol_edges == artifact.protocol_edges);
  CHECK(restored.size() == 1);

  ArtifactRegistry orphan_registry;
  auto orphan = artifact;
  ForeignProtocolIdentity event;
  event.kind = ForeignProtocolIdentityKind::Event;
  event.canonical_package = "pkg";
  event.canonical_module = "ffi";
  event.canonical_resource = "Session";
  event.canonical_name = "complete";
  event.owner_callable = "wait";
  event.fingerprint = chtholly::compiler::StableFingerprint::fromCanonicalBytes(
      "event-pkg-ffi-session-wait-complete");
  orphan.protocol_identities.push_back(event);
  orphan.fingerprint = chtholly::compiler::StableFingerprint::fromCanonicalBytes(
      "resource-with-orphan-event");
  const auto orphan_reference =
      orphan_registry.publish("pkg", "ffi", "wait", orphan, error);
  CHECK(orphan_reference.verify(error));
  CHECK(!orphan_registry.verify(error));

  auto corrupted = encoded;
  corrupted[0] ^= 1;
  CHECK(!chtholly::compiler::interop::ArtifactBundle::decode(corrupted, error));
}

void cfdl_source_and_flow_validation_are_closed() {
  SourceBuffer source(SourceInput("binding.cfdl",
                                  R"(module demo::ffi;
foreign type Session: void* invalid null;
foreign fn open(url: view const char*) -> owned Session
where result obliges close;
foreign fn wait(session: ref_mut Session) -> i32
where session discharges close;
)"));
  chtholly::compiler::CFDLSyntaxFile file;
  std::vector<CFDLDiagnostic> diagnostics;
  CHECK(parseCFDL(source, file, diagnostics));
  CHECK(diagnostics.empty());
  CHECK(file.callables.size() == 2);
  CHECK(file.callables.front().result.qualifier == CFDLFlowQualifier::Owned);

  CFDLCallableSyntax valid = file.callables.front();
  CHECK(validateCFDLCallable(valid, diagnostics));
  CHECK(diagnostics.empty());

  valid.where_facts.front().subject = "missing";
  CHECK(!validateCFDLCallable(valid, diagnostics));
  CHECK(!diagnostics.empty());

  CFDLCallableSyntax duplicate = file.callables.front();
  duplicate.where_facts.clear();
  duplicate.parameters.push_back(CFDLCallableParameterSyntax{"url", {}, 0});
  diagnostics.clear();
  CHECK(!validateCFDLCallable(duplicate, diagnostics));
}

void foreign_type_carriers_are_explicit() {
  SourceBuffer source(SourceInput("carriers.cfdl",
                                  R"(module carriers;
foreign type FILE;
foreign type Handle: void* invalid null;
foreign struct Pair {
  left: c_long;
  right: array<c_uint,4>;
};
)"));
  chtholly::compiler::CFDLSyntaxFile file;
  std::vector<CFDLDiagnostic> diagnostics;
  CHECK(parseCFDL(source, file, diagnostics));
  CHECK(diagnostics.empty());
  CHECK(file.foreign_types.size() == 3);
  CHECK(file.foreign_types[0].carrier_kind ==
        chtholly::compiler::CFDLForeignCarrierKind::Incomplete);
  CHECK(file.foreign_types[1].carrier_kind ==
        chtholly::compiler::CFDLForeignCarrierKind::Scalar);
  CHECK(file.foreign_types[1].scalar_carrier == "void*");
  CHECK(file.foreign_types[1].invalid_kind ==
        chtholly::compiler::CFDLForeignInvalidKind::Null);
  CHECK(file.foreign_types[2].carrier_kind ==
        chtholly::compiler::CFDLForeignCarrierKind::Record);
  CHECK(file.foreign_types[2].fields.size() == 2);
  CHECK(file.foreign_types[2].fields[1].physical_type == "array<c_uint,4>");
  const auto printed = printCFDLSyntax(file);
  CHECK(printed.find("ForeignType FILE") != std::string::npos);
  CHECK(printed.find("carrier=void*") != std::string::npos);
  CHECK(printed.find("fields=2") != std::string::npos);

  const std::vector<std::string> invalid = {
      "module bad; foreign type H: i32 where valid null;",
      "module bad; foreign type H: void* invalid -;",
      "module bad; foreign struct R { x: i32 };",
  };
  for (const auto &text : invalid) {
    SourceBuffer bad(SourceInput("bad.cfdl", text));
    chtholly::compiler::CFDLSyntaxFile bad_file;
    diagnostics.clear();
    CHECK(!parseCFDL(bad, bad_file, diagnostics));
    CHECK(!diagnostics.empty());
  }
}

void enum_union_and_callable_abi_facts_parse() {
  SourceBuffer source(SourceInput("abi-facts.cfdl",
                                  R"(module abi::facts;
foreign enum Status: c_int {
  STATUS_OK = 0;
  STATUS_FAILED = -1;
};
foreign union Number {
  integer: c_long;
  real: f64;
};
foreign fn query(callback: c_fn win64(c_int)->void, value: out Number) -> Status
link "library_query" call win64;
)"));
  chtholly::compiler::CFDLSyntaxFile file;
  std::vector<CFDLDiagnostic> diagnostics;
  CHECK(parseCFDL(source, file, diagnostics));
  CHECK(diagnostics.empty());
  CHECK(file.foreign_types.size() == 2);
  CHECK(file.foreign_types[0].carrier_kind ==
        chtholly::compiler::CFDLForeignCarrierKind::Enum);
  CHECK(file.foreign_types[0].enum_constants.size() == 2);
  CHECK(file.foreign_types[1].carrier_kind ==
        chtholly::compiler::CFDLForeignCarrierKind::Union);
  CHECK(file.callables.front().external_symbol == "library_query");
  CHECK(file.callables.front().calling_convention ==
        chtholly::compiler::ForeignCallingConvention::Win64);
  CHECK(file.callables.front().parameters.front().type.physical_type ==
        "c_fn win64(c_int)->void");
  const auto printed = printCFDLSyntax(file);
  CHECK(printed.find("enum_carrier=c_int constants=2") != std::string::npos);
  CHECK(printed.find("union_members=2") != std::string::npos);
  CHECK(printed.find("link=library_query") != std::string::npos);

  for (const auto &text : {
           "module bad; foreign union Empty {};",
           "module bad; foreign enum E: c_int { A = 2147483648; };",
           "module bad; foreign fn f() -> i32 link \"\";",
           "module bad; foreign fn f() -> i32 call vectorcall;",
       }) {
    SourceBuffer bad(SourceInput("bad.cfdl", text));
    chtholly::compiler::CFDLSyntaxFile bad_file;
    diagnostics.clear();
    (void)parseCFDL(bad, bad_file, diagnostics);
    CHECK(!diagnostics.empty() || !bad_file.foreign_types.empty());
  }
}

void constants_and_errno_contracts_are_typed() {
  SourceBuffer source(SourceInput(
      "errno.cfdl",
      "module errno_api; "
      "foreign const EINVAL: c_int = 22; "
      "foreign const ENABLED: c_bool = true; "
      "foreign type Handle: void* invalid -1; "
      "foreign fn probe(fail: c_int) -> c_int link \"probe\" call c "
      "error errno when result == -1; "
      "foreign fn status(code: c_int) -> c_int "
      "error code when result != 0; "
      "foreign fn pointer() -> void* "
      "error errno when result == null; "
      "foreign fn win32() -> c_int "
      "error win32 when result == 0; "
      "foreign fn set_status() -> c_int "
      "error code when result in { -4 through -2, EINVAL, 7 }; "
      "foreign fn allowed_status() -> c_uint "
      "error code when result not in { 0, 4 through 6 }; "
      "foreign fn handle() -> Handle "
      "error win32 when result == invalid; "
      "foreign fn read(buffer: view_mut void*, capacity: c_size) -> c_ptrdiff "
      "error errno when result == -1 "
      "outcome posix_read<u8>(buffer, capacity) link \"read\" call c;"));
  chtholly::compiler::CFDLSyntaxFile file;
  std::vector<CFDLDiagnostic> diagnostics;
  CHECK(parseCFDL(source, file, diagnostics));
  CHECK(file.foreign_constants.size() == 2);
  CHECK(file.foreign_constants[0].integer_payload == 22);
  CHECK(file.foreign_constants[1].bool_value);
  CHECK(file.callables.front().error_contract.has_value());
  CHECK(file.callables.front().error_contract->predicate ==
        chtholly::compiler::CFDLErrorPredicateKind::Equal);
  CHECK(file.callables[1].error_contract->domain ==
        chtholly::compiler::CFDLErrorDomainKind::Code);
  CHECK(file.callables[1].error_contract->predicate ==
        chtholly::compiler::CFDLErrorPredicateKind::NotEqual);
  CHECK(file.callables[2].error_contract->predicate ==
        chtholly::compiler::CFDLErrorPredicateKind::Null);
  CHECK(file.callables[3].error_contract->domain ==
        chtholly::compiler::CFDLErrorDomainKind::Win32);
  CHECK(file.callables[4].error_contract->predicate ==
        chtholly::compiler::CFDLErrorPredicateKind::InSet);
  CHECK(file.callables[4].error_contract->ranges.size() == 3);
  CHECK(file.callables[4].error_contract->ranges.front().upper.has_value());
  CHECK(file.callables[5].error_contract->predicate ==
        chtholly::compiler::CFDLErrorPredicateKind::NotInSet);
  CHECK(file.callables[6].error_contract->predicate ==
        chtholly::compiler::CFDLErrorPredicateKind::Invalid);
  CHECK(file.callables[7].outcome_contract.has_value());
  CHECK(file.callables[7].outcome_contract->kind ==
        chtholly::compiler::CFDLOutcomeKind::PosixRead);
  CHECK(file.callables[7].outcome_contract->buffer == "buffer");
  CHECK(validateCFDLCallable(file.callables.front(), diagnostics));
  CHECK(renderCFDLSource(file).find("error errno when result == -1") !=
        std::string::npos);
  CHECK(renderCFDLSource(file).find("error code when result != 0") !=
        std::string::npos);
  CHECK(renderCFDLSource(file).find("error errno when result == null") !=
        std::string::npos);
  CHECK(renderCFDLSource(file).find(
            "error code when result in { -4 through -2, EINVAL, 7 }") !=
        std::string::npos);
  CHECK(renderCFDLSource(file).find("error win32 when result == invalid") !=
        std::string::npos);
  CHECK(renderCFDLSource(file).find(
            "link \"read\" call c outcome posix_read<u8>(buffer, capacity) "
            "error errno when result == -1") != std::string::npos);

  for (const auto &text : {
           "module bad; foreign const VALUE: f64 = 1;",
           "module bad; foreign fn f() -> c_int error errno when result "
           "> 0;",
           "module bad; foreign fn f() -> c_int error errno when result "
           "== -1, error errno when result < 0;",
           "module bad; foreign fn f() -> void* error errno when result "
           "!= null;",
           "module bad; foreign fn f() -> c_int error code when result "
           "in {};",
           "module bad; foreign fn f() -> c_int error code when result "
           "not { 0 };",
           "module bad; foreign fn f() -> c_int error code when result "
           "== invalid through 2;",
       }) {
    SourceBuffer bad(SourceInput("bad.cfdl", text));
    chtholly::compiler::CFDLSyntaxFile bad_file;
    diagnostics.clear();
    if (parseCFDL(bad, bad_file, diagnostics) && !bad_file.callables.empty())
      (void)validateCFDLCallable(bad_file.callables.front(), diagnostics);
    CHECK(!diagnostics.empty() || !bad_file.foreign_constants.empty());
  }

  auto out_contract = file.callables.front();
  out_contract.parameters.front().type.qualifier = CFDLFlowQualifier::Out;
  diagnostics.clear();
  CHECK(!validateCFDLCallable(out_contract, diagnostics));
}

void win32_read_outcome_is_structurally_closed() {
  SourceBuffer source(SourceInput(
      "read-file.cfdl",
      "module kernel; foreign fn ReadFile(handle: void*, buffer: view_mut "
      "void*, capacity: c_uint, count: out c_uint, overlapped: void*) -> "
      "c_int link \"ReadFile\" call win64 outcome win32_read<u8>(buffer, "
      "capacity, count, overlapped) error win32 when result == 0;"));
  chtholly::compiler::CFDLSyntaxFile file;
  std::vector<CFDLDiagnostic> diagnostics;
  CHECK(parseCFDL(source, file, diagnostics));
  CHECK(file.callables.size() == 1);
  const auto &callable = file.callables.front();
  CHECK(callable.outcome_contract.has_value());
  CHECK(callable.outcome_contract->kind ==
        chtholly::compiler::CFDLOutcomeKind::Win32Read);
  CHECK(callable.outcome_contract->buffer == "buffer");
  CHECK(callable.outcome_contract->capacity == "capacity");
  CHECK(callable.outcome_contract->count == "count");
  CHECK(callable.outcome_contract->context == "overlapped");
  CHECK(validateCFDLCallable(callable, diagnostics));
  CHECK(renderCFDLSource(file).find(
            "outcome win32_read<u8>(buffer, capacity, count, overlapped) "
            "error win32 when result == 0") != std::string::npos);

  using Artifact = chtholly::compiler::interop::ForeignOperationArtifact;
  Artifact artifact;
  artifact.kind = chtholly::compiler::interop::ForeignOperationKind::Value;
  artifact.capabilities.push_back({.path = "value.result"});
  artifact.error_extractor = Artifact::ErrorExtractor::Win32LastError;
  artifact.error_predicate = Artifact::ErrorPredicate::IntegerSet;
  artifact.error_success_payload = Artifact::ErrorSuccessPayload::Raw;
  artifact.error_intervals.push_back({0, 0});
  artifact.error_predicate_width = 32;
  artifact.error_predicate_signed = true;
  artifact.outcome_projection = Artifact::OutcomeProjection::Win32Read;
  artifact.outcome_buffer_lane = 1;
  artifact.outcome_capacity_lane = 2;
  artifact.outcome_count_lane = 3;
  artifact.outcome_context_lane = 4;
  artifact.outcome_element_type = chtholly::compiler::PublicType::integer(8, false);
  artifact.outcome_count_type = chtholly::compiler::PublicType::integer(32, false);
  artifact.argument_sources = {
      {Artifact::ArgumentSourceKind::PublicArgument, 0},
      {Artifact::ArgumentSourceKind::PublicArgument, 1},
      {Artifact::ArgumentSourceKind::PublicArgument, 2},
      {Artifact::ArgumentSourceKind::OutcomeStorage,
       chtholly::core::AnyId::InvalidIndex},
      {Artifact::ArgumentSourceKind::NullPointer,
       chtholly::core::AnyId::InvalidIndex},
  };
  artifact.fingerprint =
      chtholly::compiler::StableFingerprint::fromCanonicalBytes("win32-read");
  std::string error;
  CHECK(artifact.verify(error));

  auto tampered = artifact;
  tampered.argument_sources[3] = {Artifact::ArgumentSourceKind::PublicArgument,
                                  3};
  CHECK(!tampered.verify(error));
  tampered = artifact;
  tampered.outcome_count_type = chtholly::compiler::PublicType::integer(32, true);
  CHECK(!tampered.verify(error));
  tampered = artifact;
  tampered.error_intervals = {{1, 1}};
  CHECK(!tampered.verify(error));
}

void recv_outcome_keeps_ordinary_flags_lane() {
  SourceBuffer source(SourceInput(
      "recv.cfdl",
      "module socket; foreign fn recv(sockfd: c_int, buffer: view_mut void*, "
      "capacity: c_size, flags: c_int) -> c_ptrdiff link \"recv\" call c "
      "outcome posix_read<u8>(buffer, capacity) error errno when result == "
      "-1;"));
  chtholly::compiler::CFDLSyntaxFile file;
  std::vector<CFDLDiagnostic> diagnostics;
  CHECK(parseCFDL(source, file, diagnostics));
  CHECK(file.callables.size() == 1);
  CHECK(file.callables.front().parameters.size() == 4);
  CHECK(file.callables.front().parameters.back().name == "flags");
  CHECK(validateCFDLCallable(file.callables.front(), diagnostics));
  const auto rendered = renderCFDLSource(file);
  CHECK(rendered.find("foreign fn recv(sockfd: c_int, buffer: view_mut void*, "
                      "capacity: c_size, flags: c_int) -> c_ptrdiff") !=
        std::string::npos);
  CHECK(rendered.find("outcome posix_read<u8>(buffer, capacity)") !=
        std::string::npos);
}

void fread_outcome_allows_count_capacity_alias() {
  SourceBuffer source(SourceInput(
      "fread.cfdl",
      "module io; foreign fn fread(arg0: view_mut void*, arg1: c_ulong, "
      "arg2: c_ulong, arg3: FILE*) -> c_ulong link \"fread\" call c "
      "outcome fread<u8>(arg0, arg1, arg2, arg3) eof \"feof\"(arg3) "
      "ferror \"ferror\"(arg3) error errno when ferror != 0;"));
  chtholly::compiler::CFDLSyntaxFile file;
  std::vector<CFDLDiagnostic> diagnostics;
  CHECK(parseCFDL(source, file, diagnostics));
  CHECK(validateCFDLCallable(file.callables.front(), diagnostics));

  using Artifact = chtholly::compiler::interop::ForeignOperationArtifact;
  Artifact artifact;
  artifact.kind = chtholly::compiler::interop::ForeignOperationKind::Memory;
  artifact.capabilities.push_back({.path = "loan.mutate.arg0"});
  artifact.error_extractor = Artifact::ErrorExtractor::Errno;
  artifact.error_predicate = Artifact::ErrorPredicate::IntegerSet;
  artifact.error_success_payload = Artifact::ErrorSuccessPayload::Raw;
  artifact.error_intervals.push_back({0, 0});
  artifact.error_predicate_width = 32;
  artifact.error_predicate_signed = true;
  artifact.error_predicate_inverted = true;
  artifact.outcome_projection = Artifact::OutcomeProjection::Fread;
  artifact.outcome_buffer_lane = 0;
  artifact.outcome_capacity_lane = 2;
  artifact.outcome_count_lane = 2;
  artifact.outcome_context_lane = 3;
  artifact.outcome_size_lane = 1;
  artifact.outcome_eof_symbol = "feof";
  artifact.outcome_ferror_symbol = "ferror";
  artifact.outcome_element_type = chtholly::compiler::PublicType::integer(8, false);
  artifact.argument_sources = {
      {Artifact::ArgumentSourceKind::PublicArgument, 0},
      {Artifact::ArgumentSourceKind::PublicArgument, 1},
      {Artifact::ArgumentSourceKind::PublicArgument, 2},
      {Artifact::ArgumentSourceKind::PublicArgument, 3},
  };
  artifact.fingerprint =
      chtholly::compiler::StableFingerprint::fromCanonicalBytes("fread");
  std::string error;
  CHECK(artifact.verify(error));
}

void status_code_allows_initialized_output() {
  SourceBuffer source(SourceInput(
      "bcrypt.cfdl",
      "module bcrypt; foreign fn get_property(buffer: view_mut void*, "
      "capacity: c_ulong, result: out c_ulong) -> c_long link "
      "\"BCryptGetProperty\" call win64 error code when result != 0;"));
  chtholly::compiler::CFDLSyntaxFile file;
  std::vector<CFDLDiagnostic> diagnostics;
  CHECK(parseCFDL(source, file, diagnostics));
  CHECK(validateCFDLCallable(file.callables.front(), diagnostics));
  CHECK(renderCFDLSource(file).find("result: out c_ulong") !=
        std::string::npos);
}

void foreign_error_contract_artifacts_are_closed() {
  using Artifact = chtholly::compiler::interop::ForeignOperationArtifact;
  Artifact artifact;
  artifact.kind = chtholly::compiler::interop::ForeignOperationKind::Value;
  artifact.capabilities.push_back({.path = "value.result"});
  artifact.error_extractor = Artifact::ErrorExtractor::ReturnedCode;
  artifact.error_predicate = Artifact::ErrorPredicate::IntegerSet;
  artifact.error_success_payload = Artifact::ErrorSuccessPayload::Void;
  artifact.error_intervals.push_back({0, 0});
  artifact.error_predicate_width = 32;
  artifact.error_predicate_signed = true;
  artifact.error_predicate_inverted = true;
  artifact.fingerprint =
      chtholly::compiler::StableFingerprint::fromCanonicalBytes("error-code");
  std::string error;
  CHECK(artifact.verify(error));

  auto invalid = artifact;
  invalid.error_success_payload = Artifact::ErrorSuccessPayload::Raw;
  CHECK(!invalid.verify(error));

  invalid = artifact;
  invalid.error_predicate = Artifact::ErrorPredicate::Null;
  invalid.error_intervals.clear();
  invalid.error_predicate_signed = false;
  invalid.error_predicate_inverted = false;
  CHECK(!invalid.verify(error));

  invalid = artifact;
  invalid.error_predicate_inverted = false;
  invalid.error_intervals = {{0, 3}, {2, 4}};
  CHECK(!invalid.verify(error));

  invalid = artifact;
  invalid.error_predicate_width = 65;
  invalid.canonicalize();
  CHECK(!invalid.verify(error));

  auto canonical = artifact;
  canonical.error_predicate_inverted = false;
  canonical.error_intervals = {{2, 3}, {0, 1}, {1, 2}};
  canonical.canonicalize();
  CHECK(canonical.error_intervals.size() == 1);
  CHECK((canonical.error_intervals.front() == Artifact::ErrorInterval{0, 3}));
  CHECK(canonical.verify(error));
}

void raw_cffi_callables_are_value_only() {
  SourceBuffer source(SourceInput(
      "raw.cfdl",
      "module raw; foreign fn version() -> c_int link \"library_version\" "
      "call c; foreign fn reset() -> void;"));
  chtholly::compiler::CFDLSyntaxFile file;
  std::vector<CFDLDiagnostic> diagnostics;
  CHECK(parseCFDL(source, file, diagnostics));
  CHECK(diagnostics.empty());
  CHECK(file.callables.size() == 2);
  for (const auto &callable : file.callables) {
    CHECK(callable.where_facts.empty());
    CHECK(!callable.error_contract.has_value());
    CHECK(!callable.outcome_contract.has_value());
    for (const auto &parameter : callable.parameters)
      CHECK(parameter.type.qualifier == CFDLFlowQualifier::Value);
    CHECK(callable.result.qualifier == CFDLFlowQualifier::Value);

    ElaboratedCFDLResourceFlow flow;
    CFDLBindingEnvironment environment;
    environment.resolve_callable = [](std::string_view) {
      return CanonicalTypeId(17);
    };
    diagnostics.clear();
    CHECK(elaborateCFDLResourceFlow(callable, environment, flow,
                                    diagnostics));
    CHECK(diagnostics.empty());
    CHECK(flow.normalized.plan.category == ResourceFlowCategory::Value);
    CHECK(flow.normalized.facts.size() == 1);
    CHECK(flow.normalized.facts.front().relation ==
          ResourceFlowRelationKind::ValueResult);
    CHECK(flow.declaration.action_names.empty());
    CHECK(flow.declaration.event_names.empty());
  }
}

void retired_cfdl_surface_is_rejected() {
  const std::vector<std::string> retired_sources = {
      "module demo; operation { close; }",
      "module demo; foreign fn open() -> owned Handle bind resource.open;",
      "module demo; unsafe extern \"C\" fn open();",
  };

  for (const auto &text : retired_sources) {
    SourceBuffer source(SourceInput("retired.cfdl", text));
    chtholly::compiler::CFDLSyntaxFile file;
    std::vector<CFDLDiagnostic> diagnostics;
    CHECK(!parseCFDL(source, file, diagnostics));
    CHECK(!diagnostics.empty());
  }

  const std::vector<std::string> retired_where_sources = {
      "module demo; foreign fn open() -> owned Handle where result invokes "
      "until result.complete;",
      "module demo; foreign fn open() -> owned Handle where result derives "
      "from result;",
  };
  for (const auto &text : retired_where_sources) {
    SourceBuffer source(SourceInput("retired-where.cfdl", text));
    chtholly::compiler::CFDLSyntaxFile file;
    std::vector<CFDLDiagnostic> diagnostics;
    CHECK(!parseCFDL(source, file, diagnostics));
    CHECK(!diagnostics.empty());
  }
}

void package_protocols_close_across_units() {
  SourceBuffer acquire_source(
      SourceInput("acquire.cfdl", "module acquire; foreign fn open() -> owned "
                                  "Session where result obliges close;"));
  SourceBuffer release_source(SourceInput(
      "release.cfdl", "module release; foreign fn close(session: ref_mut "
                      "Session) -> void where session discharges close;"));
  chtholly::compiler::CFDLSyntaxFile acquire;
  chtholly::compiler::CFDLSyntaxFile release;
  std::vector<CFDLDiagnostic> diagnostics;
  CHECK(parseCFDL(acquire_source, acquire, diagnostics));
  CHECK(parseCFDL(release_source, release, diagnostics));
  const std::vector<const chtholly::compiler::CFDLSyntaxFile *> files = {&acquire,
                                                                     &release};
  std::vector<chtholly::compiler::CFDLPackageDiagnostic> package_diagnostics;
  CHECK(validateCFDLPackageProtocol(files, package_diagnostics));

  SourceBuffer qualified_release_source(
      SourceInput("qualified-release.cfdl",
                  "module release; foreign fn close(session: ref_mut Session) "
                  "-> void where session discharges acquire::close;"));
  chtholly::compiler::CFDLSyntaxFile qualified_release;
  diagnostics.clear();
  CHECK(parseCFDL(qualified_release_source, qualified_release, diagnostics));
  const std::vector<const chtholly::compiler::CFDLSyntaxFile *> qualified_files = {
      &acquire, &qualified_release};
  CHECK(validateCFDLPackageProtocol(qualified_files, package_diagnostics));

  release.callables.front().where_facts.clear();
  CHECK(!validateCFDLPackageProtocol(files, package_diagnostics));
  CHECK(!package_diagnostics.empty());

  SourceBuffer event_source(
      SourceInput("event.cfdl", "module event; foreign fn emit() -> void where "
                                "result invokes complete;"));
  chtholly::compiler::CFDLSyntaxFile event_file;
  diagnostics.clear();
  CHECK(parseCFDL(event_source, event_file, diagnostics));
  CHECK(printCFDLSyntax(event_file).find("invokes complete") !=
        std::string::npos);
  event_file.callables.front().where_facts.push_back(
      event_file.callables.front().where_facts.front());
  const std::vector<const chtholly::compiler::CFDLSyntaxFile *> event_files = {
      &event_file};
  CHECK(!validateCFDLPackageProtocol(event_files, package_diagnostics));
}

void multi_event_completion_protocol_is_closed() {
  ForeignOperationArtifact artifact;
  artifact.kind = ForeignOperationKind::Resource;
  artifact.capabilities.push_back(
      ForeignCapability{.path = "async.completion"});
  const auto make_event = [](const char *name) {
    ForeignProtocolIdentity identity;
    identity.kind = ForeignProtocolIdentityKind::Event;
    identity.canonical_package = "pkg";
    identity.canonical_module = "host";
    identity.canonical_resource = "Task";
    identity.canonical_name = name;
    identity.owner_callable = "poll";
    identity.fingerprint =
        chtholly::compiler::StableFingerprint::fromCanonicalBytes(name);
    return identity;
  };
  const auto complete = make_event("complete");
  const auto cancelled = make_event("cancelled");
  const auto wake = make_event("wake");
  artifact.protocol_identities = {complete, cancelled, wake};
  for (const auto &event : artifact.protocol_identities)
    artifact.protocol_edges.push_back(
        ForeignProtocolEdge{.kind = ForeignProtocolEdgeKind::Invokes,
                            .source = event.fingerprint,
                            .target = event.fingerprint,
                            .owner_callable = "poll"});
  artifact.completion_events = {complete.fingerprint, cancelled.fingerprint,
                                wake.fingerprint};
  artifact.cancel_events = {cancelled.fingerprint};
  artifact.wake_events = {wake.fingerprint};
  artifact.requires_quiescence = true;
  artifact.discharges_quiescence = true;
  artifact.fingerprint =
      chtholly::compiler::StableFingerprint::fromCanonicalBytes("multi-event");
  artifact.canonicalize();
  std::string error;
  CHECK(artifact.verify(error));

  auto unreachable = artifact;
  unreachable.protocol_edges.erase(unreachable.protocol_edges.begin());
  CHECK(!unreachable.verify(error));
  CHECK(error.find("unreachable") != std::string::npos);
}

} // namespace

int main() {
  resource_facts_are_canonicalized();
  duplicate_facts_are_rejected();
  projection_is_more_specific_than_value();
  interop_registry_uses_stable_reference_identity();
  cfdl_source_and_flow_validation_are_closed();
  foreign_type_carriers_are_explicit();
  enum_union_and_callable_abi_facts_parse();
  constants_and_errno_contracts_are_typed();
  win32_read_outcome_is_structurally_closed();
  recv_outcome_keeps_ordinary_flags_lane();
  fread_outcome_allows_count_capacity_alias();
  status_code_allows_initialized_output();
  foreign_error_contract_artifacts_are_closed();
  raw_cffi_callables_are_value_only();
  retired_cfdl_surface_is_rejected();
  package_protocols_close_across_units();
  multi_event_completion_protocol_is_closed();
  return 0;
}
