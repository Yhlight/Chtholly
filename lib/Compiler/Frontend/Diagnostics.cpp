#include "chtholly/Compiler/Diagnostics.h"

#include "chtholly/Compiler/Source.h"

#include <algorithm>
#include <array>
#include <sstream>

namespace chtholly::compiler {
namespace {

constexpr auto Levels = std::to_array<DiagnosticLevel>({
#define CHTHOLLY_COMPILER_DIAGNOSTIC(Name, Level, Code) DiagnosticLevel::Level,
#include "chtholly/Compiler/DiagnosticKind.def"
});

constexpr auto Codes = std::to_array<std::string_view>({
#define CHTHOLLY_COMPILER_DIAGNOSTIC(Name, Level, Code) Code,
#include "chtholly/Compiler/DiagnosticKind.def"
});

std::string_view tokenDisplayName(TokenKind kind) {
  const auto spelling = tokenSpelling(kind);
  return spelling.empty() ? tokenKindName(kind) : spelling;
}

std::string_view levelName(DiagnosticLevel level) {
  switch (level) {
  case DiagnosticLevel::Note:
    return "note";
  case DiagnosticLevel::Warning:
    return "warning";
  case DiagnosticLevel::Error:
    return "error";
  }
  return "error";
}

} // namespace

void DiagnosticEmitter::emit(Diagnostic diagnostic) {
  diagnostics_.push_back(diagnostic);
}

void DiagnosticEmitter::emit(DiagnosticKind kind, std::uint32_t offset,
                             std::uint32_t length, TokenKind expected,
                             TokenKind actual) {
  emit({kind, offset, length, expected, actual});
}

bool DiagnosticEmitter::hasError() const {
  for (const auto &diagnostic : diagnostics_) {
    if (diagnosticLevel(diagnostic.kind) == DiagnosticLevel::Error)
      return true;
  }
  return false;
}

std::string DiagnosticEmitter::format(const SourceBuffer &source) const {
  std::ostringstream out;
  for (const auto &diagnostic : diagnostics_) {
    const auto location = source.lineColumn(diagnostic.offset);
    const auto line = source.lineText(location.line);
    const auto marker_length = std::max<std::size_t>(
        1, std::min<std::size_t>(diagnostic.length,
                                 location.column <= line.size()
                                     ? line.size() - location.column + 1
                                     : 1));
    out << source.filename() << ':' << location.line << ':' << location.column
        << ": " << levelName(diagnosticLevel(diagnostic.kind)) << ": "
        << diagnosticMessage(diagnostic) << " ["
        << diagnosticCode(diagnostic.kind) << "]\n";
    out << line << '\n';
    for (std::uint32_t column = 1; column < location.column; ++column)
      out << ' ';
    out << '^';
    for (std::size_t index = 1; index < marker_length; ++index)
      out << '~';
    out << '\n';
    for (const auto &note : diagnostic.notes) {
      out << (note.path.empty() ? source.filename() : note.path) << ':';
      if (note.path.empty()) {
        const auto note_location = source.lineColumn(note.offset);
        out << note_location.line << ':' << note_location.column;
      } else {
        out << "?:?";
      }
      out << ": note: "
          << (note.message.empty()
                  ? diagnosticMessage(
                        Diagnostic{note.kind, note.offset, note.length,
                                  TokenKind::Invalid, TokenKind::Invalid, {}})
                  : note.message)
          << " ["
          << (note.code.empty() ? diagnosticCode(note.kind) : note.code)
          << "]\n";
      if (note.path.empty()) {
        const auto note_location = source.lineColumn(note.offset);
        out << source.lineText(note_location.line) << '\n';
        for (std::uint32_t column = 1; column < note_location.column;
             ++column)
          out << ' ';
        out << '^\n';
      }
    }
  }
  return out.str();
}

DiagnosticLevel diagnosticLevel(DiagnosticKind kind) {
  const auto index = static_cast<std::size_t>(kind);
  return index < Levels.size() ? Levels[index] : DiagnosticLevel::Error;
}

std::string_view diagnosticCode(DiagnosticKind kind) {
  const auto index = static_cast<std::size_t>(kind);
  return index < Codes.size() ? Codes[index] : "chtholly.next.unknown";
}

std::string diagnosticMessage(const Diagnostic &diagnostic) {
  switch (diagnostic.kind) {
  case DiagnosticKind::UnexpectedCharacter:
    return "unexpected character";
  case DiagnosticKind::UnterminatedComment:
    return "unterminated block comment";
  case DiagnosticKind::UnterminatedLiteral:
    return "unterminated literal";
  case DiagnosticKind::InvalidUtf8:
    return "source contains invalid UTF-8";
  case DiagnosticKind::NonAsciiIdentifier:
    return "Chtholly v1 identifiers are restricted to ASCII";
  case DiagnosticKind::InvalidEscape:
    return "literal contains an unsupported escape sequence";
  case DiagnosticKind::InvalidCharLiteral:
    return "character literal must contain exactly one Unicode scalar value";
  case DiagnosticKind::InvalidNumericLiteral:
    return "invalid numeric literal";
  case DiagnosticKind::TokenTooLong:
    return "token exceeds the compact token length limit";
  case DiagnosticKind::SourceTooLarge:
    return "source exceeds the compact source offset limit";
  case DiagnosticKind::ExpectedToken:
    return "expected `" + std::string(tokenDisplayName(diagnostic.expected)) +
           "`, found `" + std::string(tokenDisplayName(diagnostic.actual)) +
           "`";
  case DiagnosticKind::ExpectedExpression:
    return "expected expression";
  case DiagnosticKind::ExpectedDeclaration:
    return "expected declaration";
  case DiagnosticKind::ParseTreeTooLarge:
    return "parse subtree exceeds the compact size limit";
  case DiagnosticKind::InvalidParseTree:
    return "parser produced an invalid tree";
  case DiagnosticKind::MissingModuleDeclaration:
    return "source must begin with exactly one module declaration";
  case DiagnosticKind::DuplicateModuleDeclaration:
    return "source contains more than one module declaration";
  case DiagnosticKind::MisplacedModuleDeclaration:
    return "module declaration must be the first declaration";
  case DiagnosticKind::DuplicateModule:
    return "duplicate module name in the compilation session";
  case DiagnosticKind::UnknownImport:
    return "imported module is not part of the compilation session";
  case DiagnosticKind::AmbiguousImport:
    return "imported module is exposed by more than one direct dependency";
  case DiagnosticKind::DuplicateImport:
    return "module is imported more than once";
  case DiagnosticKind::ImportCycle:
    return "module import cycle detected";
  case DiagnosticKind::NominalCompletionCycle:
    return "nominal definition completion cycle detected";
  case DiagnosticKind::NominalCompletionFailed:
    return "nominal definition completion failed";
  case DiagnosticKind::UnknownImportedFunction:
    return "imported module has no matching public function";
  case DiagnosticKind::DuplicateName:
    return "duplicate name in the same scope";
  case DiagnosticKind::AmbiguousCall:
    return "call is ambiguous between equally ranked overloads";
  case DiagnosticKind::InvalidNamedArgument:
    return "named argument is unknown, duplicated, or followed by a positional "
           "argument";
  case DiagnosticKind::InvalidDefaultArgument:
    return "default argument must be a trailing compile-time constant";
  case DiagnosticKind::UnknownName:
    return "unknown name";
  case DiagnosticKind::UnknownType:
    return "unknown type";
  case DiagnosticKind::TypeMismatch:
    return "incompatible semantic types";
  case DiagnosticKind::InvalidMoveOperand:
    return "move requires an initialized addressable place";
  case DiagnosticKind::InvalidCopyOperand:
    return "copy requires an initialized addressable place";
  case DiagnosticKind::InvalidAssignmentTarget:
    return "assignment requires an addressable destination place";
  case DiagnosticKind::AssignmentToImmutablePlace:
    return "assignment cannot modify an immutable place";
  case DiagnosticKind::CopyUnavailable:
    return "type does not provide the required copy operation";
  case DiagnosticKind::MoveUnavailable:
    return "type does not provide the required move operation";
  case DiagnosticKind::InvalidPointerDereference:
    return "operand is not a pointer or checked reference that can be "
           "dereferenced";
  case DiagnosticKind::UnsafePointerDereferenceRequired:
    return "raw pointer dereference requires an unsafe block";
  case DiagnosticKind::RawPointerCapabilityUnavailable:
    return "raw pointer pointee does not provide this checked place capability";
  case DiagnosticKind::InvalidLifecycleAttribute:
    return "lifecycle attribute has an invalid or duplicated policy";
  case DiagnosticKind::InvalidRepresentationAttribute:
    return "representation attribute is invalid for this nominal type";
  case DiagnosticKind::InvalidLifecycleImpl:
    return "lifecycle implementation does not match its canonical role";
  case DiagnosticKind::InvalidRepresentationImpl:
    return "representation implementation does not match its declared policy";
  case DiagnosticKind::MissingRepresentationImpl:
    return "representation policy requires a complete canonical implementation";
  case DiagnosticKind::RepresentationInitReadBeforeWrite:
    return "representation initializer reads a field before initializing it";
  case DiagnosticKind::RepresentationReferenceReinitialization:
    return "representation initializer cannot reinitialize a reference field";
  case DiagnosticKind::IncompleteRepresentationInit:
    return "representation initializer does not initialize every required "
           "field";
  case DiagnosticKind::RecursiveRepresentationCarrier:
    return "representation carrier recursively contains the represented type";
  case DiagnosticKind::InvalidObjectRepresentationProjection:
    return "object representation projection is invalid for this field";
  case DiagnosticKind::InvalidCanonicalImpl:
    return "canonical implementation is duplicated or has an invalid role";
  case DiagnosticKind::InvalidConstructor:
    return "constructor must be a canonical init function returning Self or "
           "Result<Self, E>";
  case DiagnosticKind::MissingLifecycleImpl:
    return "lifecycle policy requires a complete canonical implementation";
  case DiagnosticKind::InvalidLifecycleComposition:
    return "field lifecycle policies cannot form the requested aggregate "
           "lifecycle";
  case DiagnosticKind::UseAfterMove:
    return "place is used after its value was moved";
  case DiagnosticKind::UseOfMaybeMovedPlace:
    return "place may have been moved on an incoming control-flow path";
  case DiagnosticKind::UseOfPartiallyMovedPlace:
    return "complete value is used after one of its fields was moved";
  case DiagnosticKind::NestedDefer:
    return "a defer body cannot register another defer";
  case DiagnosticKind::DeferControlFlowEscape:
    return "control flow cannot escape from a defer body";
  case DiagnosticKind::DeferMustFallThrough:
    return "a defer body must reach its end normally";
  case DiagnosticKind::UnknownInterface:
    return "unknown interface in this scope";
  case DiagnosticKind::InvalidInterface:
    return "interface declaration or implementation is malformed";
  case DiagnosticKind::MissingInterfaceRequirement:
    return "interface implementation omits a required member";
  case DiagnosticKind::InterfaceSignatureMismatch:
    return "implementation member does not match the interface requirement";
  case DiagnosticKind::DuplicateConformance:
    return "type has more than one matching implementation of this interface";
  case DiagnosticKind::OrphanConformance:
    return "interface implementation must own the interface or the subject "
           "type";
  case DiagnosticKind::UnsatisfiedConstraint:
    return "generic constraint has no matching complete interface witness";
  case DiagnosticKind::AmbiguousAssociatedAlias:
    return "associated alias is ambiguous between visible constraints";
  case DiagnosticKind::ConstraintCycle:
    return "generic constraints form a recursive identity cycle";
  case DiagnosticKind::RecursiveTypeAlias:
    return "type aliases form a recursive expansion cycle";
  case DiagnosticKind::CannotInferGenericArguments:
    return "generic arguments cannot be inferred from this call";
  case DiagnosticKind::ConflictingGenericInference:
    return "generic inference produced conflicting types for one parameter";
  case DiagnosticKind::GenericInstantiationLimit:
    return "generic specialization exceeded the deterministic instantiation "
           "limit";
  case DiagnosticKind::GenericEntryPoint:
    return "program entry point cannot be generic";
  case DiagnosticKind::InvalidClosureCapture:
    return "closure capture is duplicated, unknown, or has an invalid transfer";
  case DiagnosticKind::ClosureReferenceCapture:
    return "closure cannot capture a checked reference beyond its valid "
           "lifetime";
  case DiagnosticKind::ClosureCaptureRequired:
    return "closure body uses a local value that is absent from its capture "
           "list";
  case DiagnosticKind::MissingOperatorImport:
    return "overloaded operators require an explicit `import std::ops`";
  case DiagnosticKind::MissingOperatorImplementation:
    return "left operand has no matching standard operator implementation";
  case DiagnosticKind::InvalidObjectType:
    return "type has no object representation in this position";
  case DiagnosticKind::InvalidMember:
    return "member is not available on this type";
  case DiagnosticKind::InvalidInherentMethod:
    return "inherent member must be a defined non-generic function; self, if "
           "present, must be first with type Self, const Self&, or Self&";
  case DiagnosticKind::ImmutableMethodReceiver:
    return "immutable receiver cannot call a mutable method";
  case DiagnosticKind::ExplicitMethodReceiverMoveRequired:
    return "consuming method requires an explicit move of this receiver";
  case DiagnosticKind::InvalidMethodReceiver:
    return "receiver cannot be adjusted to this method's self parameter";
  case DiagnosticKind::PrivateMemberAccess:
    return "member is private to the module that defines this type";
  case DiagnosticKind::PrivateAggregateInitialization:
    return "type cannot be initialized outside its module because it has "
           "private fields";
  case DiagnosticKind::InvalidIndex:
    return "value cannot be indexed with this expression";
  case DiagnosticKind::InvalidSliceSource:
    return "slice construction requires an addressable array or slice";
  case DiagnosticKind::NotCallable:
    return "expression is not a directly callable function";
  case DiagnosticKind::ArgumentCountMismatch:
    return "call argument count does not match the function signature";
  case DiagnosticKind::InvalidVariadicDeclaration:
    return "variadic marker requires unsafe extern \"C\" and a fixed parameter";
  case DiagnosticKind::InvalidVariadicArgument:
    return "type is not supported as a C variadic argument";
  case DiagnosticKind::InvalidCallbackAdapter:
    return "callback adapter requires a matching concrete ordinary function";
  case DiagnosticKind::InvalidCast:
    return "source value cannot be explicitly converted to the target type";
  case DiagnosticKind::IntegerOverflow:
    return "integer arithmetic overflows its result type";
  case DiagnosticKind::DivisionByZero:
    return "integer division by zero";
  case DiagnosticKind::RemainderByZero:
    return "integer remainder by zero";
  case DiagnosticKind::ShiftOutOfRange:
    return "integer shift count is outside the left operand width";
  case DiagnosticKind::MissingOrderingImport:
    return "three-way comparison requires an explicit import of std::compare";
  case DiagnosticKind::MissingCheckedCastImports:
    return "checked cast requires explicit imports of std::result and "
           "std::convert";
  case DiagnosticKind::InvalidCallbackRegistration:
    return "callback registration has an invalid physical ABI or release "
           "authority contract";
  case DiagnosticKind::InvalidCallbackRegistrationBinding:
    return "callback registration binding is missing, duplicated, unknown, or "
           "has the wrong type";
  case DiagnosticKind::InvalidForeignFunctionReference:
    return "foreign function reference requires a concrete non-generic unsafe "
           "extern declaration";
  case DiagnosticKind::UnsupportedAbiType:
    return "type is not supported by the next function ABI";
  case DiagnosticKind::ProjectionCapabilityUnavailable:
    return "object field projection does not provide the required capability";
  case DiagnosticKind::InvalidCarrierView:
    return "core::carrier requires the direct owner parameter of a canonical "
           "object projector or lifecycle function";
  case DiagnosticKind::CarrierViewEscape:
    return "carrier view escapes its canonical representation function";
  case DiagnosticKind::CarrierViewRegionViolation:
    return "carrier access is outside the computed projector region";
  case DiagnosticKind::BorrowConflict:
    return "operation conflicts with a live borrow of an overlapping region";
  case DiagnosticKind::InactiveUnionMember:
    return "union member is not definitely active on this control-flow path";
  case DiagnosticKind::UnknownActiveUnionMember:
    return "union active member is unknown; access requires an unsafe block";
  case DiagnosticKind::BorrowReturnEscape:
    return "borrowed return does not derive from a callable parameter region";
  case DiagnosticKind::TemporaryReferenceEscape:
    return "reference to a full-expression temporary escapes its permitted "
           "lifetime";
  case DiagnosticKind::OwnershipSummaryNonConvergent:
    return "interprocedural ownership summaries did not converge";
  case DiagnosticKind::OwnershipSummaryArtifactMismatch:
    return "cached ownership summary disagrees with its callable body";
  case DiagnosticKind::InvalidCallableContract:
    return "callable effect contract is incomplete or invalid";
  case DiagnosticKind::CallableContractMismatch:
    return "callable body effects exceed its declared effect contract";
  case DiagnosticKind::OwnershipBorrowOrigin:
    return "borrow originates here";
  case DiagnosticKind::OwnershipConflictOrigin:
    return "conflicting access originates here";
  case DiagnosticKind::OwnershipMoveOrigin:
    return "move originates here";
  case DiagnosticKind::OwnershipCallEffect:
    return "call effect contributes this ownership requirement";
  case DiagnosticKind::OwnershipReturnProvenance:
    return "returned provenance originates here";
  case DiagnosticKind::OwnershipCleanupBoundary:
    return "cleanup boundary keeps this value live";
  case DiagnosticKind::OwnershipWidenedRegion:
    return "analysis widened this ownership region conservatively";
  case DiagnosticKind::OwnershipContractOrigin:
    return "ownership contract originates here";
  case DiagnosticKind::MissingFunctionDefinition:
    return "bodyless Chtholly function has no definition in this module";
  case DiagnosticKind::InvalidConstantFunction:
    return "const fn must be a safe Chtholly definition using constant-safe "
           "operations";
  case DiagnosticKind::InvalidConstantInitializer:
    return "initializer is not a valid compile-time constant";
  case DiagnosticKind::InvalidStaticInitializer:
    return "static initializer cannot be encoded as a readonly object";
  case DiagnosticKind::ConstantEvaluationLimit:
    return "constant evaluation exceeded its deterministic execution budget";
  case DiagnosticKind::ConstantEvaluationCycle:
    return "constant declarations form an evaluation cycle";
  case DiagnosticKind::ConstantEvaluationFatalFailure:
    return "constant evaluation executes unrecoverable failure";
  case DiagnosticKind::InvalidLayoutQuery:
    return "layout query requires a complete visible object representation";
  case DiagnosticKind::InvalidTypeQuery:
    return "type query has an invalid arity, type, category, or capability";
  case DiagnosticKind::RecursiveNominalType:
    return "nominal value fields form an infinitely recursive object type";
  case DiagnosticKind::InvalidBorrowOperand:
    return "borrow requires a materializable value or addressable place";
  case DiagnosticKind::InvalidArrayLiteral:
    return "array literal requires at least one element with a common type";
  case DiagnosticKind::InvalidPatternBinding:
    return "pattern binding requires a name, transfer, and valid projection";
  case DiagnosticKind::InvalidSemanticShape:
    return "source form could not be represented by its required semantic "
           "shape";
  case DiagnosticKind::InvalidConcreteSpecializationArtifact:
    return "concrete specialization artifact is missing, stale, or invalid";
  case DiagnosticKind::ConcreteSpecializationFailure:
    return "generic specialization could not be materialized for this use";
  case DiagnosticKind::MissingHashWitness:
    return "container key type has no complete std::hash::Hash witness";
  case DiagnosticKind::MissingEqualWitness:
    return "container key type has no complete std::hash::Equal witness";
  case DiagnosticKind::MissingLifecycleWitness:
    return "container key or value type has no complete move/drop witness";
  case DiagnosticKind::WitnessSpecializationFailure:
    return "container witness specialization could not be materialized";
  case DiagnosticKind::WitnessArtifactMismatch:
    return "imported container witness artifact does not match its fingerprint";
  case DiagnosticKind::InvalidImportedValueArtifact:
    return "imported constant or static value artifact is invalid";
  case DiagnosticKind::UnsafeCallRequired:
    return "call to an unsafe foreign callable requires an unsafe block";
  case DiagnosticKind::InvalidEnumVariant:
    return "enum variant or payload does not match its declaration";
  case DiagnosticKind::MissingEnumPayloadTransfer:
    return "enum payload binding requires move, copy, or borrow";
  case DiagnosticKind::InvalidPatternRest:
    return "pattern rest must appear once as the final item";
  case DiagnosticKind::IncompletePattern:
    return "pattern must cover every logical leaf or end with rest";
  case DiagnosticKind::OverlappingPatternProjection:
    return "pattern projection overlaps an earlier projection";
  case DiagnosticKind::NonExhaustiveSwitch:
    return "switch does not cover every enum variant";
  case DiagnosticKind::DuplicateSwitchArm:
    return "switch contains a duplicate enum variant arm";
  case DiagnosticKind::UnreachableSwitchArm:
    return "switch arm is unreachable after an earlier wildcard";
  case DiagnosticKind::UnreachableCode:
    return "statement is unreachable";
  case DiagnosticKind::InvalidTryOperand:
    return "question-mark requires std::result::Result<T, E>";
  case DiagnosticKind::InvalidTryReturn:
    return "question-mark requires a Result return type with the same error "
           "type";
  case DiagnosticKind::InvalidAsyncDeclaration:
    return "async currently requires a non-generic Chtholly free function";
  case DiagnosticKind::InvalidAsyncEntryPoint:
    return "async main must have no parameters and return i32";
  case DiagnosticKind::InvalidAsyncOperation:
    return "task creation and cancellation operations require an async body";
  case DiagnosticKind::InvalidWaitOperand:
    return "wait requires an owned compiler task";
  case DiagnosticKind::MissingAsyncResultImport:
    return "fallible wait requires the canonical std::result::Result type";
  case DiagnosticKind::MissingCFFIResultImport:
    return "foreign error contract requires an import of std::result";
  case DiagnosticKind::MissingCFFIOutcomeImport:
    return "foreign POSIX outcome requires imports of std::io and std::result";
  case DiagnosticKind::TaskDiscard:
    return "an async call result must be waited or bound to an inferred local";
  case DiagnosticKind::TaskNotConsumed:
    return "task must be consumed by wait before this non-cancellation exit";
  case DiagnosticKind::TaskAlreadyConsumed:
    return "task has already been consumed by wait";
  case DiagnosticKind::TaskScopeEscape:
    return "a task handle cannot escape its lexical task scope";
  case DiagnosticKind::UninitializedStorage:
    return "storage declared without an initializer must be initialized by a "
           "verified Initialize callable effect before use or scope exit";
  case DiagnosticKind::BreakOutsideLoop:
    return "break can only be used inside a loop";
  case DiagnosticKind::ContinueOutsideLoop:
    return "continue can only be used inside a loop";
  case DiagnosticKind::InvalidLoopLabel:
    return "a statement label must introduce while, for, or do";
  case DiagnosticKind::UnknownLoopLabel:
    return "loop control names no active enclosing loop label";
  case DiagnosticKind::DuplicateLoopLabel:
    return "an active enclosing loop already uses this label";
  case DiagnosticKind::AsyncRequires11:
    return "async syntax requires Chtholly language 1.1 or newer";
  case DiagnosticKind::ConcurrencyRequires11:
    return "atomic and volatile operations require Chtholly language 1.1 or "
           "newer";
  case DiagnosticKind::CallableRequires12:
    return "closure and bound-method syntax requires Chtholly language 1.2 or "
           "newer";
  case DiagnosticKind::ModuleAliasRequires13:
    return "module aliases require Chtholly language 1.3 or newer";
  case DiagnosticKind::LoopLabelRequires13:
    return "loop labels require Chtholly language 1.3 or newer";
  case DiagnosticKind::OperatorRequires13:
    return "operator protocol dispatch requires Chtholly language 1.3 or newer";
  case DiagnosticKind::ForeachRequires14:
    return "foreach syntax requires Chtholly language 1.4 or newer";
  case DiagnosticKind::ValueBlockRequires17:
    return "block values and implicit callable returns require Chtholly "
           "language 1.7 or newer";
  case DiagnosticKind::SwitchStatementRequires17:
    return "switch statement syntax requires Chtholly language 1.7 or newer";
  case DiagnosticKind::MissingReturnValue:
    return "reachable callable body has no value for its declared return type";
  case DiagnosticKind::CollectVecRequiresOwnedItem:
    return "collect_vec requires a movable owned Item without loan carriers";
  case DiagnosticKind::ValueEnumRequires18:
    return "explicit value enums require Chtholly language 1.8 or newer";
  case DiagnosticKind::ValueEnumExplicitDiscriminantRequired:
    return "every value-enum variant requires an explicit i32 discriminant";
  case DiagnosticKind::ValueEnumInvalidDiscriminant:
    return "value-enum discriminant must be an i32 integer literal";
  case DiagnosticKind::ValueEnumDuplicateDiscriminant:
    return "value-enum discriminants must be unique";
  case DiagnosticKind::InvalidValueEnumDeclaration:
    return "value enums cannot declare payloads, generics, constraints, "
           "lifecycle, or representation controls";
  case DiagnosticKind::ValueEnumBracedConstruction:
    return "value-enum variants are values and cannot use braced construction";
  case DiagnosticKind::InvalidForeachIterator:
    return "foreach requires a verified iterator conformance";
  case DiagnosticKind::InvalidForeachBinding:
    return "foreach binding must be let or var, an identifier, and an optional "
           "type";
  case DiagnosticKind::ForeachIteratorMoveRequired:
    return "foreach requires an owned iterator value; move or copy a named "
           "iterator";
  case DiagnosticKind::ForeachItemTypeMismatch:
    return "foreach item annotation does not match the iterator Item reference";
  case DiagnosticKind::ForeachProtocolMismatch:
    return "foreach iterator must return the canonical Item/Done step";
  case DiagnosticKind::UnsupportedAtomicType:
    return "atomic operation requires bool or a builtin integer scalar";
  case DiagnosticKind::InvalidMemoryOrder:
    return "atomic operation has an invalid or non-constant memory order";
  case DiagnosticKind::InvalidCompilerIntrinsic:
    return "compiler intrinsic call has an invalid canonical signature";
  case DiagnosticKind::AmbiguousBoundMethod:
    return "bound method formation requires one receiver-selected target";
  case DiagnosticKind::UnsupportedBoundMethod:
    return "this method family cannot form a bound value";
  case DiagnosticKind::UnsupportedSemantics:
    return "syntax is not supported by the next semantic pipeline";
  case DiagnosticKind::InvalidSemIR:
    return "semantic analysis produced invalid IR";
  case DiagnosticKind::CFDLInvalidSyntax:
    return "invalid CFDL syntax";
  case DiagnosticKind::CFDLInvalidModule:
    return "CFDL requires one leading module declaration";
  case DiagnosticKind::CFDLInvalidImport:
    return "CFDL may import only foreign binding modules";
  case DiagnosticKind::CFDLDuplicateName:
    return "duplicate name in the CFDL binding module";
  case DiagnosticKind::CFDLUnknownType:
    return "unknown CFDL ABI type";
  case DiagnosticKind::CFDLUnknownCallable:
    return "unknown CFDL foreign callable";
  case DiagnosticKind::CFDLInvalidCallable:
    return "invalid CFDL foreign callable signature";
  case DiagnosticKind::CFDLIncompleteCarrier:
    return "incomplete foreign type requires an explicit carrier for this use";
  case DiagnosticKind::CFDLInvalidCarrier:
    return "foreign carrier shape or invalid sentinel is incompatible";
  case DiagnosticKind::CFDLCompletionFailed:
    return "CFDL nominal definition completion failed";
  case DiagnosticKind::CFDLInvalidProtocol:
    return "invalid normalized CFDL resource protocol";
  case DiagnosticKind::Count:
    break;
  }
  return "unknown diagnostic";
}

} // namespace chtholly::compiler
