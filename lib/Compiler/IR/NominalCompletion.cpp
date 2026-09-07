#include "chtholly/Compiler/NominalCompletion.h"

#include <algorithm>

namespace chtholly::compiler {

void NominalCompletionService::registerShell(NominalTypeId id,
                                             std::string identity,
                                             NodeId declaration) {
  if (id.hasValue())
    records_.try_emplace(id.index,
                         Record{std::move(identity), declaration});
}

NominalCompletionResult NominalCompletionService::requireComplete(
    NominalTypeId id, NodeId request, const std::function<bool()> &complete) {
  auto *record = find(id);
  if (!record)
    return {NominalCompletionState::Failed, nullptr};
  if (record->state == NominalCompletionState::Complete)
    return {record->state, nullptr};
  if (record->state == NominalCompletionState::Failed)
    return {record->state, &record->failure};
  if (record->state == NominalCompletionState::Completing) {
    const auto begin = std::ranges::find(stack_, id);
    record->failure = {NominalCompletionFailureKind::Cycle, request, {}};
    for (auto current = begin; current != stack_.end(); ++current)
      record->failure.cycle.emplace_back(identity(*current));
    record->failure.cycle.emplace_back(record->identity);
    record->state = NominalCompletionState::Failed;
    return {record->state, &record->failure};
  }

  record->state = NominalCompletionState::Completing;
  stack_.push_back(id);
  const auto succeeded = complete();
  stack_.pop_back();
  // A recursive request can install a more precise cycle failure.
  record = find(id);
  if (record->state == NominalCompletionState::Completing) {
    record->state = succeeded ? NominalCompletionState::Complete
                              : NominalCompletionState::Failed;
    if (!succeeded)
      record->failure = {NominalCompletionFailureKind::Definition, request, {}};
  }
  return {record->state,
          record->state == NominalCompletionState::Failed ? &record->failure
                                                          : nullptr};
}

NominalCompletionState
NominalCompletionService::state(NominalTypeId id) const {
  const auto *record = find(id);
  return record ? record->state : NominalCompletionState::Failed;
}

const NominalCompletionFailure *
NominalCompletionService::failure(NominalTypeId id) const {
  const auto *record = find(id);
  return record && record->state == NominalCompletionState::Failed
             ? &record->failure
             : nullptr;
}

std::string_view NominalCompletionService::identity(NominalTypeId id) const {
  const auto *record = find(id);
  return record ? std::string_view(record->identity) : std::string_view{};
}

bool NominalCompletionService::markFailureDiagnosed(NominalTypeId id) {
  auto *record = find(id);
  if (!record || record->state != NominalCompletionState::Failed ||
      record->failure_diagnosed)
    return false;
  record->failure_diagnosed = true;
  return true;
}

NominalCompletionService::Record *
NominalCompletionService::find(NominalTypeId id) {
  const auto found = id.hasValue() ? records_.find(id.index) : records_.end();
  return found == records_.end() ? nullptr : &found->second;
}

const NominalCompletionService::Record *
NominalCompletionService::find(NominalTypeId id) const {
  const auto found = id.hasValue() ? records_.find(id.index) : records_.end();
  return found == records_.end() ? nullptr : &found->second;
}

} // namespace chtholly::compiler
