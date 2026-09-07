#include "chtholly/Compiler/TypeConcurrency.h"

#include <algorithm>

namespace chtholly::compiler {

TypeConcurrencySummary::TypeConcurrencySummary(
    SpecificResolver resolve_specific, ForeignResolver resolve_foreign,
    std::size_t max_work_items)
    : resolve_specific_(std::move(resolve_specific)),
      resolve_foreign_(std::move(resolve_foreign)),
      max_work_items_(max_work_items) {}

TypeConcurrencyFacts
TypeConcurrencySummary::summarize(const PublicType &type, std::string &error) {
  error.clear();
  work_items_ = 0;
  active_.clear();
  return summarizeImpl(type, error, 0);
}

TypeConcurrencyFacts TypeConcurrencySummary::summarizeImpl(
    const PublicType &type, std::string &error, std::size_t depth) {
  if (++work_items_ > max_work_items_) {
    error = "Send/Sync capability worklist exceeded its bound";
    return {};
  }
  const auto key = canonicalPublicTypeBytes(type);
  if (const auto found = completed_.find(key); found != completed_.end())
    return found->second;
  // A by-value recursive dependency should already be rejected by nominal
  // layout validation. Keep this query fail-closed if it reaches the summary.
  if (!active_.insert(key).second)
    return {};

  TypeConcurrencyFacts result{};
  switch (type.kind) {
  case PublicTypeKind::Bool:
  case PublicTypeKind::Char:
  case PublicTypeKind::Integer:
  case PublicTypeKind::Float:
    result = {true, true};
    break;
  case PublicTypeKind::Array:
  case PublicTypeKind::Tuple:
    result = {true, true};
    for (const auto &argument : type.arguments) {
      const auto child = summarizeImpl(argument, error, depth + 1);
      result.transferable &= child.transferable;
      result.shareable &= child.shareable;
    }
    break;
  case PublicTypeKind::Nominal: {
    if (resolve_foreign_) {
      if (const auto foreign = resolve_foreign_(type)) {
        result = *foreign;
        break;
      }
    }
    const auto *specific = resolve_specific_ ? resolve_specific_(type) : nullptr;
    if (!specific)
      break;
    result = {true, true};
    for (const auto &field : specific->fields) {
      const auto child = summarizeImpl(field.type, error, depth + 1);
      result.transferable &= child.transferable;
      result.shareable &= child.shareable;
    }
    for (const auto &variant : specific->variants)
      for (const auto &field : variant.fields) {
        const auto child = summarizeImpl(field.type, error, depth + 1);
        result.transferable &= child.transferable;
        result.shareable &= child.shareable;
      }
    break;
  }
  case PublicTypeKind::Void:
  case PublicTypeKind::Never:
  case PublicTypeKind::String:
  case PublicTypeKind::Reference:
  case PublicTypeKind::Slice:
  case PublicTypeKind::RawPointer:
  case PublicTypeKind::Function:
  case PublicTypeKind::CFunctionPointer:
  case PublicTypeKind::TypeParameter:
  case PublicTypeKind::TypeProjection:
  case PublicTypeKind::CallbackAdapter:
  case PublicTypeKind::CallbackRegistration:
  case PublicTypeKind::CallbackCompletion:
  case PublicTypeKind::CallbackWake:
  case PublicTypeKind::ForeignCompletion:
  case PublicTypeKind::ForeignWake:
  case PublicTypeKind::ForeignOperationState:
  case PublicTypeKind::Count:
    // Borrowed, opaque, function, and pointer representations do not acquire
    // cross-thread capabilities from their physical representation.
    break;
  }
  active_.erase(key);
  if (error.empty())
    completed_.emplace(key, result);
  return result;
}

} // namespace chtholly::compiler
