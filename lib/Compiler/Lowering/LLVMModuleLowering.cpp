#include "chtholly/Compiler/LLVM.h"

#include "chtholly/Compiler/BuiltinOperator.h"
#include "chtholly/Compiler/ProgramModel.h"

#include "LLVMInternal.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chtholly::compiler {

namespace {

class ModuleLoweringContext {
public:
  ModuleLoweringContext(
      const LowIR &low_ir, std::string_view package_name,
      ModuleEmissionRole emission_role, llvm::Module &module,
      std::span<const std::uint32_t> native_definition_exports,
      std::span<const std::pair<std::string, std::string>>
          runtime_symbol_mappings,
      DebugInfoMode debug_info,
      std::span<const ComponentExportLoweringPlan> component_exports)
      : low_ir_(low_ir), sem_ir_(low_ir.semIR()), module_(module),
        context_(module.getContext()), package_name_(package_name),
        emission_role_(emission_role),
        runtime_symbol_mappings_(runtime_symbol_mappings),
        debug_info_(debug_info), component_exports_(component_exports),
        type_state_(debug_types_, types_),
        value_state_(values_, slots_, place_flags_, initialized_slots_),
        object_state_(functions_, static_globals_, local_function_refs_,
                     coroutine_constructors_),
        object_value_state_{
            low_ir_, module_, functions_, instruction_error_,
            [this](TypeId type) { return lowerObjectType(type); },
            [this](TypeId type) { return lowerValueType(type); },
            [this](TypeId type) {
              return usesPointerValueRepresentation(type);
            }},
        intrinsic_state_{
            low_ir_, sem_ir_, module_, context_, functions_,
            instruction_error_,
            [this](LowInstId id) { return value(id); },
            [this](TypeId type) { return lowerValueType(type); },
            [this](TypeId type) { return lowerObjectType(type); },
            [this](llvm::Function &function, llvm::Type *type,
                   llvm::StringRef name) {
              return entryAlloca(function, type, name);
            },
            [this](llvm::Value *destination, llvm::Value *source, TypeId type,
                   llvm::IRBuilder<> &builder) {
              copyObject(destination, source, type, builder);
            },
            [this](llvm::Value *destination, llvm::Value *source, TypeId type,
                   llvm::IRBuilder<> &builder) {
              moveSemanticObject(destination, source, type, builder);
            },
            [this](llvm::Value *destination, llvm::Value *source, TypeId type,
                   llvm::IRBuilder<> &builder) {
              moveSemanticValueToObject(destination, source, type, builder);
            },
            [this](llvm::Value *source, TypeId type,
                   llvm::IRBuilder<> &builder) {
              return loadValueFromObject(source, type, builder);
            },
            [this](llvm::Value *destination, llvm::Value *source, TypeId type,
                   llvm::IRBuilder<> &builder) {
              storeValueToObject(destination, source, type, builder);
            },
            [this](llvm::Value *owner, TypeId type, std::uint32_t variant,
                   std::uint32_t field, llvm::IRBuilder<> &builder) {
              return enumPayloadAddress(owner, type, variant, field, builder);
            },
            [this](TypeId type, llvm::Value *address,
                   llvm::IRBuilder<> &builder, llvm::Function &function) {
              emitCoroutineDestroyAddress(type, address, builder, function);
            },
            [this](TypeId type) {
              if (!current_function_.hasValue())
                return static_cast<llvm::GlobalVariable *>(nullptr);
              const auto owner = low_ir_.function(current_function_).semantic_function;
              for (const auto &descriptor : sem_ir_.typedChannelDescriptors())
                if (descriptor.owner_function == owner &&
                    sem_ir_.canonicalType(descriptor.payload_type) ==
                        sem_ir_.canonicalType(type))
                  return typedChannelRuntimeDescriptor(type, descriptor);
              return static_cast<llvm::GlobalVariable *>(nullptr);
            },
            [this](TypeId payload, llvm::Value *channel, llvm::Value *source,
                   unsigned operation, std::uint32_t channel_key,
                   TypeId channel_type, llvm::IRBuilder<> &builder,
                   llvm::Function &function) {
              return lowerTypedChannelTransition(payload, channel, source,
                                                 operation, channel_key,
                                                 channel_type, builder, function);
            }},
        scalar_instruction_state_{
            low_ir_, sem_ir_, module_, context_, instruction_error_,
            [this](LowInstId id) { return value(id); },
            [this](TypeId type) { return lowerValueType(type); },
            [this](TypeId type) { return lowerObjectType(type); },
            [this](llvm::Function &function, llvm::Type *type,
                   llvm::StringRef name) {
              return entryAlloca(function, type, name);
            },
            [this](llvm::Value *destination, llvm::Value *source, TypeId type,
                   llvm::IRBuilder<> &builder) {
              storeValueToObject(destination, source, type, builder);
            },
            [this](llvm::Value *owner, TypeId type, std::uint32_t variant,
                   std::uint32_t field, llvm::IRBuilder<> &builder) {
              return enumPayloadAddress(owner, type, variant, field, builder);
            }},
        object_instruction_state_{
            low_ir_, sem_ir_, module_, context_, functions_, static_globals_,
            slots_, place_flags_, initialized_slots_, coroutine_state_,
            object_value_state_, instruction_error_,
            [this](LowInstId id) { return value(id); },
            [this](TypeId type) { return lowerValueType(type); },
            [this](TypeId type) { return lowerObjectType(type); },
            [this](llvm::Function &function, llvm::Type *type,
                   llvm::StringRef name) {
              return entryAlloca(function, type, name);
            },
            [this](TypeId type) {
              return usesPointerValueRepresentation(type);
            },
            [this](llvm::Value *destination, llvm::Value *source, TypeId type,
                   llvm::IRBuilder<> &builder, llvm::Function &function) {
              copySemanticObject(destination, source, type, builder, function);
            },
            [this](llvm::Value *owner, TypeId type, std::uint32_t variant,
                   std::uint32_t field, llvm::IRBuilder<> &builder) {
              return enumPayloadAddress(owner, type, variant, field, builder);
            },
            [this](llvm::Value *condition, std::uint32_t reason,
                   std::string_view name, llvm::IRBuilder<> &builder,
                   llvm::Function &function) {
              emitArithmeticTrap(condition, reason, name, builder, function);
            },
            [this](TypeId type, llvm::Value *address, llvm::IRBuilder<> &builder, llvm::Function &function) {
              emitCoroutineDestroyAddress(type, address, builder, function);
            }},
        interop_state_{callback_thunks_, runtime_symbol_mappings_,
                       component_exports_},
        foreign_call_state_{
            context_,
            [this](llvm::Function &function, llvm::Type *type,
                   llvm::StringRef name) {
              return entryAlloca(function, type, name);
            },
            [this](TypeId type) { return lowerObjectType(type); },
            [this](llvm::Value *destination, llvm::Value *source, TypeId type,
                   llvm::IRBuilder<> &builder) {
              copyObject(destination, source, type, builder);
            },
            [this](const ForeignAbiLane &lane) {
              return lowerForeignLane(lane);
            },
            [this](const ForeignAbiFunctionLayout &layout) {
              return foreignFunctionType(layout);
            },
            [this](llvm::CallBase &call,
                   const ForeignAbiFunctionLayout &layout) {
              applyForeignAttributes(call, layout);
            }},
        foreign_result_state_{
            low_ir_, sem_ir_, module_, context_, functions_,
            [this](LowInstId id) { return value(id); },
            [this](llvm::Function &function, llvm::Type *type,
                   llvm::StringRef name) {
              return entryAlloca(function, type, name);
            },
            [this](TypeId type) { return lowerObjectType(type); },
            [this](TypeId type) { return lowerValueType(type); },
            [this](const ForeignAbiFunctionLayout &layout,
                   const ForeignAbiCallLayout &call_layout,
                   llvm::Value *callee,
                   std::span<llvm::Value *const> arguments,
                   llvm::IRBuilder<> &builder, llvm::Function &function) {
              return emitForeignCallValues(layout, call_layout, callee,
                                           arguments, builder, function);
            },
            [this](llvm::Value *owner, TypeId type, std::uint32_t variant,
                   std::uint32_t field, llvm::IRBuilder<> &builder) {
              return enumPayloadAddress(owner, type, variant, field, builder);
            },
            [this](llvm::Value *destination, llvm::Value *source, TypeId type,
                   llvm::IRBuilder<> &builder) {
              storeValueToObject(destination, source, type, builder);
            }},
        foreign_value_state_{
            low_ir_, sem_ir_, context_, functions_,
            [this](LowInstId id) { return value(id); },
            [this](TypeId type) { return lowerValueType(type); },
            [this](TypeId type) { return lowerObjectType(type); },
            [this](TypeId type) {
              return usesPointerValueRepresentation(type);
            },
            [this](llvm::Function &function, llvm::Type *type,
                   llvm::StringRef name) {
              return entryAlloca(function, type, name);
            },
            [this](llvm::Value *destination, llvm::Value *source, TypeId type,
                   llvm::IRBuilder<> &builder) {
              storeValueToObject(destination, source, type, builder);
            },
            [this](llvm::Value *source, TypeId type,
                   llvm::IRBuilder<> &builder) {
              return loadValueFromObject(source, type, builder);
            }},
        callback_thunk_state_{
            low_ir_, sem_ir_, module_, context_, functions_, callback_thunks_,
            [this](const ForeignAbiFunctionLayout &layout) {
              return foreignFunctionType(layout);
            },
            [this](const ForeignAbiValueLayout &layout) {
              const auto values = foreignPhysicalTypes(layout);
              return std::vector<llvm::Type *>(values.begin(), values.end());
            },
            [this](llvm::Function &function,
                   const ForeignAbiFunctionLayout &layout) {
              applyForeignAttributes(function, layout);
            },
            [this](TypeId type) { return lowerObjectType(type); },
            [this](llvm::Function &function, llvm::Type *type,
                   std::string_view name) {
              return entryAlloca(function, type, name);
            },
            [this](llvm::Value *destination, llvm::Value *source, TypeId type,
                   llvm::IRBuilder<> &builder) {
              copyObject(destination, source, type, builder);
            },
            [this](llvm::Value *source, TypeId type,
                   llvm::IRBuilder<> &builder) {
              return loadValueFromObject(source, type, builder);
            }},
        callback_state_{
            low_ir_, sem_ir_, module_, context_, functions_,
            [this](LowInstId id) { return value(id); },
            [this](TypeId type) { return lowerValueType(type); },
            [this](const ForeignAbiFunctionLayout &layout) {
              return foreignFunctionType(layout);
            },
            [this](llvm::CallBase &call,
                   const ForeignAbiFunctionLayout &layout) {
              applyForeignAttributes(call, layout);
            },
            [this](ForeignAbiThunkPlanId plan) { return callbackThunk(plan); },
            [this](const ForeignAbiFunctionLayout &layout,
                   const ForeignAbiCallLayout &call_layout,
                   llvm::Value *callee,
                   std::span<llvm::Value *const> arguments,
                   llvm::IRBuilder<> &builder, llvm::Function &function) {
              return emitForeignCallValues(layout, call_layout, callee,
                                           arguments, builder, function);
            }},
        callback_wake_state_{callback_state_, coroutine_state_},
        cleanup_state_{
            low_ir_, sem_ir_, module_, context_, functions_, blocks_, slots_,
            place_flags_, instruction_error_,
            [this](LowInstId id) { return value(id); },
            [this](LowPlaceId id, llvm::IRBuilder<> &builder) {
              return placeAddress(id, builder);
            },
            [this](TypeId id) { return lowerObjectType(id); },
            [this](TypeId id) { return lowerValueType(id); },
            [this](TypeId id) { return usesPointerValueRepresentation(id); },
            [this]() { return isCurrentRepresentationPack(); },
            [this](llvm::Function &function, llvm::Type *type,
                   llvm::StringRef name) {
              return entryAlloca(function, type, name);
            },
            [this](llvm::Value *address, llvm::Value *source, TypeId type,
                   llvm::IRBuilder<> &builder) {
              storeValueToObject(address, source, type, builder);
            },
            [this](LowPlaceId place, llvm::IRBuilder<> &builder) {
              markMoved(place, builder);
            },
            [this](LowPlaceId place, llvm::IRBuilder<> &builder) {
              markInitialized(place, builder);
            },
            [this](llvm::Value *adapter, TypeId type,
                   llvm::IRBuilder<> &builder) {
              emitCallbackAdapterRelease(adapter, type, builder);
            },
            [this](TypeId type, llvm::Value *address,
                   llvm::IRBuilder<> &builder, llvm::Function &function) {
              return lowerVecDropAddress(type, address, builder, function);
            },
            [this](TypeId type, llvm::Value *address,
                   llvm::IRBuilder<> &builder, llvm::Function &function) {
              return lowerContainerDropAddress(type, address, builder, function);
            },
            current_function_, current_result_slot_, current_outcome_slot_},
        coroutine_instruction_state_{
            module_, context_, coroutine_state_, instruction_error_,
            [this](LowInstId id) { return value(id); },
            [this](llvm::IRBuilder<> &builder, llvm::Function &function) {
              return coroutineCancellationTarget(builder, function);
            }},
        coroutine_task_create_state_{
            low_ir_, sem_ir_, module_, context_, coroutine_state_,
            coroutine_constructors_,
            [this](LowInstId id) { return value(id); },
            [this](llvm::Function &function, llvm::Type *type,
                   llvm::StringRef name) {
              return entryAlloca(function, type, name);
            },
            [this](TypeId type) { return lowerValueType(type); },
            [this](llvm::Value *status, llvm::Value *storage, TypeId type,
                   llvm::IRBuilder<> &builder) {
              return makeCoroutineChecked(status, storage, type, builder);
            },
            [](const PublicEntity &entity) {
              return LLVMObjectIdentityService::coroutineConstructorSymbol(
                  entity);
            }},
        coroutine_task_group_state_{
            low_ir_, sem_ir_, module_, context_, coroutine_state_,
            [this](LowInstId id) { return value(id); },
            [this](llvm::Function &function, llvm::Type *type,
                   llvm::StringRef name) {
              return entryAlloca(function, type, name);
            },
            [this](std::uint32_t reason, llvm::IRBuilder<> &builder) {
              emitCoroutineProtocolTrap(reason, builder);
            }},
        coroutine_completion_state_{
            coroutine_task_group_state_,
            [this](llvm::Value *status, llvm::Value *storage, TypeId type,
                   llvm::IRBuilder<> &builder) {
              return makeCoroutineChecked(status, storage, type, builder);
            }},
        coroutine_completion_set_state_{
            low_ir_, sem_ir_, module_, context_, coroutine_state_,
            callback_wake_state_,
            [this](LowInstId id) { return value(id); },
            [this](llvm::Function &function, llvm::Type *type,
                   llvm::StringRef name) {
              return entryAlloca(function, type, name);
            },
            [this](TypeId type) { return lowerValueType(type); },
            [this](TypeId type) { return lowerObjectType(type); },
            [this](llvm::Value *status, llvm::Value *storage, TypeId type,
                   llvm::IRBuilder<> &builder) {
              return makeCoroutineChecked(status, storage, type, builder);
            },
            [this](const CallbackWakePlan &plan, llvm::Value *completion,
                   llvm::IRBuilder<> &builder, llvm::Function &function) {
              emitCoroutineDetachCompletion(plan, completion, builder,
                                            function);
            }},
        coroutine_task_result_state_{
            sem_ir_, module_, context_,
            [this](LowInstId id) { return value(id); },
            [this](llvm::Function &function, llvm::Type *type,
                   llvm::StringRef name) {
              return entryAlloca(function, type, name);
            },
            [this](TypeId type) { return lowerObjectType(type); },
            [this](llvm::Value *storage, TypeId type,
                   llvm::IRBuilder<> &builder) {
              return loadValueFromObject(storage, type, builder);
            },
            [this](llvm::Value *status, llvm::Value *storage, TypeId type,
                   llvm::IRBuilder<> &builder) {
              return makeCoroutineChecked(status, storage, type, builder);
            }},
        coroutine_scaffold_state_{
            low_ir_, sem_ir_, module_, context_, functions_,
            coroutine_constructors_, values_, slots_, place_flags_,
            blocks_, local_function_refs_, initialized_slots_,
            coroutine_state_, current_result_slot_, current_outcome_slot_,
            current_function_, emission_role_, source_entry_,
            entry_candidate_count_,
            [this](LowInstId id) { return value(id); },
            [this](TypeId type) { return lowerValueType(type); },
            [this](TypeId type) { return lowerObjectType(type); },
            [this](llvm::Function &function, llvm::Type *type,
                   llvm::StringRef name) {
              return entryAlloca(function, type, name);
            },
            [this](llvm::Value *address, TypeId type,
                   llvm::IRBuilder<> &builder) {
              return loadValueFromObject(address, type, builder);
            },
            [this](llvm::Value *address, llvm::Value *source, TypeId type,
                   llvm::IRBuilder<> &builder) {
              storeValueToObject(address, source, type, builder);
            },
            [this](llvm::Value *owner, TypeId type, std::uint32_t variant,
                   std::uint32_t field, llvm::IRBuilder<> &builder) {
              return enumPayloadAddress(owner, type, variant, field, builder);
            },
            [this](llvm::Value *adapter, TypeId type,
                   llvm::IRBuilder<> &builder) {
              emitCallbackAdapterRelease(adapter, type, builder);
            },
            [this](CallbackCompletionPlanId plan, llvm::Value *completion,
                   llvm::IRBuilder<> &builder, llvm::Function &function) {
              return finishCallbackCompletionValue(
                  plan, completion, builder, function);
            },
            [this](llvm::Value *adapter, const CallbackWakePlan &plan,
                   llvm::IRBuilder<> &builder) {
              emitWakeAdapterRelease(adapter, plan, builder);
            },
            [this](const CompletionProviderPlan &provider, llvm::Value *set,
                   std::uint32_t count, llvm::IRBuilder<> &builder,
                   llvm::Function &function) {
              armCompletionSet(provider, set, count, builder, function);
            },
            [this](const CompletionProviderPlan &provider, llvm::Value *set,
                   std::uint32_t count, llvm::IRBuilder<> &builder,
                   llvm::Function &function) {
              detachCompletionSet(provider, set, count, builder, function);
            },
            [this](const CompletionProviderPlan &provider, llvm::Value *set,
                   std::uint32_t count, llvm::IRBuilder<> &builder,
                   llvm::Function &function) {
              releaseCompletionSet(provider, set, count, builder, function);
            },
            [this](const CoroutineTaskCompletionCombinePlan &plan,
                   llvm::Value *set, llvm::IRBuilder<> &builder,
                   llvm::Function &function) {
              return probeCompletionSet(plan, set, builder, function);
            },
            [](std::span<const LowPlaceProjection> prefix,
               std::span<const LowPlaceProjection> path) {
              return pathPrefix(prefix, path);
            },
            [this](std::uint32_t bit, llvm::IRBuilder<> &builder) {
              return testCoroutineInitializationBit(bit, builder);
            },
            [this](SlotId slot, bool initialized,
                   llvm::IRBuilder<> &builder) {
              setSlotPlacesInitialized(slot, initialized, builder);
            },
            [this](LowInstId id, llvm::IRBuilder<> &builder,
                   llvm::Function &function) {
              lowerInst(id, builder, function);
            },
            [this](const ForeignAbiFunctionLayout &layout,
                   const ForeignAbiCallLayout &call_layout,
                   llvm::Value *callee,
                   std::span<llvm::Value *const> arguments,
                   llvm::IRBuilder<> &builder, llvm::Function &function) {
              return emitForeignCallValues(layout, call_layout, callee,
                                           arguments, builder, function);
            },
            [this](std::uint32_t reason, llvm::IRBuilder<> &builder) {
              emitCoroutineProtocolTrap(reason, builder);
            },
            [this](llvm::IRBuilder<> &builder, llvm::Function &function) {
              return coroutineCancellationTarget(builder, function);
            },
            [](const PublicEntity &entity) {
              return LLVMObjectIdentityService::coroutineConstructorSymbol(
                  entity);
            }} {
    for (const auto function : native_definition_exports)
      native_definition_exports_.insert(function);
    if (debug_info != DebugInfoMode::None && !sem_ir_.sourcePath().empty()) {
      debug_builder_ = std::make_unique<llvm::DIBuilder>(module_);
      const auto source_path =
          std::filesystem::path(std::string(sem_ir_.sourcePath()));
      debug_file_ = debug_builder_->createFile(
          source_path.filename().string(), source_path.parent_path().string());
      debug_builder_->createCompileUnit(
          llvm::dwarf::DW_LANG_C, debug_file_, "Chtholly", false, "", 0, "",
          debug_info == DebugInfoMode::LineTablesOnly
              ? llvm::DICompileUnit::DebugEmissionKind::LineTablesOnly
              : llvm::DICompileUnit::DebugEmissionKind::FullDebug);
      module_.addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                            llvm::DEBUG_METADATA_VERSION);
      if (llvm::Triple(module_.getTargetTriple()).isOSBinFormatCOFF())
        module_.addModuleFlag(llvm::Module::Warning, "CodeView", 1);
      else
        module_.addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);
    }
  }

  bool run(std::string &error) {
    LLVMModuleOrchestrationState state{
        low_ir_, sem_ir_, emission_role_, functions_, local_function_refs_,
        source_entry_, entry_candidate_count_,
        [this](FunctionId id) {
          return semanticFunction(semanticFunctionRef(id)).flags;
        },
        [this](std::string &registration_error) {
          LLVMAbiIdentityState abi_identity_state{sem_ir_, package_name_};
          LLVMFunctionRegistrationState registration_state{
              low_ir_,
              sem_ir_,
              module_,
              context_,
              package_name_,
              emission_role_,
              runtime_symbol_mappings_,
              native_definition_exports_,
              functions_,
              local_function_refs_,
              source_entry_,
              entry_candidate_count_,
              [this](const ForeignAbiValueLayout &layout) {
                const auto physical = foreignPhysicalTypes(layout);
                return std::vector<llvm::Type *>(physical.begin(), physical.end());
              },
              [this](TypeId type) { return lowerValueType(type); },
              [this](llvm::Function &function,
                     const ForeignAbiFunctionLayout &layout) {
                applyForeignAttributes(function, layout);
              },
              [&abi_identity_state](const SemFunctionRef &reference,
                                    std::string_view package, IdentifierId module,
                                    std::string_view name,
                                    bool canonical_semantic_target) {
                return LLVMAbiIdentityService::callableAbiSuffix(
                    reference, package, module, name, canonical_semantic_target,
                    abi_identity_state);
              },
              [&abi_identity_state](TypeId type) {
                return LLVMAbiIdentityService::concreteTypeSuffix(
                    type, abi_identity_state);
              }};
          if (!LLVMFunctionRegistrationService::registerAll(
                  registration_state, registration_error))
            return false;
          if (!lowering_error_.empty()) {
            registration_error = lowering_error_;
            return false;
          }
          return true;
        },
        [this](std::string &globals_error) {
          return emitStaticGlobals(globals_error);
        },
        [this](std::string &scaffold_error) {
          return emitCoroutineScaffolds(scaffold_error);
        },
        [this](LowFunctionId id, std::string &function_error) {
          return lowerFunction(id, function_error);
        },
        [this](llvm::Function &driver) { return emitTaskDriverHost(driver); },
        [this](llvm::Function &source) { emitEntryPoints(source); },
        [this](std::string &export_error) {
          return emitComponentExports(export_error);
        },
        [this]() {
          if (debug_builder_)
            debug_builder_->finalize();
        }};
    return LLVMModuleOrchestrationService::run(state, error);
  }
