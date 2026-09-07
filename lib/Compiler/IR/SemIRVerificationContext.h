#pragma once

#include "chtholly/Compiler/SemIR.h"

#include <functional>
#include <optional>
#include <utility>

namespace chtholly::compiler::internal {

class SemIRVerificationContext {
public:
  explicit SemIRVerificationContext(const SemIR &sem_ir) : sem_ir_(sem_ir) {}

  [[nodiscard]] bool verifyStructure(std::string &error) const;
  [[nodiscard]] bool verifyEntityTables(std::string &error) const;
  [[nodiscard]] bool verifyTypeRecords(std::string &error) const;
  [[nodiscard]] bool verifyConstantRecords(std::string &error) const;
  [[nodiscard]] bool verifyDeclarationRecords(std::string &error) const;
  [[nodiscard]] bool verifyControlFlow(std::string &error) const;
  [[nodiscard]] bool verifyFunctions(std::string &error) const;
  [[nodiscard]] bool verifyInstructions(std::string &error) const;

private:
  [[nodiscard]] bool verifyCompilerIntrinsicInstruction(
      const SemInst &value, TypeId instruction_type,
      const std::function<std::optional<std::uint8_t>(InstId)>
          &constant_enum_discriminant,
      std::string &error) const;
  // The instruction verifier was extracted from SemIR::verify without
  // changing its access model. These forwarding views keep the moved logic
  // read-only while avoiding a second SemIR store or copied state.
#define CHTHOLLY_SEMIR_VERIFY_FORWARD(Name)                                  \
  template <typename... Args> decltype(auto) Name(Args &&...args) const {    \
    return sem_ir_.Name(std::forward<Args>(args)...);                         \
  }
  CHTHOLLY_SEMIR_VERIFY_FORWARD(asyncErrorType)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(asyncSuccessType)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(callbackArmParameters)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(callbackCompletionAuthority)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(callbackContextParameter)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(callbackContract)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(callbackDetachParameters)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(callbackRegistrationAuthority)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(callbackRegistrationBindings)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(canonicalReadOutcomeShape)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(canonicalResultShape)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(completionSetElementType)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(constantEntity)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(constantValue)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(coroutineCheckedPayloadType)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(coroutineConstructorEntity)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(coroutineTaskCompletionCapacity)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(coroutineTaskErrorType)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(coroutineTaskSuccessType)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(enumPayloadFieldType)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(foreignOperationStateOwner)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(function)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(functionDeclaration)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(functionIntrinsicRole)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(functionOwnership)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(functionRef)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(functionRefCount)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(functionSemanticContract)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(i32Type)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(identifier)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(importIRs)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(inst)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(instBlock)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(integer)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(integerCount)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(local)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(name)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(nominalFieldType)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(nominalType)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(rawPointerPointee)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(rawPointerPointeeConst)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(referenceMutability)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(referencePointee)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(tryGetCallableEnvironment)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(tupleElementType)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(type)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(typeBlock)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(typeRepresentation)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(voidType)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(containsArg)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(foreignRepresentationType)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(isConcreteReverseTarget)
  CHTHOLLY_SEMIR_VERIFY_FORWARD(isCompletionAggregationProvider)
#undef CHTHOLLY_SEMIR_VERIFY_FORWARD
  const SemIR &sem_ir_;
};

} // namespace chtholly::compiler::internal
