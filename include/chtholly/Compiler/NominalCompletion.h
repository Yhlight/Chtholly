#pragma once

#include "chtholly/Compiler/CompilationIds.h"
#include "chtholly/Compiler/ParseTree.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace chtholly::compiler {

enum class NominalCompletionState : std::uint8_t {
  Unloaded,
  Completing,
  Complete,
  Failed,
};

enum class NominalCompletionFailureKind : std::uint8_t {
  None,
  Definition,
  Cycle,
};

struct NominalCompletionFailure {
  NominalCompletionFailureKind kind = NominalCompletionFailureKind::None;
  NodeId location;
  std::vector<std::string> cycle;
};

struct NominalCompletionResult {
  NominalCompletionState state = NominalCompletionState::Unloaded;
  const NominalCompletionFailure *failure = nullptr;

  [[nodiscard]] bool succeeded() const {
    return state == NominalCompletionState::Complete;
  }
};

// Tracks declaration completion separately from stable nominal shell identity.
class NominalCompletionService {
public:
  void registerShell(NominalTypeId id, std::string identity,
                     NodeId declaration);

  [[nodiscard]] NominalCompletionResult
  requireComplete(NominalTypeId id, NodeId request,
                  const std::function<bool()> &complete);

  [[nodiscard]] NominalCompletionState state(NominalTypeId id) const;
  [[nodiscard]] const NominalCompletionFailure *failure(NominalTypeId id) const;
  [[nodiscard]] std::string_view identity(NominalTypeId id) const;
  [[nodiscard]] bool markFailureDiagnosed(NominalTypeId id);

private:
  struct Record {
    std::string identity;
    NodeId declaration;
    NominalCompletionState state = NominalCompletionState::Unloaded;
    NominalCompletionFailure failure;
    bool failure_diagnosed = false;
  };

  [[nodiscard]] Record *find(NominalTypeId id);
  [[nodiscard]] const Record *find(NominalTypeId id) const;

  std::unordered_map<std::uint32_t, Record> records_;
  std::vector<NominalTypeId> stack_;
};

} // namespace chtholly::compiler