private:
  [[nodiscard]] const SemInst &semanticInst(SemIRInstRef ref) const {
    return sem_ir_.inst(ref.checked(sem_ir_));
  }
  [[nodiscard]] const SemType &semanticType(SemIRTypeRef ref) const {
    return sem_ir_.type(ref.checked(sem_ir_));
  }
  [[nodiscard]] const SemFunction &
  semanticFunction(SemIRFunctionRef ref) const {
    return sem_ir_.function(ref.checked(sem_ir_));
  }
  [[nodiscard]] const SemCallableSemanticContract &
  semanticFunctionContract(SemIRFunctionRef ref) const {
    return sem_ir_.functionSemanticContract(ref.checked(sem_ir_));
  }
  [[nodiscard]] SemIRInstRef semanticInstRef(InstId id) const {
    return makeSemIRRef(sem_ir_, id);
  }
  [[nodiscard]] SemIRTypeRef semanticTypeRef(TypeId id) const {
    return makeSemIRRef(sem_ir_, id);
  }
  [[nodiscard]] SemIRFunctionRef semanticFunctionRef(FunctionId id) const {
    return makeSemIRRef(sem_ir_, id);
  }

  bool emitComponentExports(std::string &error) {
    LLVMComponentExportState state{module_, context_, functions_, component_exports_};
    return LLVMComponentExportLoweringService::emit(state, error);
  }
  llvm::Function *emitTaskDriverHost(llvm::Function &driver) {
    return LLVMCoroutineScaffoldService::taskDriverHost(
        driver, coroutine_scaffold_state_);
  }

  void emitCoroutineDestroyAddress(TypeId type, llvm::Value *address,
                                   llvm::IRBuilder<> &builder,
                                   llvm::Function &function) {
    LLVMCoroutineScaffoldService::destroyAddress(
        type, address, builder, function, coroutine_scaffold_state_);
  }

  void emitCoroutineDetachCompletion(const CallbackWakePlan &plan,
                                     llvm::Value *completion,
                                     llvm::IRBuilder<> &builder,
                                     llvm::Function &function) {
    LLVMCoroutineScaffoldService::detachCompletion(
        plan, completion, builder, function, coroutine_scaffold_state_);
  }

  void emitCoroutineCleanupGraph(
      llvm::Function &function, llvm::StructType *frame_type,
      llvm::Value *frame, const CoroutineFramePlan &plan,
      CoroutineCleanupGraphId graph_id, llvm::BasicBlock *entry,
      llvm::BasicBlock *continuation, std::string_view prefix,
      bool preserve_result) {
    LLVMCoroutineScaffoldService::cleanupGraph(
        function, frame_type, frame, plan, graph_id, entry, continuation,
        prefix, preserve_result, coroutine_scaffold_state_);
  }

  bool emitCoroutineScaffolds(std::string &error) {
    return LLVMCoroutineScaffoldService::emit(
        error, coroutine_scaffold_state_);
  }
  llvm::Type *lowerForeignLane(const ForeignAbiLane &lane) const {
    return LLVMInteropLoweringService::lowerForeignLane(lane, context_);
  }

  llvm::SmallVector<llvm::Type *, 2>
  foreignPhysicalTypes(const ForeignAbiValueLayout &layout) {
    const auto values = LLVMInteropLoweringService::foreignPhysicalTypes(
        layout, context_, [this](TypeId type) { return lowerValueType(type); },
        [this](const ForeignAbiLane &lane) { return lowerForeignLane(lane); });
    return llvm::SmallVector<llvm::Type *, 2>(values.begin(), values.end());
  }

  llvm::FunctionType *
  foreignFunctionType(const ForeignAbiFunctionLayout &layout) {
    return LLVMInteropLoweringService::foreignFunctionType(
        layout, context_,
        [this](const ForeignAbiValueLayout &value_layout) {
          const auto values = foreignPhysicalTypes(value_layout);
          return std::vector<llvm::Type *>(values.begin(), values.end());
        },
        [this](TypeId type) { return lowerObjectType(type); });
  }

  void applyForeignAttributes(llvm::Function &function,
                              const ForeignAbiFunctionLayout &layout) {
    LLVMInteropLoweringService::applyForeignAttributes(
        function, layout, context_,
        [this](TypeId type) { return lowerObjectType(type); },
        [this](const ForeignAbiValueLayout &value_layout) {
          return foreignPhysicalTypes(value_layout).size();
        });
  }

  void applyForeignAttributes(llvm::CallBase &call,
                              const ForeignAbiFunctionLayout &layout) {
    LLVMInteropLoweringService::applyForeignAttributes(
        call, layout, context_,
        [this](TypeId type) { return lowerObjectType(type); },
        [this](const ForeignAbiValueLayout &value_layout) {
          return foreignPhysicalTypes(value_layout).size();
        });
  }

  llvm::Function *callbackThunk(ForeignAbiThunkPlanId plan_id) {
    return LLVMCallbackThunkService::lower(plan_id, callback_thunk_state_);
  }

  bool usesPointerValueRepresentation(TypeId id) const {
    return low_ir_.typeRepresentation(id).facts.value_repr ==
           ValueReprKind::Pointer;
  }

  [[nodiscard]] bool isConstructor(FunctionRefId target) const {
    const auto &reference = sem_ir_.functionRef(target);
    if (reference.local_function.hasValue())
      return semanticFunctionContract(
                 semanticFunctionRef(reference.local_function))
                 .role == CallableSemanticRole::Constructor;
    const auto *entity =
        sem_ir_.importIRs().tryGetEntity(reference.public_entity);
    return entity &&
           entity->semantic_contract.role == CallableSemanticRole::Constructor;
  }

  [[nodiscard]] std::optional<CanonicalResultShape>
  fallibleConstructorShape(FunctionRefId target) const {
    if (!isConstructor(target))
      return std::nullopt;
    const auto &function_type = semanticType(
        semanticTypeRef(sem_ir_.functionRef(target).local_type));
    return function_type.kind == SemTypeKind::Function
               ? sem_ir_.canonicalResultShape(TypeId(function_type.arg1))
               : std::nullopt;
  }

  [[nodiscard]] std::uint32_t currentHiddenResultCount() const {
    const auto function = low_ir_.function(current_function_).semantic_function;
    const auto &semantic_function =
        semanticFunction(semanticFunctionRef(function));
    const auto &semantic_type =
        semanticType(semanticTypeRef(semantic_function.type));
    const auto role =
        semanticFunctionContract(semanticFunctionRef(function)).role;
    if (role == CallableSemanticRole::Constructor &&
        sem_ir_.canonicalResultShape(TypeId(semantic_type.arg1)))
      return 2;
    if (role != CallableSemanticRole::Pack &&
        low_ir_.typeRepresentation(TypeId(semantic_type.arg1))
                .facts.init_repr == InitReprKind::InPlace)
      return 1;
    return 0;
  }

  bool isCurrentRepresentationPack() const {
    return semanticFunctionContract(semanticFunctionRef(
               low_ir_.function(current_function_).semantic_function))
               .role == CallableSemanticRole::Pack;
  }

  llvm::Type *lowerObjectType(TypeId id) {
    return LLVMObjectLoweringService::lowerObjectType(
        id, low_ir_, sem_ir_, module_, context_, types_, lowering_error_,
        [this](TypeId type) { return lowerObjectType(type); });
  }

  std::uint64_t enumPayloadFieldOffset(TypeId owner, std::uint32_t variant,
                                       std::uint32_t field) {
    const auto *layout = low_ir_.enumLayout(owner);
    assert(layout && variant < layout->variants.size() &&
           field < layout->variants[variant].field_offsets.size());
    return layout->variants[variant].field_offsets[field];
  }

  llvm::Value *enumPayloadAddress(llvm::Value *owner_address, TypeId owner,
                                  std::uint32_t variant, std::uint32_t field,
                                  llvm::IRBuilder<> &builder) {
    return LLVMObjectLoweringService::enumPayloadAddress(
        owner_address, owner, variant, field, builder, low_ir_, context_,
        [this](TypeId type) { return lowerObjectType(type); });
  }

  void copySemanticObject(llvm::Value *destination, llvm::Value *source,
                          TypeId type, llvm::IRBuilder<> &builder,
                          llvm::Function &function);

  llvm::Type *lowerValueType(TypeId id) {
    return LLVMValueLoweringService::valueType(
        low_ir_, id, context_, [this](TypeId type) { return lowerObjectType(type); },
        [this](TypeId type) { return lowerValueType(type); });
  }

  llvm::AllocaInst *entryAlloca(llvm::Function &function, llvm::Type *type,
                                std::string_view name) {
    llvm::IRBuilder<> entry(&function.getEntryBlock(),
                            function.getEntryBlock().begin());
    return entry.CreateAlloca(type, nullptr, llvm::StringRef(name));
  }

  llvm::Value *stringConstant(StringLiteralId id, llvm::IRBuilder<> &builder) {
    return LLVMValueLoweringService::stringConstant(
        id, sem_ir_, module_, context_, builder,
        [this](TypeId type) { return lowerObjectType(type); });
  }

  llvm::Value *cstringConstant(StringLiteralId id) {
    return LLVMValueLoweringService::cstringConstant(id, sem_ir_, module_,
                                                     context_);
  }

  llvm::Constant *constantObject(ConstantId id, std::string &error);

  bool emitStaticGlobals(std::string &error) {
    LLVMStaticGlobalState state{
        sem_ir_, module_, package_name_, static_globals_,
        [this](TypeId type) { return lowerObjectType(type); },
        [this](ConstantId id, std::string &constant_error) {
          return constantObject(id, constant_error);
        },
        [](std::string_view package, std::string_view module,
           std::string_view name) {
          return LLVMObjectIdentityService::mangleStatic(package, module, name);
        }};
    if (!LLVMStaticGlobalLoweringService::emit(state, error))
      return false;
    if (!lowering_error_.empty()) {
      error = lowering_error_;
      return false;
    }
    return true;
  }

  llvm::DIType *debugType(TypeId id) {
    if (!debug_builder_)
      return nullptr;
    LLVMDebugTypeState state{
        sem_ir_, low_ir_, module_, *debug_builder_, debug_file_, debug_types_,
        [this](TypeId type) { return debugType(type); }};
    return LLVMDebugTypeLoweringService::lower(id, state);
  }

  bool lowerFunction(LowFunctionId id, std::string &error) {
    const auto &function = low_ir_.function(id);
    current_function_ = id;
    auto *llvm_function = functions_.at(
        local_function_refs_.at(function.semantic_function.index).index);
    blocks_.clear();
    slots_.clear();
    values_.clear();
    typed_tokens_.clear();
    typed_payloads_.clear();
    initialized_slots_.clear();
    place_flags_.clear();
    LLVMFunctionSetupState setup_state{
        low_ir_, sem_ir_, context_, debug_builder_.get(), debug_file_,
        debug_info_, blocks_, slots_, place_flags_, initialized_slots_,
        current_result_slot_, current_outcome_slot_,
        [this](llvm::Function &fn, llvm::Type *type, std::string_view name) {
          return entryAlloca(fn, type, name);
        },
        [this](TypeId type) { return lowerObjectType(type); },
        [this](TypeId type) { return debugType(type); }};
    auto setup = LLVMFunctionSetupService::prepare(id, *llvm_function,
                                                   setup_state, error);
    if (!setup)
      return false;
    auto *debug_subprogram = setup->debug_subprogram;
    const auto debug_location = setup->debug_location;
    LLVMFunctionBodyState body_state{
        low_ir_, sem_ir_, context_, blocks_, instruction_error_,
        [this](LowInstId inst_id, llvm::IRBuilder<> &builder,
               llvm::Function &function) {
          lowerInst(inst_id, builder, function);
        }};
    return LLVMFunctionBodyService::lower(
        id, *llvm_function, debug_subprogram, debug_location, body_state,
        error);
  }

  void emitEntryPoints(llvm::Function &source) {
    LLVMEntryPointState state{module_, context_};
    LLVMEntryPointLoweringService::emitAll(source, state);
  }

