#pragma once

#include "chtholly/Compiler/LowIR.h"

namespace chtholly::compiler::internal {

struct LowIRForeignVerificationService {
  [[nodiscard]] static const ForeignAbiSignature *foreignSignature(
      const SemIR &sem_ir, FunctionRefId target);
  [[nodiscard]] static const interop::ForeignOperationArtifact *
  foreignOperation(const SemIR &sem_ir, FunctionRefId target);
  [[nodiscard]] static bool isForeign(const SemIR &sem_ir,
                                      FunctionRefId target);
};

class LowIRVerificationContext {
public:
  explicit LowIRVerificationContext(const LowIR &low_ir) : low_ir_(low_ir) {}

  [[nodiscard]] bool verifyPreconditions(std::string &error) const;
  [[nodiscard]] bool verifyStructure(std::string &error) const;
  [[nodiscard]] bool verifyCleanupGraphs(
      std::vector<FunctionId> &instruction_owners,
      std::vector<CoroutineCleanupGraphId> &instruction_cleanup_graph,
      std::string &error) const;
  [[nodiscard]] bool verifyBlocks(std::string &error) const;
  [[nodiscard]] bool verifyCoroutinePlans(std::string &error) const;
  [[nodiscard]] bool verifyForeignPlans(std::string &error) const;
  [[nodiscard]] bool verifyRepresentations(std::string &error) const;
  [[nodiscard]] bool verifyAbiIndexes(std::string &error) const;
  [[nodiscard]] bool verifyForeignInstruction(LowInstId id,
                                               std::string &error) const;
  [[nodiscard]] bool verifyCoroutineInstruction(
      LowInstId id, std::span<const FunctionId> instruction_owners,
      std::string &error) const;
  [[nodiscard]] bool verifyObjectInstruction(LowInstId id,
                                             std::string &error) const;
  [[nodiscard]] bool verifyScalarInstruction(LowInstId id,
                                             std::string &error) const;

private:
  const LowIR &low_ir_;
};

} // namespace chtholly::compiler::internal
