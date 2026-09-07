#include "chtholly/Compiler/SemIR.h"

#include <array>

namespace chtholly::compiler {
namespace {

constexpr auto InstNames = std::to_array<std::string_view>({
#define CHTHOLLY_COMPILER_SEM_INST(Name, Arg0, Arg1) #Name,
#include "chtholly/Compiler/SemIRKind.def"
});

constexpr auto InstArgs = std::to_array<std::array<SemArgKind, 2>>({
#define CHTHOLLY_COMPILER_SEM_INST(Name, Arg0, Arg1)                               \
  std::array{SemArgKind::Arg0, SemArgKind::Arg1},
#include "chtholly/Compiler/SemIRKind.def"
});

constexpr auto TypeNames =
    std::to_array<std::string_view>({"invalid",
                                     "void",
                                     "bool",
                                     "integer",
                                     "float",
                                     "string",
                                     "array",
                                     "function",
                                     "async-function",
                                     "type-parameter",
                                     "nominal",
                                     "reference",
                                     "raw-pointer",
                                     "c-function-pointer",
                                     "c-variadic-function-pointer",
                                     "callback-adapter",
                                     "callback-registration",
                                     "callback-completion",
                                     "callback-wake",
                                     "foreign-completion",
                                     "foreign-wake",
                                     "coroutine-executor",
                                     "coroutine-scope",
                                     "coroutine-task",
                                     "coroutine-task-outcome",
                                     "coroutine-task-completion",
                                     "coroutine-task-completion-set",
                                     "coroutine-task-selection",
                                     "coroutine-checked",
                                     "never",
                                     "tuple",
                                     "slice",
                                     "type-projection",
                                     "char"});

static_assert(InstNames.size() == static_cast<std::size_t>(SemInstKind::Count));
static_assert(InstArgs.size() == static_cast<std::size_t>(SemInstKind::Count));
static_assert(TypeNames.size() == static_cast<std::size_t>(SemTypeKind::Count));

} // namespace

std::string_view semInstKindName(SemInstKind kind) {
  const auto index = static_cast<std::size_t>(kind);
  return index < InstNames.size() ? InstNames[index] : "InvalidSemInstKind";
}

SemArgKind semInstArgKind(SemInstKind kind, std::size_t index) {
  const auto kind_index = static_cast<std::size_t>(kind);
  return kind_index < InstArgs.size() && index < 2 ? InstArgs[kind_index][index]
                                                   : SemArgKind::None;
}

std::string_view semTypeKindName(SemTypeKind kind) {
  const auto index = static_cast<std::size_t>(kind);
  return index < TypeNames.size() ? TypeNames[index] : "invalid-type-kind";
}

SemExprCategory expressionCategory(const SemIR &sem_ir, InstId instruction) {
  if (!instruction.hasValue() || instruction.index >= sem_ir.instCount())
    return SemExprCategory::Error;
  const auto &value = sem_ir.inst(instruction);
  if (TypeId(value.type) == sem_ir.neverType())
    return SemExprCategory::Diverging;
  switch (value.kind) {
  case SemInstKind::Invalid:
    return SemExprCategory::Error;
  case SemInstKind::NameRef:
  case SemInstKind::StructFieldAccess:
  case SemInstKind::UnionFieldAccess:
  case SemInstKind::Index:
  case SemInstKind::Dereference:
    return SemExprCategory::Place;
  case SemInstKind::Move:
  case SemInstKind::Copy:
  case SemInstKind::ArrayLiteral:
  case SemInstKind::TupleLiteral:
  case SemInstKind::AggregateInit:
  case SemInstKind::Closure:
  case SemInstKind::BoundMethod:
  case SemInstKind::UnionInit:
  case SemInstKind::EnumInit:
  case SemInstKind::Call:
  case SemInstKind::ForeignOperationCall:
  case SemInstKind::CompilerIntrinsicCall:
  case SemInstKind::Placement:
  case SemInstKind::IndirectCall:
  case SemInstKind::IndirectForeignCall:
  case SemInstKind::CallbackAdapterCall:
  case SemInstKind::If:
  case SemInstKind::Switch:
  case SemInstKind::ScopedBlock:
  case SemInstKind::MaterializeTemporary:
    return SemExprCategory::Temporary;
  case SemInstKind::Return:
  case SemInstKind::Break:
  case SemInstKind::Continue:
    return SemExprCategory::Diverging;
  default:
    return SemExprCategory::Value;
  }
}

} // namespace chtholly::compiler