#include "LLVMModuleLoweringValue.inc"
#include "LLVMModuleLoweringObject.inc"
#include "LLVMModuleLoweringInterop.inc"
#include "LLVMModuleLoweringCoroutine.inc"
#include "LLVMModuleLoweringCleanup.inc"

  void lowerInst(LowInstId id, llvm::IRBuilder<> &builder,
                 llvm::Function &function);

  const LowIR &low_ir_;
  const SemIR &sem_ir_;
  llvm::Module &module_;
  llvm::LLVMContext &context_;
  std::string_view package_name_;
  ModuleEmissionRole emission_role_ = ModuleEmissionRole::Library;
  std::unordered_set<std::uint32_t> native_definition_exports_;
  std::span<const std::pair<std::string, std::string>> runtime_symbol_mappings_;
  std::unique_ptr<llvm::DIBuilder> debug_builder_;
  llvm::DIFile *debug_file_ = nullptr;
  DebugInfoMode debug_info_ = DebugInfoMode::None;
  std::span<const ComponentExportLoweringPlan> component_exports_;
  std::unordered_map<std::uint32_t, llvm::DIType *> debug_types_;
  std::unordered_map<std::uint32_t, llvm::Type *> types_;
  std::string lowering_error_;
  std::string instruction_error_;
  std::unordered_map<std::uint32_t, llvm::Function *> functions_;
  std::unordered_map<std::uint32_t, llvm::GlobalVariable *> static_globals_;
  std::unordered_map<std::uint32_t, FunctionRefId> local_function_refs_;
  std::unordered_map<std::uint32_t, llvm::Function *> coroutine_constructors_;
  std::unordered_map<std::uint64_t, llvm::Function *> callback_thunks_;
  std::unordered_map<std::string, llvm::GlobalVariable *>
      typed_channel_descriptors_;
  std::unordered_map<std::string,
                     std::pair<llvm::Function *, llvm::Function *>>
      typed_channel_thunks_;
  std::unordered_map<std::uint32_t, llvm::BasicBlock *> blocks_;
  std::unordered_map<std::uint32_t, llvm::Value *> slots_;
  std::unordered_map<std::uint32_t, llvm::Value *> place_flags_;
  std::unordered_map<std::uint32_t, llvm::Value *> values_;
  std::unordered_map<std::uint32_t, llvm::Value *> typed_tokens_;
  std::unordered_map<std::uint32_t, TypeId> typed_payloads_;
  std::unordered_set<std::uint32_t> initialized_slots_;
  LowFunctionId current_function_;
  llvm::Value *current_result_slot_ = nullptr;
  llvm::Value *current_outcome_slot_ = nullptr;
  LLVMCoroutineState coroutine_state_;
  llvm::Function *source_entry_ = nullptr;
  std::uint32_t entry_candidate_count_ = 0;
  LLVMTypeState type_state_;
  LLVMValueState value_state_;
  LLVMObjectState object_state_;
  LLVMObjectValueState object_value_state_;
  LLVMIntrinsicState intrinsic_state_;
  LLVMScalarInstructionState scalar_instruction_state_;
  LLVMObjectInstructionState object_instruction_state_;
  LLVMInteropState interop_state_;
  LLVMForeignCallState foreign_call_state_;
  LLVMForeignResultState foreign_result_state_;
  LLVMForeignValueState foreign_value_state_;
  LLVMCallbackThunkState callback_thunk_state_;
  LLVMCallbackState callback_state_;
  LLVMCallbackWakeState callback_wake_state_;
  LLVMCleanupState cleanup_state_;
  LLVMCoroutineInstructionState coroutine_instruction_state_;
  LLVMCoroutineTaskCreateState coroutine_task_create_state_;
  LLVMCoroutineTaskGroupState coroutine_task_group_state_;
  LLVMCoroutineCompletionState coroutine_completion_state_;
  LLVMCoroutineCompletionSetState coroutine_completion_set_state_;
  LLVMCoroutineTaskResultState coroutine_task_result_state_;
  LLVMCoroutineScaffoldState coroutine_scaffold_state_;
};

} // namespace

