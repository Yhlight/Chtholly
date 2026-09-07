#pragma once

// Private foreign-ABI classification and verified protocol helpers. This
// header is included inside the lowering implementation's anonymous namespace
// by both foreign layout translation units.

constexpr auto Names = std::to_array<std::string_view>({
#define CHTHOLLY_COMPILER_LOW_INST(Name, Arg0, Arg1) #Name,
#include "chtholly/Compiler/LowIRKind.def"
});
constexpr auto Args = std::to_array<std::array<LowArgKind, 2>>({
#define CHTHOLLY_COMPILER_LOW_INST(Name, Arg0, Arg1)                               \
  std::array{LowArgKind::Arg0, LowArgKind::Arg1},
#include "chtholly/Compiler/LowIRKind.def"
});
static_assert(Names.size() == static_cast<std::size_t>(LowInstKind::Count));
static_assert(Args.size() == static_cast<std::size_t>(LowInstKind::Count));

bool isTerminator(LowInstKind kind) {
  return kind == LowInstKind::Branch || kind == LowInstKind::BranchIf ||
         kind == LowInstKind::Return || kind == LowInstKind::ReturnInPlace ||
         kind == LowInstKind::Unreachable ||
         kind == LowInstKind::FatalFailure ||
         kind == LowInstKind::CoroutineRuntimeFault ||
         kind == LowInstKind::CoroutineReturnSuccess ||
         kind == LowInstKind::CoroutineReturnError ||
         kind == LowInstKind::CoroutineReturnCancelled ||
         kind == LowInstKind::CoroutineCleanupEnd;
}

ForeignAbiTargetKind classifyForeignTarget(std::string_view triple) {
  std::string value(triple);
  std::ranges::transform(value, value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  const auto windows = value.find("windows") != std::string::npos;
  if (value.starts_with("x86_64") || value.starts_with("amd64"))
    return windows ? ForeignAbiTargetKind::WindowsX64
                   : ForeignAbiTargetKind::SysVAMD64;
  if (value.starts_with("aarch64") && !windows &&
      (value.find("linux") != std::string::npos ||
       value.find("gnu") != std::string::npos))
    return ForeignAbiTargetKind::AAPCS64;
  return ForeignAbiTargetKind::Unsupported;
}

const ForeignAbiSignature *foreignSignature(const SemIR &sem_ir,
                                            FunctionRefId target) {
  const auto &reference = sem_ir.functionRef(target);
  if (reference.local_function.hasValue()) {
    const auto &declaration =
        sem_ir.functionDeclaration(reference.local_function);
    return declaration.foreign_signature ? &*declaration.foreign_signature
                                         : nullptr;
  }
  const auto *entity = sem_ir.importIRs().tryGetEntity(reference.public_entity);
  return entity && entity->foreign_signature ? &*entity->foreign_signature
                                             : nullptr;
}

const interop::ForeignOperationArtifact *
foreignOperation(const SemIR &sem_ir, FunctionRefId target) {
  const auto &reference = sem_ir.functionRef(target);
  if (reference.local_function.hasValue()) {
    const auto &declaration =
        sem_ir.functionDeclaration(reference.local_function);
    return declaration.interop_artifact
               ? sem_ir.importIRs().interopRegistry().resolve(
                     *declaration.interop_artifact)
               : nullptr;
  }
  const auto *entity = sem_ir.importIRs().tryGetEntity(reference.public_entity);
  return entity && entity->interop_artifact
             ? sem_ir.importIRs().interopRegistry().resolve(
                   *entity->interop_artifact)
             : nullptr;
}

bool isForeign(const SemIR &sem_ir, FunctionRefId target) {
  const auto &reference = sem_ir.functionRef(target);
  if (reference.local_function.hasValue())
    return sem_ir.functionDeclaration(reference.local_function).kind ==
           SemCallableDeclarationKind::Foreign;
  const auto *entity = sem_ir.importIRs().tryGetEntity(reference.public_entity);
  return entity &&
         entity->declaration_kind == PublicCallableDeclarationKind::Foreign;
}

const CanonicalForeignResourceProtocol *verifiedForeignResourceProtocol(
    const SemIR &sem_ir, TypeId type, bool completion_projection,
    ForeignResourceProtocolId &protocol_id, std::string &error) {
  protocol_id = sem_ir.foreignResourceProtocolId(type);
  const auto &protocol = sem_ir.foreignResourceProtocol(type);
  const auto fields = sem_ir.typeBlock(TypeBlockId(sem_ir.type(type).arg0));
  if (protocol.facts.completion_projection != completion_projection ||
      protocol.types.size() != fields.size()) {
    error = "foreign resource protocol disagrees with its semantic type";
    return nullptr;
  }
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (protocol.types[index] != sem_ir.canonicalType(fields[index])) {
      error = "foreign resource protocol has stale canonical type bindings";
      return nullptr;
    }
  }
  if (!protocol.facts.verify(static_cast<std::uint32_t>(protocol.types.size()),
                             error))
    return nullptr;
  return &protocol;
}