void ModuleLoweringContext::copySemanticObject(llvm::Value *destination,
                                               llvm::Value *source, TypeId type,
                                               llvm::IRBuilder<> &builder,
                                               llvm::Function &function) {
  LLVMObjectLoweringService::copySemanticObject(
      destination, source, type, builder, function, sem_ir_, low_ir_, context_,
      functions_,
      [this](TypeId id) { return lowerObjectType(id); },
      [this](llvm::Value *dst, llvm::Value *src, TypeId id,
             llvm::IRBuilder<> &ir_builder) {
        copyObject(dst, src, id, ir_builder);
      },
      [this](llvm::Value *address, TypeId owner, std::uint32_t variant,
             std::uint32_t field, llvm::IRBuilder<> &ir_builder) {
        return enumPayloadAddress(address, owner, variant, field, ir_builder);
      },
      [this](llvm::Value *dst, llvm::Value *src, TypeId id,
             llvm::IRBuilder<> &ir_builder, llvm::Function &ir_function) {
        copySemanticObject(dst, src, id, ir_builder, ir_function);
      });
}

llvm::Constant *ModuleLoweringContext::constantObject(ConstantId id,
                                                      std::string &error) {
  return LLVMObjectLoweringService::constantObject(
      id, error, sem_ir_, module_, context_,
      [this](TypeId type) { return lowerObjectType(type); },
      [this](ConstantId nested, std::string &nested_error) {
        return constantObject(nested, nested_error);
      });
}

void ModuleLoweringContext::lowerInst(LowInstId id,
                                      llvm::IRBuilder<> &builder,
                                      llvm::Function &function) {
  llvm::Value *result = nullptr;
  coroutine_state_.low_instruction = id;
  visitLowInst(low_ir_.inst(id), [&](auto typed) {
    result = lowerInst(typed, builder, function);
  });
  if (result)
    value_state_.values.emplace(id.index, result);
}


bool internal::lowerLLVMModule(
    const LowIR &low_ir, std::string_view package_name,
    ModuleEmissionRole emission_role, llvm::Module &module,
    std::span<const std::uint32_t> native_definition_exports,
    std::span<const std::pair<std::string, std::string>> runtime_symbol_mappings,
    DebugInfoMode debug_info,
    std::span<const ComponentExportLoweringPlan> component_exports,
    std::string &error) {
  ModuleLoweringContext lowering(low_ir, package_name, emission_role, module,
                                 native_definition_exports,
                                 runtime_symbol_mappings, debug_info,
                                 component_exports);
  return lowering.run(error);
}

} // namespace chtholly::compiler
