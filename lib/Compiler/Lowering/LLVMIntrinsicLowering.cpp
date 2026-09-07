#include "LLVMInternal.h"

#include "chtholly/Basic/LanguageVersion.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>

namespace chtholly::compiler {
namespace {

class IntrinsicEmitter {
public:
  explicit IntrinsicEmitter(LLVMIntrinsicState &state) : state_(state) {}
  [[nodiscard]] std::optional<std::uint8_t>
  compilerIntrinsicOrder(LowInstId operand) const {
    if (!operand.hasValue() || operand.index >= state_.low_ir.instCount())
      return std::nullopt;
    const auto &lowered = state_.low_ir.inst(operand);
    if (lowered.kind != LowInstKind::MakeEnum ||
        lowered.arg1 >= state_.sem_ir.integerCount())
      return std::nullopt;
    const auto lowered_value = state_.sem_ir.integer(IntegerId(lowered.arg1));
    if (lowered_value < 0 || lowered_value > 4)
      return std::nullopt;

    const auto semantic_id = state_.low_ir.origin(operand);
    if (!semantic_id.hasValue() || semantic_id.index >= state_.sem_ir.instCount())
      return std::nullopt;
    const auto &semantic = state_.sem_ir.inst(semantic_id);
    const auto semantic_value = [&]() -> std::optional<std::int64_t> {
      if (semantic.kind == SemInstKind::EnumInit)
        return state_.sem_ir.integer(IntegerId(semantic.arg1));
      if (semantic.kind != SemInstKind::ConstantRef)
        return std::nullopt;
      const auto &entity =
          state_.sem_ir.constantEntity(ConstantEntityId(semantic.arg0));
      if (!entity.result.isConcrete())
        return std::nullopt;
      const auto &constant = state_.sem_ir.constantValue(entity.result.value);
      if (constant.kind != ConstantValueKind::Enum ||
          constant.payload >= state_.sem_ir.integerCount())
        return std::nullopt;
      return state_.sem_ir.integer(
          IntegerId(static_cast<std::uint32_t>(constant.payload)));
    }();
    return semantic_value && *semantic_value == lowered_value
               ? std::optional(static_cast<std::uint8_t>(lowered_value))
               : std::nullopt;
  }

  [[nodiscard]] static llvm::AtomicOrdering
  llvmAtomicOrdering(std::uint8_t order) {
    switch (order) {
    case 0:
      return llvm::AtomicOrdering::Monotonic;
    case 1:
      return llvm::AtomicOrdering::Acquire;
    case 2:
      return llvm::AtomicOrdering::Release;
    case 3:
      return llvm::AtomicOrdering::AcquireRelease;
    case 4:
      return llvm::AtomicOrdering::SequentiallyConsistent;
    default:
      llvm_unreachable("invalid verified atomic ordering");
    }
  }

  llvm::Value *lowerVecOrOptionIntrinsic(CompilerIntrinsicRole role,
                                         LowCompilerIntrinsicCall inst,
                                         std::span<const LowInstId> operands,
                                         llvm::IRBuilder<> &builder,
                                         llvm::Function &function) {
    assert(isVecCompilerIntrinsic(role) || isOptionCompilerIntrinsic(role));

    const auto trap_if = [&](llvm::Value *condition, std::uint32_t reason,
                             std::string_view label) {
      auto *failed = llvm::BasicBlock::Create(
          state_.context, std::string(label) + ".trap", &function);
      auto *valid = llvm::BasicBlock::Create(
          state_.context, std::string(label) + ".valid", &function);
      builder.CreateCondBr(condition, failed, valid);
      builder.SetInsertPoint(failed);
      builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_runtime_v1_trap_failure",
              llvm::FunctionType::get(builder.getVoidTy(),
                                      {builder.getInt32Ty()}, false)),
          {builder.getInt32(reason)});
      builder.CreateUnreachable();
      builder.SetInsertPoint(valid);
    };
    const auto checked_add = [&](llvm::Value *left, llvm::Value *right,
                                 std::string_view label) {
      auto *operation = llvm::Intrinsic::getDeclaration(
          &state_.module, llvm::Intrinsic::uadd_with_overflow,
          {builder.getInt64Ty()});
      auto *result =
          builder.CreateCall(operation, {left, right}, std::string(label));
      trap_if(builder.CreateExtractValue(result, 1), 2,
              std::string(label) + ".overflow");
      return builder.CreateExtractValue(result, 0,
                                        std::string(label) + ".value");
    };
    const auto checked_mul = [&](llvm::Value *left, llvm::Value *right,
                                 std::string_view label) {
      auto *operation = llvm::Intrinsic::getDeclaration(
          &state_.module, llvm::Intrinsic::umul_with_overflow,
          {builder.getInt64Ty()});
      auto *result =
          builder.CreateCall(operation, {left, right}, std::string(label));
      trap_if(builder.CreateExtractValue(result, 1), 2,
              std::string(label) + ".overflow");
      return builder.CreateExtractValue(result, 0,
                                        std::string(label) + ".value");
    };
    const auto referenced_type = [&](LowInstId operand) {
      const auto type = TypeId(state_.low_ir.inst(operand).type);
      return state_.sem_ir.type(type).kind == SemTypeKind::Reference
                 ? state_.sem_ir.referencePointee(type)
                 : TypeId::invalid();
    };
    const auto named_variant = [&](TypeId type,
                                   std::string_view name) -> std::uint32_t {
      if (!type.hasValue() || state_.sem_ir.type(type).kind != SemTypeKind::Nominal)
        return core::AnyId::InvalidIndex;
      const auto &nominal =
          state_.sem_ir.nominalType(NominalTypeId(state_.sem_ir.type(type).arg0));
      if (nominal.kind != NominalKind::Enum)
        return core::AnyId::InvalidIndex;
      for (std::uint32_t index = 0; index < nominal.variants.size(); ++index)
        if (state_.sem_ir.identifier(
                state_.sem_ir.name(nominal.variants[index].name).text) == name)
          return index;
      return core::AnyId::InvalidIndex;
    };
    const auto validate_option = [&](TypeId option_type,
                                     TypeId expected_payload) {
      const auto some = named_variant(option_type, "Some");
      const auto none = named_variant(option_type, "None");
      if (some == core::AnyId::InvalidIndex ||
          none == core::AnyId::InvalidIndex)
        return std::pair{core::AnyId::InvalidIndex, core::AnyId::InvalidIndex};
      const auto &nominal =
          state_.sem_ir.nominalType(NominalTypeId(state_.sem_ir.type(option_type).arg0));
      if (nominal.variants[some].fields.size() != 1 ||
          !nominal.variants[none].fields.empty() ||
          (expected_payload.hasValue() &&
           state_.sem_ir.enumPayloadFieldType(option_type, some, 0) !=
               expected_payload))
        return std::pair{core::AnyId::InvalidIndex, core::AnyId::InvalidIndex};
      return std::pair{some, none};
    };

    if (isOptionCompilerIntrinsic(role)) {
      if (operands.size() != 1) {
        state_.instruction_error = "Option intrinsic has invalid operands";
        return nullptr;
      }
      const auto operand_type = TypeId(state_.low_ir.inst(operands[0]).type);
      const auto option_type =
          state_.sem_ir.type(operand_type).kind == SemTypeKind::Reference
              ? state_.sem_ir.referencePointee(operand_type)
              : operand_type;
      const auto variants = validate_option(option_type, TypeId::invalid());
      if (variants.first == core::AnyId::InvalidIndex) {
        state_.instruction_error = "Option intrinsic has an invalid enum shape";
        return nullptr;
      }
      auto *record = llvm::cast<llvm::StructType>(lowerObjectType(option_type));
      auto *option = value(operands[0]);
      if (role == CompilerIntrinsicRole::OptionAsRef ||
          role == CompilerIntrinsicRole::OptionAsMut) {
        const auto result_option_type = inst.type;
        const auto result_payload = [&] {
          const auto result_some = variants.first;
          return state_.sem_ir.enumPayloadFieldType(result_option_type,
                                                     result_some, 0);
        }();
        const auto source_payload =
            state_.sem_ir.enumPayloadFieldType(option_type, variants.first, 0);
        if (state_.sem_ir.type(result_payload).kind != SemTypeKind::Reference ||
            state_.sem_ir.referencePointee(result_payload) != source_payload) {
          state_.instruction_error =
              "Option projection payload does not preserve provenance";
          return nullptr;
        }
        auto *result = entryAlloca(function, lowerObjectType(result_option_type),
                                   "option.projection.result");
        builder.CreateLifetimeStart(result);
        auto *source_tag = builder.CreateLoad(
            builder.getInt32Ty(), builder.CreateStructGEP(record, option, 0),
            "option.projection.tag");
        auto *some = llvm::BasicBlock::Create(state_.context,
                                              "option.projection.some", &function);
        auto *none = llvm::BasicBlock::Create(state_.context,
                                              "option.projection.none", &function);
        auto *done = llvm::BasicBlock::Create(state_.context,
                                              "option.projection.done", &function);
        builder.CreateCondBr(builder.CreateICmpEQ(source_tag,
                                                  builder.getInt32(variants.first)),
                             some, none);
        auto *result_record = llvm::cast<llvm::StructType>(
            lowerObjectType(result_option_type));
        builder.SetInsertPoint(some);
        builder.CreateStore(builder.getInt32(variants.first),
                            builder.CreateStructGEP(result_record, result, 0));
        builder.CreateStore(
            enumPayloadAddress(option, option_type, variants.first, 0, builder),
            enumPayloadAddress(result, result_option_type, variants.first, 0,
                               builder));
        builder.CreateBr(done);
        builder.SetInsertPoint(none);
        builder.CreateStore(builder.getInt32(variants.second),
                            builder.CreateStructGEP(result_record, result, 0));
        builder.CreateBr(done);
        builder.SetInsertPoint(done);
        return result;
      }
      auto *tag = builder.CreateLoad(builder.getInt32Ty(),
                                     builder.CreateStructGEP(record, option, 0),
                                     "option.tag");
      auto *is_some =
          builder.CreateICmpEQ(tag, builder.getInt32(variants.first));
      if (role == CompilerIntrinsicRole::OptionIsSome)
        return is_some;
      if (role == CompilerIntrinsicRole::OptionIsNone)
        return builder.CreateICmpEQ(tag, builder.getInt32(variants.second));
      trap_if(builder.CreateNot(is_some), 1, "option.unwrap.none");
      const auto payload_type =
          state_.sem_ir.enumPayloadFieldType(option_type, variants.first, 0);
      return loadValueFromObject(
          enumPayloadAddress(option, option_type, variants.first, 0, builder),
          payload_type, builder);
    }

    TypeId vec_type = TypeId::invalid();
    if (role == CompilerIntrinsicRole::VecInit) {
      vec_type = inst.type;
    } else if ((role == CompilerIntrinsicRole::VecIterNext ||
                role == CompilerIntrinsicRole::VecIterMutNext) &&
               !operands.empty()) {
      const auto iterator_type = TypeId(state_.low_ir.inst(operands[0]).type);
      if (state_.sem_ir.type(iterator_type).kind == SemTypeKind::Nominal) {
        const auto &iterator_nominal = state_.sem_ir.nominalType(
            NominalTypeId(state_.sem_ir.type(iterator_type).arg0));
        if (!iterator_nominal.fields.empty() &&
            state_.sem_ir.type(state_.sem_ir.nominalFieldType(iterator_type, 0)).kind ==
                SemTypeKind::Reference)
          vec_type = state_.sem_ir.referencePointee(
              state_.sem_ir.nominalFieldType(iterator_type, 0));
      }
    } else {
      vec_type =
          operands.empty() ? TypeId::invalid() : referenced_type(operands[0]);
    }
    if (!vec_type.hasValue() ||
        state_.sem_ir.type(vec_type).kind != SemTypeKind::Nominal) {
      state_.instruction_error = "Vec intrinsic has no concrete Vec receiver";
      return nullptr;
    }
    const auto &vec_nominal =
        state_.sem_ir.nominalType(NominalTypeId(state_.sem_ir.type(vec_type).arg0));
    if (vec_nominal.kind != NominalKind::Struct ||
        vec_nominal.fields.size() != 3) {
      state_.instruction_error = "Vec intrinsic has an invalid object shape";
      return nullptr;
    }
    const auto pointer_type = state_.sem_ir.nominalFieldType(vec_type, 0);
    const auto length_type = state_.sem_ir.nominalFieldType(vec_type, 1);
    const auto capacity_type = state_.sem_ir.nominalFieldType(vec_type, 2);
    if (state_.sem_ir.type(pointer_type).kind != SemTypeKind::RawPointer ||
        state_.sem_ir.type(length_type).kind != SemTypeKind::Integer ||
        state_.sem_ir.type(length_type).arg0 != 64 ||
        state_.sem_ir.type(length_type).arg1 != 0 || capacity_type != length_type) {
      state_.instruction_error = "Vec intrinsic object layout is invalid";
      return nullptr;
    }
    const auto element_type = state_.sem_ir.rawPointerPointee(pointer_type);
    const auto &element_representation =
        state_.low_ir.typeRepresentation(element_type);
    if (element_representation.facts.move == MoveReprKind::Unavailable ||
        element_representation.facts.move == MoveReprKind::Dependent) {
      state_.instruction_error = "Vec element type is not movable";
      return nullptr;
    }

    auto *vec_record = llvm::cast<llvm::StructType>(lowerObjectType(vec_type));
    auto *vec =
        role == CompilerIntrinsicRole::VecInit ? nullptr : value(operands[0]);
    const auto field_address = [&](unsigned field, std::string_view name) {
      return builder.CreateStructGEP(vec_record, vec, field, llvm::Twine(name));
    };
    const auto data_address = [&] { return field_address(0, "vec.data.addr"); };
    const auto length_address = [&] {
      return field_address(1, "vec.length.addr");
    };
    const auto capacity_address = [&] {
      return field_address(2, "vec.capacity.addr");
    };
    const auto element_address = [&](llvm::Value *data, llvm::Value *index,
                                     std::string_view name) {
      return builder.CreateInBoundsGEP(lowerObjectType(element_type), data,
                                       index, llvm::Twine(name));
    };
    const auto element_layout =
        state_.module.getDataLayout().getTypeAllocSize(lowerObjectType(element_type));
    if (element_layout.isScalable()) {
      state_.instruction_error = "Vec element type has a scalable LLVM layout";
      return nullptr;
    }
    const auto element_stride =
        std::max<std::uint64_t>(1, element_layout.getFixedValue());
    const auto element_alignment = std::max<std::uint64_t>(
        state_.module.getDataLayout()
            .getABITypeAlign(lowerObjectType(element_type))
            .value(),
        state_.module.getDataLayout().getPointerSize());

    const auto emit_element_loop = [&](llvm::Value *begin, llvm::Value *end,
                                       std::string_view label,
                                       const auto &body) {
      auto *preheader = builder.GetInsertBlock();
      auto *test = llvm::BasicBlock::Create(
          state_.context, std::string(label) + ".test", &function);
      auto *step = llvm::BasicBlock::Create(
          state_.context, std::string(label) + ".step", &function);
      auto *done = llvm::BasicBlock::Create(
          state_.context, std::string(label) + ".done", &function);
      builder.CreateBr(test);
      builder.SetInsertPoint(test);
      auto *index = builder.CreatePHI(builder.getInt64Ty(), 2,
                                      std::string(label) + ".index");
      index->addIncoming(begin, preheader);
      builder.CreateCondBr(builder.CreateICmpULT(index, end), step, done);
      builder.SetInsertPoint(step);
      body(index);
      auto *next = builder.CreateAdd(index, builder.getInt64(1));
      auto *backedge = builder.GetInsertBlock();
      builder.CreateBr(test);
      index->addIncoming(next, backedge);
      builder.SetInsertPoint(done);
    };
    const auto emit_reserve = [&](llvm::Value *additional) {
      auto *old_data = builder.CreateLoad(builder.getPtrTy(), data_address(),
                                          "vec.old.data");
      auto *length = builder.CreateLoad(builder.getInt64Ty(), length_address(),
                                        "vec.length");
      auto *old_capacity = builder.CreateLoad(
          builder.getInt64Ty(), capacity_address(), "vec.old.capacity");
      auto *required = checked_add(length, additional, "vec.required");
      auto *grow =
          llvm::BasicBlock::Create(state_.context, "vec.reserve.grow", &function);
      auto *done =
          llvm::BasicBlock::Create(state_.context, "vec.reserve.done", &function);
      builder.CreateCondBr(builder.CreateICmpUGT(required, old_capacity), grow,
                           done);
      builder.SetInsertPoint(grow);
      auto *increment = builder.CreateSelect(
          builder.CreateICmpUGT(old_capacity, builder.getInt64(1)),
          builder.CreateLShr(old_capacity, builder.getInt64(1)),
          builder.getInt64(1));
      auto *growth_operation = llvm::Intrinsic::getDeclaration(
          &state_.module, llvm::Intrinsic::uadd_with_overflow,
          {builder.getInt64Ty()});
      auto *growth =
          builder.CreateCall(growth_operation, {old_capacity, increment});
      auto *grown = builder.CreateExtractValue(growth, 0);
      auto *growth_overflow = builder.CreateExtractValue(growth, 1);
      auto *candidate = builder.CreateSelect(
          growth_overflow, required,
          builder.CreateSelect(builder.CreateICmpUGT(grown, required), grown,
                               required));
      auto *new_capacity = builder.CreateSelect(
          builder.CreateICmpULT(candidate, builder.getInt64(4)),
          builder.getInt64(4), candidate, "vec.new.capacity");
      auto *bytes = checked_mul(new_capacity, builder.getInt64(element_stride),
                                "vec.allocation.bytes");
      auto allocate = state_.module.getOrInsertFunction(
          "chtholly_next_runtime_v1_allocate",
          llvm::FunctionType::get(builder.getPtrTy(),
                                  {builder.getInt64Ty(), builder.getInt64Ty()},
                                  false));
      auto *new_data = builder.CreateCall(
          allocate, {bytes, builder.getInt64(element_alignment)},
          "vec.new.data");
      trap_if(builder.CreateIsNull(new_data), 4, "vec.allocate");
      emit_element_loop(
          builder.getInt64(0), length, "vec.relocate", [&](llvm::Value *index) {
            moveSemanticObject(
                element_address(new_data, index, "vec.new.element"),
                element_address(old_data, index, "vec.old.element"),
                element_type, builder);
          });
      builder.CreateStore(new_data, data_address());
      builder.CreateStore(new_capacity, capacity_address());
      auto *old_bytes =
          builder.CreateMul(old_capacity, builder.getInt64(element_stride));
      builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_runtime_v1_deallocate",
              llvm::FunctionType::get(builder.getVoidTy(),
                                      {builder.getPtrTy(), builder.getInt64Ty(),
                                       builder.getInt64Ty()},
                                      false)),
          {old_data, old_bytes, builder.getInt64(element_alignment)});
      builder.CreateBr(done);
      builder.SetInsertPoint(done);
    };
    const auto make_option = [&](TypeId option_type, llvm::Value *source,
                                 bool some) -> llvm::Value * {
      const auto variants = validate_option(option_type, element_type);
      if (variants.first == core::AnyId::InvalidIndex) {
        state_.instruction_error = "Vec result is not Option<T>";
        return nullptr;
      }
      auto *storage =
          entryAlloca(function, lowerObjectType(option_type), "vec.option");
      builder.CreateLifetimeStart(storage);
      builder.CreateStore(
          llvm::Constant::getNullValue(lowerObjectType(option_type)), storage);
      auto *record = llvm::cast<llvm::StructType>(lowerObjectType(option_type));
      const auto variant = some ? variants.first : variants.second;
      builder.CreateStore(builder.getInt32(variant),
                          builder.CreateStructGEP(record, storage, 0));
      if (some)
        moveSemanticObject(
            enumPayloadAddress(storage, option_type, variant, 0, builder),
            source, element_type, builder);
      return storage;
    };

    const auto make_iterator = [&](TypeId iterator_type,
                                   llvm::Value *owner) -> llvm::Value * {
      auto *storage =
          entryAlloca(function, lowerObjectType(iterator_type), "vec.iterator");
      builder.CreateLifetimeStart(storage);
      auto *record =
          llvm::cast<llvm::StructType>(lowerObjectType(iterator_type));
      auto *owner_address =
          builder.CreateStructGEP(record, storage, 0, "iterator.owner.addr");
      // References are pointer-valued semantic values. Store the pointer
      // itself; treating it as an object address would copy from the Vec.
      builder.CreateStore(owner, owner_address);
      builder.CreateStore(
          builder.getInt64(0),
          builder.CreateStructGEP(record, storage, 1, "iterator.index.addr"));
      return storage;
    };
    switch (role) {
    case CompilerIntrinsicRole::VecInit: {
      if (!operands.empty()) {
        state_.instruction_error = "Vec init has invalid operands";
        return nullptr;
      }
      auto *storage =
          entryAlloca(function, lowerObjectType(vec_type), "vec.value");
      builder.CreateLifetimeStart(storage);
      builder.CreateStore(
          llvm::Constant::getNullValue(lowerObjectType(vec_type)), storage);
      return storage;
    }
    case CompilerIntrinsicRole::VecIter:
    case CompilerIntrinsicRole::VecIterMut:
      if (operands.size() != 1) {
        state_.instruction_error = "Vec iterator construction has invalid operands";
        return nullptr;
      }
      return make_iterator(inst.type, value(operands[0]));
    case CompilerIntrinsicRole::VecIterNext:
    case CompilerIntrinsicRole::VecIterMutNext: {
      if (operands.size() != 1) {
        state_.instruction_error = "Vec iterator next has invalid operands";
        return nullptr;
      }
      const auto iterator_type = TypeId(state_.low_ir.inst(operands[0]).type);
      const auto step_type = inst.type;
      const auto &step_nominal =
          state_.sem_ir.nominalType(NominalTypeId(state_.sem_ir.type(step_type).arg0));
      std::uint32_t item_variant = core::AnyId::InvalidIndex;
      std::uint32_t done_variant = core::AnyId::InvalidIndex;
      for (std::uint32_t variant = 0; variant < step_nominal.variants.size();
           ++variant) {
        const auto name = state_.sem_ir.identifier(
            state_.sem_ir.name(step_nominal.variants[variant].name).text);
        if (name == "Item")
          item_variant = variant;
        else if (name == "Done")
          done_variant = variant;
      }
      if (item_variant == core::AnyId::InvalidIndex ||
          done_variant == core::AnyId::InvalidIndex ||
          step_nominal.variants[item_variant].fields.size() != 2 ||
          !step_nominal.variants[done_variant].fields.empty()) {
        state_.instruction_error = "Vec iterator next has an invalid step shape";
        return nullptr;
      }
      const auto &iterator_nominal =
          state_.sem_ir.nominalType(NominalTypeId(state_.sem_ir.type(iterator_type).arg0));
      if (iterator_nominal.fields.size() != 2) {
        state_.instruction_error = "Vec iterator next has an invalid iterator shape";
        return nullptr;
      }
      auto *iterator = value(operands[0]);
      auto *iterator_record =
          llvm::cast<llvm::StructType>(lowerObjectType(iterator_type));
      auto *owner = builder.CreateLoad(
          builder.getPtrTy(),
          builder.CreateStructGEP(iterator_record, iterator, 0),
          "iterator.owner");
      auto *index = builder.CreateLoad(
          builder.getInt64Ty(),
          builder.CreateStructGEP(iterator_record, iterator, 1),
          "iterator.index");
      auto *length = builder.CreateLoad(
          builder.getInt64Ty(), builder.CreateStructGEP(vec_record, owner, 1),
          "iterator.length");
      auto *data = builder.CreateLoad(
          builder.getPtrTy(), builder.CreateStructGEP(vec_record, owner, 0),
          "iterator.data");
      auto *result =
          entryAlloca(function, lowerObjectType(step_type), "iterator.step");
      builder.CreateLifetimeStart(result);
      auto *item_block =
          llvm::BasicBlock::Create(state_.context, "iterator.item", &function);
      auto *done_block =
          llvm::BasicBlock::Create(state_.context, "iterator.done", &function);
      auto *merge_block =
          llvm::BasicBlock::Create(state_.context, "iterator.merge", &function);
      builder.CreateCondBr(builder.CreateICmpULT(index, length), item_block,
                           done_block);
      auto *step_record =
          llvm::cast<llvm::StructType>(lowerObjectType(step_type));
      builder.SetInsertPoint(item_block);
      builder.CreateStore(builder.getInt32(item_variant),
                          builder.CreateStructGEP(step_record, result, 0));
      auto *item_address =
          enumPayloadAddress(result, step_type, item_variant, 0, builder);
      auto *next_address =
          enumPayloadAddress(result, step_type, item_variant, 1, builder);
      auto *element = element_address(data, index, "iterator.element");
      builder.CreateStore(element, item_address);
      auto *next_iterator = make_iterator(iterator_type, owner);
      builder.CreateStore(
          builder.CreateAdd(index, builder.getInt64(1)),
          builder.CreateStructGEP(iterator_record, next_iterator, 1));
      copyObject(next_address, next_iterator, iterator_type, builder);
      builder.CreateBr(merge_block);
      builder.SetInsertPoint(done_block);
      builder.CreateStore(builder.getInt32(done_variant),
                          builder.CreateStructGEP(step_record, result, 0));
      builder.CreateBr(merge_block);
      builder.SetInsertPoint(merge_block);
      return result;
    }
    case CompilerIntrinsicRole::VecLen:
      return builder.CreateLoad(builder.getInt64Ty(), length_address(),
                                "vec.length");
    case CompilerIntrinsicRole::VecCapacity:
      return builder.CreateLoad(builder.getInt64Ty(), capacity_address(),
                                "vec.capacity");
    case CompilerIntrinsicRole::VecReserve:
      emit_reserve(value(operands[1]));
      return nullptr;
    case CompilerIntrinsicRole::VecPush: {
      emit_reserve(builder.getInt64(1));
      auto *data = builder.CreateLoad(builder.getPtrTy(), data_address());
      auto *length = builder.CreateLoad(builder.getInt64Ty(), length_address());
      moveSemanticValueToObject(
          element_address(data, length, "vec.push.element"), value(operands[1]),
          element_type, builder);
      builder.CreateStore(builder.CreateAdd(length, builder.getInt64(1)),
                          length_address());
      return nullptr;
    }
    case CompilerIntrinsicRole::VecAt:
    case CompilerIntrinsicRole::VecAtMut: {
      auto *index = value(operands[1]);
      auto *length = builder.CreateLoad(builder.getInt64Ty(), length_address());
      trap_if(builder.CreateICmpUGE(index, length), 1, "vec.index");
      auto *data = builder.CreateLoad(builder.getPtrTy(), data_address());
      return element_address(data, index, "vec.element");
    }
    case CompilerIntrinsicRole::VecPop: {
      auto *length = builder.CreateLoad(builder.getInt64Ty(), length_address());
      auto *empty =
          llvm::BasicBlock::Create(state_.context, "vec.pop.empty", &function);
      auto *some =
          llvm::BasicBlock::Create(state_.context, "vec.pop.some", &function);
      auto *done =
          llvm::BasicBlock::Create(state_.context, "vec.pop.done", &function);
      auto *result =
          entryAlloca(function, lowerObjectType(inst.type), "vec.pop.result");
      builder.CreateLifetimeStart(result);
      builder.CreateCondBr(builder.CreateICmpEQ(length, builder.getInt64(0)),
                           empty, some);
      builder.SetInsertPoint(empty);
      copyObject(result, make_option(inst.type, nullptr, false), inst.type,
                 builder);
      builder.CreateBr(done);
      builder.SetInsertPoint(some);
      auto *new_length = builder.CreateSub(length, builder.getInt64(1));
      auto *data = builder.CreateLoad(builder.getPtrTy(), data_address());
      builder.CreateStore(new_length, length_address());
      copyObject(
          result,
          make_option(inst.type,
                      element_address(data, new_length, "vec.pop.element"),
                      true),
          inst.type, builder);
      builder.CreateBr(done);
      builder.SetInsertPoint(done);
      return result;
    }
    case CompilerIntrinsicRole::VecRemove: {
      auto *index = value(operands[1]);
      auto *length = builder.CreateLoad(builder.getInt64Ty(), length_address());
      auto *none =
          llvm::BasicBlock::Create(state_.context, "vec.remove.none", &function);
      auto *some =
          llvm::BasicBlock::Create(state_.context, "vec.remove.some", &function);
      auto *done =
          llvm::BasicBlock::Create(state_.context, "vec.remove.done", &function);
      auto *result = entryAlloca(function, lowerObjectType(inst.type),
                                 "vec.remove.result");
      builder.CreateLifetimeStart(result);
      builder.CreateCondBr(builder.CreateICmpUGE(index, length), none, some);
      builder.SetInsertPoint(none);
      copyObject(result, make_option(inst.type, nullptr, false), inst.type,
                 builder);
      builder.CreateBr(done);
      builder.SetInsertPoint(some);
      auto *data = builder.CreateLoad(builder.getPtrTy(), data_address());
      copyObject(result,
                 make_option(inst.type,
                             element_address(data, index, "vec.remove.element"),
                             true),
                 inst.type, builder);
      auto *first_source = builder.CreateAdd(index, builder.getInt64(1));
      emit_element_loop(
          first_source, length, "vec.remove.shift",
          [&](llvm::Value *source_index) {
            auto *destination_index =
                builder.CreateSub(source_index, builder.getInt64(1));
            moveSemanticObject(
                element_address(data, destination_index,
                                "vec.remove.destination"),
                element_address(data, source_index, "vec.remove.source"),
                element_type, builder);
          });
      builder.CreateStore(builder.CreateSub(length, builder.getInt64(1)),
                          length_address());
      builder.CreateBr(done);
      builder.SetInsertPoint(done);
      return result;
    }
    case CompilerIntrinsicRole::VecClear:
    case CompilerIntrinsicRole::VecDrop: {
      auto *data = builder.CreateLoad(builder.getPtrTy(), data_address());
      auto *length = builder.CreateLoad(builder.getInt64Ty(), length_address());
      auto *preheader = builder.GetInsertBlock();
      auto *test =
          llvm::BasicBlock::Create(state_.context, "vec.destroy.test", &function);
      auto *step =
          llvm::BasicBlock::Create(state_.context, "vec.destroy.step", &function);
      auto *done =
          llvm::BasicBlock::Create(state_.context, "vec.destroy.done", &function);
      builder.CreateBr(test);
      builder.SetInsertPoint(test);
      auto *remaining =
          builder.CreatePHI(builder.getInt64Ty(), 2, "vec.destroy.remaining");
      remaining->addIncoming(length, preheader);
      builder.CreateCondBr(builder.CreateICmpEQ(remaining, builder.getInt64(0)),
                           done, step);
      builder.SetInsertPoint(step);
      auto *next = builder.CreateSub(remaining, builder.getInt64(1));
      builder.CreateStore(next, length_address());
      emitCoroutineDestroyAddress(
          element_type, element_address(data, next, "vec.destroy.element"),
          builder, function);
      auto *backedge = builder.GetInsertBlock();
      builder.CreateBr(test);
      remaining->addIncoming(next, backedge);
      builder.SetInsertPoint(done);
      if (role == CompilerIntrinsicRole::VecDrop) {
        auto *capacity =
            builder.CreateLoad(builder.getInt64Ty(), capacity_address());
        auto *bytes =
            builder.CreateMul(capacity, builder.getInt64(element_stride));
        builder.CreateCall(state_.module.getOrInsertFunction(
                               "chtholly_next_runtime_v1_deallocate",
                               llvm::FunctionType::get(builder.getVoidTy(),
                                                       {builder.getPtrTy(),
                                                        builder.getInt64Ty(),
                                                        builder.getInt64Ty()},
                                                       false)),
                           {data, bytes, builder.getInt64(element_alignment)});
        builder.CreateStore(llvm::ConstantPointerNull::get(builder.getPtrTy()),
                            data_address());
        builder.CreateStore(builder.getInt64(0), capacity_address());
      }
      return nullptr;
    }
    default:
      llvm_unreachable("non-Vec role reached Vec lowering");
    }
  }

  llvm::Value *lowerContainerIntrinsic(CompilerIntrinsicRole role,
                                       LowCompilerIntrinsicCall inst,
                                       std::span<const LowInstId> operands,
                                       llvm::IRBuilder<> &builder,
                                       llvm::Function &function) {
    const bool hash_map = isHashMapCompilerIntrinsic(role);
    auto container_type = inst.type;
    if (!operands.empty()) {
      container_type = TypeId(state_.low_ir.inst(operands[0]).type);
      if (state_.sem_ir.type(container_type).kind == SemTypeKind::Reference)
        container_type = state_.sem_ir.referencePointee(container_type);
    }
    const auto make = [&]() -> llvm::Value * {
      if (!operands.empty()) {
        state_.instruction_error = "container make has invalid operands";
        return nullptr;
      }
      auto *storage = entryAlloca(function, lowerObjectType(inst.type),
                                  hash_map ? "hashmap.value" : "hashset.value");
      builder.CreateLifetimeStart(storage);
      builder.CreateStore(
          llvm::Constant::getNullValue(lowerObjectType(inst.type)), storage);
      const auto arguments = state_.sem_ir.typeBlock(TypeBlockId(
          state_.sem_ir.type(container_type).arg1));
      if (state_.sem_ir.type(container_type).kind != SemTypeKind::Nominal ||
          (hash_map ? arguments.size() != 2 : arguments.size() != 1)) {
        state_.instruction_error = "container make has an invalid type";
        return nullptr;
      }
      const auto key_type = arguments.front();
      const auto value_type = hash_map ? arguments.back() : key_type;
      auto *vtable = containerVtable(container_type, key_type, value_type);
      if (!vtable)
        return nullptr;
      auto *status = entryAlloca(function, builder.getInt32Ty(),
                                 "container.status");
      auto validate = state_.module.getOrInsertFunction(
          "chtholly_next_container_v1_validate_vtable",
          llvm::FunctionType::get(builder.getInt32Ty(),
                                  {builder.getPtrTy(), builder.getInt32Ty(),
                                   builder.getInt32Ty()}, false));
      auto *descriptor_status = builder.CreateCall(
          validate,
          {vtable, builder.getInt32(CurrentSemanticArtifactEpoch),
           builder.getInt32(state_.module.getDataLayout().getPointerSizeInBits())},
          "container.vtable.status");
      auto *descriptor_failed = llvm::BasicBlock::Create(
          state_.context, "container.vtable.failed", &function);
      auto *descriptor_ok = llvm::BasicBlock::Create(
          state_.context, "container.vtable.ok", &function);
      builder.CreateCondBr(builder.CreateICmpNE(descriptor_status,
                                                builder.getInt32(0)),
                           descriptor_failed, descriptor_ok);
      builder.SetInsertPoint(descriptor_failed);
      builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_runtime_v1_trap_failure",
              llvm::FunctionType::get(builder.getVoidTy(),
                                      {builder.getInt32Ty()}, false)),
          {builder.getInt32(4)});
      builder.CreateUnreachable();
      builder.SetInsertPoint(descriptor_ok);
      auto create = state_.module.getOrInsertFunction(
          "chtholly_next_container_v1_create",
          llvm::FunctionType::get(builder.getPtrTy(),
                                  {builder.getPtrTy(), builder.getPtrTy(),
                                   builder.getInt64Ty(), builder.getPtrTy()},
                                  false));
      auto *table = builder.CreateCall(
          create, {vtable, llvm::ConstantPointerNull::get(builder.getPtrTy()),
                   builder.getInt64(0), status},
          "container.table");
      auto *status_value = builder.CreateLoad(builder.getInt32Ty(), status,
                                              "container.status.value");
      auto *failed = llvm::BasicBlock::Create(state_.context, "container.init.failed",
                                              &function);
      auto *done = llvm::BasicBlock::Create(state_.context, "container.init.done",
                                            &function);
      builder.CreateCondBr(builder.CreateICmpNE(status_value, builder.getInt32(0)),
                           failed, done);
      builder.SetInsertPoint(failed);
      builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_runtime_v1_trap_failure",
              llvm::FunctionType::get(builder.getVoidTy(),
                                      {builder.getInt32Ty()}, false)),
          {builder.getInt32(4)});
      builder.CreateUnreachable();
      builder.SetInsertPoint(done);
      const auto object_record =
          llvm::cast<llvm::StructType>(lowerObjectType(inst.type));
      builder.CreateStore(table, builder.CreateStructGEP(object_record, storage, 0));
      for (unsigned index = 1; index < object_record->getNumElements(); ++index)
        builder.CreateStore(builder.getInt64(0),
                            builder.CreateStructGEP(object_record, storage, index));
      return storage;
    };
    if (role == CompilerIntrinsicRole::HashMapMake ||
        role == CompilerIntrinsicRole::HashSetMake)
      return make();
    if (operands.empty()) {
      state_.instruction_error = "container intrinsic has no receiver";
      return nullptr;
    }
    auto *object = value(operands[0]);
    auto receiver_type = TypeId(state_.low_ir.inst(operands[0]).type);
    if (state_.sem_ir.type(receiver_type).kind == SemTypeKind::Reference)
      receiver_type = state_.sem_ir.referencePointee(receiver_type);
    const auto field = [&](unsigned index) {
      return builder.CreateStructGEP(lowerObjectType(receiver_type), object,
                                     index, "container.field");
    };
    const auto update_generation = [&](llvm::Value *table) {
      const unsigned generation_field = hash_map ? 5U : 4U;
      auto *generation = builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_container_v1_generation",
              llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy()},
                                      false)),
          {table}, "container.generation");
      builder.CreateStore(generation, field(generation_field));
    };
    if (role == CompilerIntrinsicRole::HashMapLen ||
        role == CompilerIntrinsicRole::HashSetLen)
      return builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_container_v1_size",
              llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy()},
                                      false)),
          {builder.CreateLoad(builder.getPtrTy(), field(0), "container.table")},
          "container.length");
    if (role == CompilerIntrinsicRole::HashMapCapacity ||
        role == CompilerIntrinsicRole::HashSetCapacity)
      return builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_container_v1_capacity",
              llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy()},
                                      false)),
          {builder.CreateLoad(builder.getPtrTy(), field(0), "container.table")},
          "container.capacity");
    if (role == CompilerIntrinsicRole::HashMapIsEmpty ||
        role == CompilerIntrinsicRole::HashSetIsEmpty) {
      auto *length = builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_container_v1_size",
              llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy()},
                                      false)),
          {builder.CreateLoad(builder.getPtrTy(), field(0), "container.table")},
          "container.length");
      return builder.CreateICmpEQ(length, builder.getInt64(0),
                                  "container.empty");
    }
    if (role == CompilerIntrinsicRole::HashMapContains ||
        role == CompilerIntrinsicRole::HashSetContains) {
      if (operands.size() != 2) {
        state_.instruction_error = "container contains has invalid operands";
        return nullptr;
      }
      auto *result_slot = entryAlloca(function, builder.getPtrTy(),
                                      "container.find.value");
      auto *status = builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_container_v1_find",
              llvm::FunctionType::get(builder.getInt32Ty(),
                                      {builder.getPtrTy(), builder.getPtrTy(),
                                       builder.getPtrTy()}, false)),
          {builder.CreateLoad(builder.getPtrTy(), field(0), "container.table"),
           value(operands[1]), result_slot},
          "container.find.status");
      return builder.CreateICmpEQ(status, builder.getInt32(0),
                                  "container.contains");
    }
    if (role == CompilerIntrinsicRole::HashMapGet ||
        role == CompilerIntrinsicRole::HashMapGetMut) {
      if (operands.size() != 2) {
        state_.instruction_error = "HashMap get has invalid operands";
        return nullptr;
      }
      const auto args = state_.sem_ir.typeBlock(TypeBlockId(
          state_.sem_ir.type(receiver_type).arg1));
      if (state_.sem_ir.type(receiver_type).kind != SemTypeKind::Nominal ||
          args.size() != 2) {
        state_.instruction_error = "HashMap get has an invalid type";
        return nullptr;
      }
      auto *value_slot = entryAlloca(function, builder.getPtrTy(),
                                     "container.get.value");
      auto *status = builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_container_v1_find",
              llvm::FunctionType::get(builder.getInt32Ty(),
                                      {builder.getPtrTy(), builder.getPtrTy(),
                                       builder.getPtrTy()}, false)),
          {builder.CreateLoad(builder.getPtrTy(), field(0), "container.table"),
           value(operands[1]), value_slot},
          "container.get.status");
      auto *borrow_table = builder.CreateLoad(builder.getPtrTy(), field(0),
                                              "container.borrow.table");
      auto *borrow_generation = builder.CreateLoad(
          builder.getInt64Ty(), field(hash_map ? 5U : 4U),
          "container.borrow.generation");
      auto *borrow_valid = builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_container_v1_borrow_is_valid",
              llvm::FunctionType::get(builder.getInt32Ty(),
                                      {builder.getPtrTy(), builder.getInt64Ty()},
                                      false)),
          {borrow_table, borrow_generation}, "container.borrow.valid");
      auto *invalid_status = builder.CreateICmpUGT(status, builder.getInt32(1));
      auto *invalid_borrow = builder.CreateICmpNE(borrow_valid, builder.getInt32(0));
      auto *borrow_trap = llvm::BasicBlock::Create(state_.context,
                                                   "container.get.borrow-trap", &function);
      auto *status_check = llvm::BasicBlock::Create(state_.context,
                                                    "container.get.status-check", &function);
      auto *status_trap = llvm::BasicBlock::Create(state_.context,
                                                   "container.get.trap", &function);
      auto *status_ok = llvm::BasicBlock::Create(state_.context,
                                                 "container.get.status-ok", &function);
      builder.CreateCondBr(invalid_borrow, borrow_trap, status_check);
      builder.SetInsertPoint(borrow_trap);
      builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_runtime_v1_trap_failure",
              llvm::FunctionType::get(builder.getVoidTy(),
                                      {builder.getInt32Ty()}, false)),
          {builder.getInt32(4)});
      builder.CreateUnreachable();
      builder.SetInsertPoint(status_check);
      builder.CreateCondBr(invalid_status, status_trap, status_ok);
      builder.SetInsertPoint(status_trap);
      builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_runtime_v1_trap_failure",
              llvm::FunctionType::get(builder.getVoidTy(),
                                      {builder.getInt32Ty()}, false)),
          {builder.getInt32(4)});
      builder.CreateUnreachable();
      builder.SetInsertPoint(status_ok);
      const auto option_type = inst.type;
      const auto &option_semantic = state_.sem_ir.type(option_type);
      if (option_semantic.kind != SemTypeKind::Nominal) {
        state_.instruction_error = "HashMap get result is not an Option";
        return nullptr;
      }
      const auto &option_nominal = state_.sem_ir.nominalType(
          NominalTypeId(option_semantic.arg0));
      std::uint32_t some_variant = core::AnyId::InvalidIndex;
      std::uint32_t none_variant = core::AnyId::InvalidIndex;
      for (std::uint32_t index = 0; index < option_nominal.variants.size(); ++index) {
        const auto variant_name = state_.sem_ir.identifier(state_.sem_ir.name(
            option_nominal.variants[index].name).text);
        if (variant_name == "Some") some_variant = index;
        if (variant_name == "None") none_variant = index;
      }
      if (some_variant == core::AnyId::InvalidIndex ||
          none_variant == core::AnyId::InvalidIndex) {
        state_.instruction_error = "HashMap get option has invalid variants";
        return nullptr;
      }
      auto *result_storage = entryAlloca(function, lowerObjectType(option_type),
                                          "container.get.result");
      builder.CreateLifetimeStart(result_storage);
      builder.CreateStore(llvm::Constant::getNullValue(lowerObjectType(option_type)),
                          result_storage);
      auto *some_block = llvm::BasicBlock::Create(state_.context,
                                                  "container.get.some", &function);
      auto *none_block = llvm::BasicBlock::Create(state_.context,
                                                  "container.get.none", &function);
      auto *merge_block = llvm::BasicBlock::Create(state_.context,
                                                   "container.get.done", &function);
      builder.CreateCondBr(builder.CreateICmpEQ(status, builder.getInt32(0)),
                           some_block, none_block);
      builder.SetInsertPoint(some_block);
      auto *record = llvm::cast<llvm::StructType>(lowerObjectType(option_type));
      builder.CreateStore(builder.getInt32(some_variant),
                          builder.CreateStructGEP(record, result_storage, 0));
      auto *payload = enumPayloadAddress(result_storage, option_type,
                                         some_variant, 0, builder);
      builder.CreateStore(builder.CreateLoad(builder.getPtrTy(), value_slot),
                          payload);
      builder.CreateBr(merge_block);
      builder.SetInsertPoint(none_block);
      builder.CreateStore(builder.getInt32(none_variant),
                          builder.CreateStructGEP(record, result_storage, 0));
      builder.CreateBr(merge_block);
      builder.SetInsertPoint(merge_block);
      return result_storage;
    }
    if (role == CompilerIntrinsicRole::HashMapInsert) {
      if (operands.size() != 3) {
        state_.instruction_error = "HashMap insert has invalid operands";
        return nullptr;
      }
      const auto args = state_.sem_ir.typeBlock(TypeBlockId(
          state_.sem_ir.type(receiver_type).arg1));
      if (state_.sem_ir.type(receiver_type).kind != SemTypeKind::Nominal ||
          args.size() != 2) {
        state_.instruction_error = "HashMap insert has an invalid type";
        return nullptr;
      }
      const auto key_type = args[0];
      const auto value_type = args[1];
      if (state_.sem_ir.typeRepresentation(key_type).value_repr ==
              ValueReprKind::None ||
          state_.sem_ir.typeRepresentation(value_type).value_repr ==
              ValueReprKind::None) {
        state_.instruction_error = "HashMap insert type has no value representation";
        return nullptr;
      }
      auto *key_storage = entryAlloca(function, lowerObjectType(key_type),
                                       "container.insert.key");
      auto *value_storage = entryAlloca(function, lowerObjectType(value_type),
                                         "container.insert.value");
      auto *replaced_storage =
          entryAlloca(function, lowerObjectType(value_type),
                      "container.insert.replaced");
      auto *replaced_flag =
          entryAlloca(function, builder.getInt8Ty(), "container.insert.flag");
      builder.CreateLifetimeStart(key_storage);
      builder.CreateLifetimeStart(value_storage);
      builder.CreateLifetimeStart(replaced_storage);
      builder.CreateStore(builder.getInt8(0), replaced_flag);
      state_.store_value_to_object(key_storage, value(operands[1]), key_type,
                                   builder);
      state_.store_value_to_object(value_storage, value(operands[2]), value_type,
                                   builder);
      auto *status = builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_container_v1_insert",
              llvm::FunctionType::get(builder.getInt32Ty(),
                                      {builder.getPtrTy(), builder.getPtrTy(),
                                       builder.getPtrTy(), builder.getPtrTy(),
                                       builder.getPtrTy()}, false)),
          {builder.CreateLoad(builder.getPtrTy(), field(0), "container.table"),
           key_storage, value_storage, replaced_storage, replaced_flag},
          "container.insert.status");
      const auto result_type = inst.type;
      const auto &result_semantic = state_.sem_ir.type(result_type);
      if (result_semantic.kind != SemTypeKind::Nominal) {
        state_.instruction_error = "HashMap insert result is not a Result";
        return nullptr;
      }
      const auto &result_nominal = state_.sem_ir.nominalType(
          NominalTypeId(result_semantic.arg0));
      const auto variant = [&](std::string_view name) -> std::uint32_t {
        for (std::uint32_t index = 0; index < result_nominal.variants.size();
             ++index)
          if (state_.sem_ir.identifier(state_.sem_ir.name(
                  result_nominal.variants[index].name).text) == name)
            return index;
        return core::AnyId::InvalidIndex;
      };
      const auto ok_variant = variant("Ok");
      const auto err_variant = variant("Err");
      if (ok_variant == core::AnyId::InvalidIndex ||
          err_variant == core::AnyId::InvalidIndex) {
        state_.instruction_error = "HashMap insert result has invalid variants";
        return nullptr;
      }
      auto *result_storage =
          entryAlloca(function, lowerObjectType(result_type),
                      "container.insert.result");
      builder.CreateLifetimeStart(result_storage);
      builder.CreateStore(llvm::Constant::getNullValue(lowerObjectType(result_type)),
                          result_storage);
      auto *failed = llvm::BasicBlock::Create(state_.context,
                                              "container.insert.failed", &function);
      auto *success = llvm::BasicBlock::Create(state_.context,
                                               "container.insert.success", &function);
      auto *done = llvm::BasicBlock::Create(state_.context,
                                            "container.insert.done", &function);
      builder.CreateCondBr(builder.CreateICmpNE(status, builder.getInt32(0)),
                           failed, success);
      builder.SetInsertPoint(failed);
      builder.CreateStore(builder.getInt32(err_variant),
                          builder.CreateStructGEP(lowerObjectType(result_type),
                                                  result_storage, 0));
      const auto error_type = state_.sem_ir.enumPayloadFieldType(
          result_type, err_variant, 0);
      auto *error_storage = entryAlloca(function, lowerObjectType(error_type),
                                         "container.insert.error");
      builder.CreateLifetimeStart(error_storage);
      builder.CreateStore(llvm::Constant::getNullValue(lowerObjectType(error_type)),
                          error_storage);
      auto *error_record = llvm::cast<llvm::StructType>(lowerObjectType(error_type));
      builder.CreateStore(builder.getInt32(0),
                          builder.CreateStructGEP(error_record, error_storage, 0));
      builder.CreateStore(status, builder.CreateStructGEP(error_record,
                                                          error_storage, 1));
      state_.move_value(enumPayloadAddress(result_storage, result_type, err_variant,
                                           0, builder),
                        error_storage, error_type, builder);
      builder.CreateBr(done);
      builder.SetInsertPoint(success);
      builder.CreateStore(builder.getInt32(ok_variant),
                          builder.CreateStructGEP(lowerObjectType(result_type),
                                                  result_storage, 0));
      const auto option_type = state_.sem_ir.enumPayloadFieldType(
          result_type, ok_variant, 0);
      const auto &option_nominal = state_.sem_ir.nominalType(
          NominalTypeId(state_.sem_ir.type(option_type).arg0));
      std::uint32_t some_variant = core::AnyId::InvalidIndex;
      std::uint32_t none_variant = core::AnyId::InvalidIndex;
      for (std::uint32_t index = 0; index < option_nominal.variants.size(); ++index) {
        const auto name = state_.sem_ir.identifier(state_.sem_ir.name(
            option_nominal.variants[index].name).text);
        if (name == "Some") some_variant = index;
        if (name == "None") none_variant = index;
      }
      auto *option_storage = enumPayloadAddress(result_storage, result_type,
                                                ok_variant, 0, builder);
      auto *option_record = llvm::cast<llvm::StructType>(lowerObjectType(option_type));
      auto *has_replaced = builder.CreateLoad(builder.getInt8Ty(), replaced_flag);
      auto *some = llvm::BasicBlock::Create(state_.context, "container.insert.some",
                                            &function);
      auto *none = llvm::BasicBlock::Create(state_.context, "container.insert.none",
                                            &function);
      builder.CreateCondBr(builder.CreateICmpNE(has_replaced, builder.getInt8(0)),
                           some, none);
      builder.SetInsertPoint(some);
      builder.CreateStore(builder.getInt32(some_variant),
                          builder.CreateStructGEP(option_record, option_storage, 0));
      state_.move_value(enumPayloadAddress(option_storage, option_type, some_variant,
                                           0, builder),
                        replaced_storage, value_type, builder);
      builder.CreateBr(done);
      builder.SetInsertPoint(none);
      builder.CreateStore(builder.getInt32(none_variant),
                          builder.CreateStructGEP(option_record, option_storage, 0));
      builder.CreateBr(done);
      builder.SetInsertPoint(done);
      update_generation(builder.CreateLoad(builder.getPtrTy(), field(0),
                                           "container.table.after-insert"));
      return result_storage;
    }
    if (role == CompilerIntrinsicRole::HashMapReserve ||
        role == CompilerIntrinsicRole::HashSetReserve) {
      if (operands.size() != 2) {
        state_.instruction_error = "container reserve has invalid operands";
        return nullptr;
      }
      auto *table = builder.CreateLoad(builder.getPtrTy(), field(0),
                                       "container.table");
      auto *capacity = builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_container_v1_capacity",
              llvm::FunctionType::get(builder.getInt64Ty(), {builder.getPtrTy()},
                                      false)),
          {table}, "container.capacity");
      auto *additional = value(operands[1]);
      auto *sum = builder.CreateCall(
          llvm::Intrinsic::getDeclaration(&state_.module,
                                          llvm::Intrinsic::uadd_with_overflow,
                                          {builder.getInt64Ty()}),
          {capacity, additional}, "container.reserve.sum");
      auto *requested = builder.CreateExtractValue(sum, 0);
      auto *overflow = builder.CreateExtractValue(sum, 1);
      auto *status = builder.CreateSelect(
          overflow, builder.getInt32(3),
          builder.CreateCall(
              state_.module.getOrInsertFunction(
                  "chtholly_next_container_v1_reserve",
                  llvm::FunctionType::get(builder.getInt32Ty(),
                                          {builder.getPtrTy(),
                                           builder.getInt64Ty()}, false)),
              {table, requested}, "container.reserve.status"));
      auto *result = makeStatusResult(inst.type, status, builder, function,
                                      "container.reserve");
      update_generation(table);
      return result;
    }
    if (role == CompilerIntrinsicRole::HashMapRemove) {
      if (operands.size() != 2) {
        state_.instruction_error = "HashMap remove has invalid operands";
        return nullptr;
      }
      const auto args = state_.sem_ir.typeBlock(TypeBlockId(
          state_.sem_ir.type(receiver_type).arg1));
      if (state_.sem_ir.type(receiver_type).kind != SemTypeKind::Nominal ||
          args.size() != 2) {
        state_.instruction_error = "HashMap remove has an invalid type";
        return nullptr;
      }
      const auto value_type = args[1];
      auto *removed_storage = entryAlloca(function, lowerObjectType(value_type),
                                           "container.remove.value");
      auto *removed_flag =
          entryAlloca(function, builder.getInt8Ty(), "container.remove.flag");
      builder.CreateLifetimeStart(removed_storage);
      builder.CreateStore(builder.getInt8(0), removed_flag);
      auto *status = builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_container_v1_erase",
              llvm::FunctionType::get(builder.getInt32Ty(),
                                      {builder.getPtrTy(), builder.getPtrTy(),
                                       builder.getPtrTy(), builder.getPtrTy()},
                                      false)),
          {builder.CreateLoad(builder.getPtrTy(), field(0), "container.table"),
           value(operands[1]), removed_storage, removed_flag},
          "container.remove.status");
      auto *invalid_status = builder.CreateICmpUGT(status, builder.getInt32(1));
      auto *status_trap = llvm::BasicBlock::Create(state_.context,
                                                   "container.remove.trap", &function);
      auto *status_ok = llvm::BasicBlock::Create(state_.context,
                                                 "container.remove.status-ok", &function);
      builder.CreateCondBr(invalid_status, status_trap, status_ok);
      builder.SetInsertPoint(status_trap);
      builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_runtime_v1_trap_failure",
              llvm::FunctionType::get(builder.getVoidTy(),
                                      {builder.getInt32Ty()}, false)),
          {builder.getInt32(4)});
      builder.CreateUnreachable();
      builder.SetInsertPoint(status_ok);
      const auto option_type = inst.type;
      const auto &option_semantic = state_.sem_ir.type(option_type);
      if (option_semantic.kind != SemTypeKind::Nominal) {
        state_.instruction_error = "HashMap remove result is not an Option";
        return nullptr;
      }
      const auto &option_nominal = state_.sem_ir.nominalType(
          NominalTypeId(option_semantic.arg0));
      std::uint32_t some_variant = core::AnyId::InvalidIndex;
      std::uint32_t none_variant = core::AnyId::InvalidIndex;
      for (std::uint32_t index = 0; index < option_nominal.variants.size(); ++index) {
        const auto variant_name = state_.sem_ir.identifier(state_.sem_ir.name(
            option_nominal.variants[index].name).text);
        if (variant_name == "Some") some_variant = index;
        if (variant_name == "None") none_variant = index;
      }
      if (some_variant == core::AnyId::InvalidIndex ||
          none_variant == core::AnyId::InvalidIndex) {
        state_.instruction_error = "HashMap remove option has invalid variants";
        return nullptr;
      }
      auto *result_storage = entryAlloca(function, lowerObjectType(option_type),
                                          "container.remove.result");
      builder.CreateLifetimeStart(result_storage);
      builder.CreateStore(llvm::Constant::getNullValue(lowerObjectType(option_type)),
                          result_storage);
      auto *some_block = llvm::BasicBlock::Create(state_.context,
                                                  "container.remove.some", &function);
      auto *none_block = llvm::BasicBlock::Create(state_.context,
                                                  "container.remove.none", &function);
      auto *merge_block = llvm::BasicBlock::Create(state_.context,
                                                   "container.remove.done", &function);
      builder.CreateCondBr(builder.CreateICmpEQ(status, builder.getInt32(0)),
                           some_block, none_block);
      builder.SetInsertPoint(some_block);
      auto *record = llvm::cast<llvm::StructType>(lowerObjectType(option_type));
      builder.CreateStore(builder.getInt32(some_variant),
                          builder.CreateStructGEP(record, result_storage, 0));
      state_.move_value(enumPayloadAddress(result_storage, option_type,
                                           some_variant, 0, builder),
                        removed_storage, value_type, builder);
      builder.CreateBr(merge_block);
      builder.SetInsertPoint(none_block);
      builder.CreateStore(builder.getInt32(none_variant),
                          builder.CreateStructGEP(record, result_storage, 0));
      builder.CreateBr(merge_block);
      builder.SetInsertPoint(merge_block);
      update_generation(builder.CreateLoad(builder.getPtrTy(), field(0),
                                           "container.table.after-remove"));
      return result_storage;
    }
    if (role == CompilerIntrinsicRole::HashSetInsert) {
      if (operands.size() != 2) {
        state_.instruction_error = "HashSet insert has invalid operands";
        return nullptr;
      }
      const auto args = state_.sem_ir.typeBlock(TypeBlockId(
          state_.sem_ir.type(receiver_type).arg1));
      if (state_.sem_ir.type(receiver_type).kind != SemTypeKind::Nominal ||
          args.size() != 1) {
        state_.instruction_error = "HashSet insert has an invalid type";
        return nullptr;
      }
      const auto element_type = args.front();
      auto *element_storage = entryAlloca(function, lowerObjectType(element_type),
                                           "container.set.element");
      auto *dummy_storage = entryAlloca(function, lowerObjectType(element_type),
                                         "container.set.dummy");
      auto *replaced_flag =
          entryAlloca(function, builder.getInt8Ty(), "container.set.flag");
      builder.CreateLifetimeStart(element_storage);
      builder.CreateLifetimeStart(dummy_storage);
      builder.CreateStore(builder.getInt8(0), replaced_flag);
      state_.store_value_to_object(element_storage, value(operands[1]),
                                   element_type, builder);
      state_.store_value_to_object(dummy_storage, value(operands[1]),
                                   element_type, builder);
      auto *status = builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_container_v1_insert",
              llvm::FunctionType::get(builder.getInt32Ty(),
                                      {builder.getPtrTy(), builder.getPtrTy(),
                                       builder.getPtrTy(), builder.getPtrTy(),
                                       builder.getPtrTy()}, false)),
          {builder.CreateLoad(builder.getPtrTy(), field(0), "container.table"),
           element_storage, dummy_storage,
           llvm::ConstantPointerNull::get(builder.getPtrTy()), replaced_flag},
          "container.set.insert.status");
      auto *result = makeBoolResult(inst.type, status, replaced_flag, builder,
                                    function, "container.set.insert");
      update_generation(builder.CreateLoad(builder.getPtrTy(), field(0),
                                           "container.table.after-insert"));
      return result;
    }
    if (role == CompilerIntrinsicRole::HashSetRemove) {
      if (operands.size() != 2) {
        state_.instruction_error = "HashSet remove has invalid operands";
        return nullptr;
      }
      const auto set_args = state_.sem_ir.typeBlock(TypeBlockId(
          state_.sem_ir.type(receiver_type).arg1));
      if (state_.sem_ir.type(receiver_type).kind != SemTypeKind::Nominal ||
          set_args.size() != 1) {
        state_.instruction_error = "HashSet remove has an invalid type";
        return nullptr;
      }
      const auto element_type = set_args.front();
      auto *removed = entryAlloca(function, lowerObjectType(element_type),
                                  "container.set.removed");
      auto *removed_flag =
          entryAlloca(function, builder.getInt8Ty(), "container.set.removed.flag");
      builder.CreateLifetimeStart(removed);
      builder.CreateStore(builder.getInt8(0), removed_flag);
      auto *status = builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_container_v1_erase",
              llvm::FunctionType::get(builder.getInt32Ty(),
                                      {builder.getPtrTy(), builder.getPtrTy(),
                                       builder.getPtrTy(), builder.getPtrTy()},
                                      false)),
          {builder.CreateLoad(builder.getPtrTy(), field(0), "container.table"),
           value(operands[1]), removed, removed_flag},
          "container.set.remove.status");
      auto *invalid_status = builder.CreateICmpUGT(status, builder.getInt32(1));
      auto *trap = llvm::BasicBlock::Create(state_.context, "container.set.remove.trap",
                                            &function);
      auto *done = llvm::BasicBlock::Create(state_.context, "container.set.remove.done",
                                            &function);
      auto *result = builder.CreateICmpEQ(status, builder.getInt32(0),
                                          "container.set.removed");
      auto *valid = llvm::BasicBlock::Create(state_.context, "container.set.remove.valid",
                                             &function);
      builder.CreateCondBr(invalid_status, trap, valid);
      builder.SetInsertPoint(trap);
      builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_runtime_v1_trap_failure",
              llvm::FunctionType::get(builder.getVoidTy(),
                                      {builder.getInt32Ty()}, false)),
          {builder.getInt32(4)});
      builder.CreateUnreachable();
      builder.SetInsertPoint(valid);
      builder.CreateBr(done);
      builder.SetInsertPoint(done);
      update_generation(builder.CreateLoad(builder.getPtrTy(), field(0),
                                           "container.table.after-remove"));
      return result;
    }
    if (role == CompilerIntrinsicRole::HashMapClear ||
        role == CompilerIntrinsicRole::HashSetClear) {
      if (operands.size() != 1) {
        state_.instruction_error = "container clear has invalid operands";
        return nullptr;
      }
      auto *status = builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_container_v1_clear",
              llvm::FunctionType::get(builder.getInt32Ty(), {builder.getPtrTy()},
                                      false)),
          {builder.CreateLoad(builder.getPtrTy(), field(0), "container.table")},
          "container.clear.status");
      auto *table = builder.CreateLoad(builder.getPtrTy(), field(0),
                                       "container.table.after-clear");
      update_generation(table);
      if (inst.type == state_.sem_ir.voidType())
        return nullptr;
      return makeStatusResult(inst.type, status, builder, function,
                              "container.clear");
    }
    if (role == CompilerIntrinsicRole::HashMapDrop ||
        role == CompilerIntrinsicRole::HashSetDrop) {
      if (operands.size() != 1) {
        state_.instruction_error = "container drop has invalid operands";
        return nullptr;
      }
      auto *storage = field(0);
      auto *table = builder.CreateLoad(builder.getPtrTy(), storage,
                                       "container.table");
      builder.CreateCall(
          state_.module.getOrInsertFunction(
              "chtholly_next_container_v1_destroy",
              llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                      false)),
          {table});
      builder.CreateStore(llvm::ConstantPointerNull::get(builder.getPtrTy()),
                          storage);
      return nullptr;
    }
    state_.instruction_error =
        "generic container operation has no native bridge plan";
    return nullptr;
  }

  llvm::Value *makeStatusResult(TypeId result_type, llvm::Value *status,
                                llvm::IRBuilder<> &builder,
                                llvm::Function &function,
                                llvm::StringRef name) {
    const auto &semantic = state_.sem_ir.type(result_type);
    if (semantic.kind != SemTypeKind::Nominal) {
      state_.instruction_error = "container status result is not an enum";
      return nullptr;
    }
    const auto &nominal = state_.sem_ir.nominalType(NominalTypeId(semantic.arg0));
    std::uint32_t ok_variant = core::AnyId::InvalidIndex;
    std::uint32_t err_variant = core::AnyId::InvalidIndex;
    for (std::uint32_t index = 0; index < nominal.variants.size(); ++index) {
      const auto variant_name = state_.sem_ir.identifier(state_.sem_ir.name(
          nominal.variants[index].name).text);
      if (variant_name == "Ok") ok_variant = index;
      if (variant_name == "Err") err_variant = index;
    }
    if (ok_variant == core::AnyId::InvalidIndex ||
        err_variant == core::AnyId::InvalidIndex) {
      state_.instruction_error = "container status result has invalid variants";
      return nullptr;
    }
    auto *storage = entryAlloca(function, lowerObjectType(result_type), name);
    builder.CreateLifetimeStart(storage);
    builder.CreateStore(llvm::Constant::getNullValue(lowerObjectType(result_type)),
                        storage);
    auto *tag = builder.CreateSelect(builder.CreateICmpEQ(status, builder.getInt32(0)),
                                    builder.getInt32(ok_variant),
                                    builder.getInt32(err_variant));
    builder.CreateStore(tag, builder.CreateStructGEP(lowerObjectType(result_type),
                                                     storage, 0));
    const auto error_type = state_.sem_ir.enumPayloadFieldType(result_type,
                                                                err_variant, 0);
    auto *error_storage = entryAlloca(function, lowerObjectType(error_type),
                                       std::string(name.str()) + ".error");
    builder.CreateLifetimeStart(error_storage);
    builder.CreateStore(llvm::Constant::getNullValue(lowerObjectType(error_type)),
                        error_storage);
    auto *error_record = llvm::dyn_cast<llvm::StructType>(lowerObjectType(error_type));
    if (!error_record || error_record->getNumElements() < 2) {
      state_.instruction_error = "container status error has invalid layout";
      return nullptr;
    }
    builder.CreateStore(builder.getInt32(0),
                        builder.CreateStructGEP(error_record, error_storage, 0));
    builder.CreateStore(status,
                        builder.CreateStructGEP(error_record, error_storage, 1));
    state_.move_value(enumPayloadAddress(storage, result_type, err_variant, 0,
                                         builder),
                      error_storage, error_type, builder);
    return storage;
  }

  llvm::Value *makeBoolResult(TypeId result_type, llvm::Value *status,
                              llvm::Value *replaced_flag,
                              llvm::IRBuilder<> &builder,
                              llvm::Function &function, llvm::StringRef name) {
    const auto &semantic = state_.sem_ir.type(result_type);
    if (semantic.kind != SemTypeKind::Nominal) {
      state_.instruction_error = "container bool result is not an enum";
      return nullptr;
    }
    const auto &nominal = state_.sem_ir.nominalType(NominalTypeId(semantic.arg0));
    std::uint32_t ok_variant = core::AnyId::InvalidIndex;
    std::uint32_t err_variant = core::AnyId::InvalidIndex;
    for (std::uint32_t index = 0; index < nominal.variants.size(); ++index) {
      const auto variant_name = state_.sem_ir.identifier(state_.sem_ir.name(
          nominal.variants[index].name).text);
      if (variant_name == "Ok") ok_variant = index;
      if (variant_name == "Err") err_variant = index;
    }
    if (ok_variant == core::AnyId::InvalidIndex ||
        err_variant == core::AnyId::InvalidIndex) {
      state_.instruction_error = "container bool result has invalid variants";
      return nullptr;
    }
    auto *storage = entryAlloca(function, lowerObjectType(result_type), name);
    builder.CreateLifetimeStart(storage);
    builder.CreateStore(llvm::Constant::getNullValue(lowerObjectType(result_type)),
                        storage);
    auto *success = llvm::BasicBlock::Create(state_.context, "container.bool.success",
                                             &function);
    auto *failure = llvm::BasicBlock::Create(state_.context, "container.bool.failure",
                                             &function);
    auto *done = llvm::BasicBlock::Create(state_.context, "container.bool.done",
                                          &function);
    builder.CreateCondBr(builder.CreateICmpEQ(status, builder.getInt32(0)),
                         success, failure);
    builder.SetInsertPoint(success);
    auto *record = llvm::cast<llvm::StructType>(lowerObjectType(result_type));
    builder.CreateStore(builder.getInt32(ok_variant),
                        builder.CreateStructGEP(record, storage, 0));
    const auto ok_payload = state_.sem_ir.enumPayloadFieldType(result_type,
                                                                ok_variant, 0);
    if (ok_payload == state_.sem_ir.boolType()) {
      auto *ok_address = enumPayloadAddress(storage, result_type, ok_variant, 0,
                                             builder);
      auto *flag = builder.CreateLoad(builder.getInt8Ty(), replaced_flag,
                                      "container.bool.flag");
      builder.CreateStore(builder.CreateICmpEQ(flag, builder.getInt8(0)),
                          ok_address);
    }
    builder.CreateBr(done);
    builder.SetInsertPoint(failure);
    builder.CreateStore(builder.getInt32(err_variant),
                        builder.CreateStructGEP(record, storage, 0));
    const auto error_type = state_.sem_ir.enumPayloadFieldType(result_type,
                                                                err_variant, 0);
    auto *error_storage = entryAlloca(function, lowerObjectType(error_type),
                                       std::string(name.str()) + ".error");
    builder.CreateLifetimeStart(error_storage);
    builder.CreateStore(llvm::Constant::getNullValue(lowerObjectType(error_type)),
                        error_storage);
    auto *error_record = llvm::cast<llvm::StructType>(lowerObjectType(error_type));
    builder.CreateStore(builder.getInt32(0),
                        builder.CreateStructGEP(error_record, error_storage, 0));
    builder.CreateStore(status,
                        builder.CreateStructGEP(error_record, error_storage, 1));
    state_.move_value(enumPayloadAddress(storage, result_type, err_variant, 0,
                                         builder),
                      error_storage, error_type, builder);
    builder.CreateBr(done);
    builder.SetInsertPoint(done);
    return storage;
  }

  llvm::GlobalVariable *containerVtable(TypeId container_type, TypeId key_type,
                                        TypeId value_type) {
    const auto descriptor = [&]() -> const SemConcreteContainerVTable * {
      for (const auto &candidate : state_.sem_ir.concreteContainerVTables())
        if (candidate.container_type == container_type &&
            candidate.key_type == key_type && candidate.value_type == value_type)
          return &candidate;
      return nullptr;
    }();
    if (!descriptor) {
      state_.instruction_error =
          "container intrinsic has no SemIR vtable descriptor";
      return nullptr;
    }
    const auto name = "__chtholly_container_vtable_" +
                      descriptor->layout_fingerprint.hex();
    if (auto *existing = state_.module.getNamedGlobal(name))
      return existing;
    const auto scalar = [&](TypeId type) {
      const auto kind = state_.sem_ir.type(type).kind;
      return kind == SemTypeKind::Integer || kind == SemTypeKind::Bool ||
             kind == SemTypeKind::Char || kind == SemTypeKind::Float ||
             kind == SemTypeKind::RawPointer;
    };
    const auto resolve_function = [&](FunctionRefId reference,
                                      std::string_view role)
        -> llvm::Function * {
      if (!reference.hasValue())
        return nullptr;
      const auto found = state_.functions.find(reference.index);
      if (found == state_.functions.end() || !found->second) {
        state_.instruction_error = "container " + std::string(role) +
                                   " witness has no LLVM target";
        return nullptr;
      }
      return found->second;
    };
    const auto custom_hash = resolve_function(descriptor->key_hash_function,
                                              "hash");
    const auto custom_equal = resolve_function(descriptor->key_equal_function,
                                               "equal");
    const bool has_custom_witness = custom_hash && custom_equal;
    if ((!scalar(key_type) || !scalar(value_type)) && !has_custom_witness) {
      state_.instruction_error =
          "container bridge requires verified Hash/Equal witnesses for non-scalar types";
      return nullptr;
    }
    auto *ptr = llvm::PointerType::getUnqual(state_.context);
    auto *i64 = llvm::Type::getInt64Ty(state_.context);
    auto *hash_type = llvm::FunctionType::get(i64, {ptr, i64, ptr}, false);
    auto *equal_type = llvm::FunctionType::get(
        llvm::Type::getInt32Ty(state_.context), {ptr, ptr, ptr}, false);
    auto *move_type = llvm::FunctionType::get(
        llvm::Type::getVoidTy(state_.context), {ptr, ptr, ptr}, false);
    const auto object_size = [&](TypeId type) -> std::uint64_t {
      return state_.module.getDataLayout()
          .getTypeAllocSize(lowerObjectType(type))
          .getFixedValue();
    };
    const auto bits = [&](TypeId type, llvm::Value *object,
                          llvm::IRBuilder<> &builder) -> llvm::Value * {
      const auto &semantic = state_.sem_ir.type(type);
      auto *loaded = builder.CreateLoad(lowerObjectType(type), object);
      if (semantic.kind == SemTypeKind::Float) {
        auto *integer = semantic.arg0 == 32 ? builder.getInt32Ty()
                                            : builder.getInt64Ty();
        auto *raw = builder.CreateBitCast(loaded, integer);
        return semantic.arg0 == 32
                   ? builder.CreateZExt(raw, builder.getInt64Ty())
                   : raw;
      }
      if (semantic.kind == SemTypeKind::RawPointer)
        return builder.CreatePtrToInt(loaded, builder.getInt64Ty());
      return builder.CreateIntCast(loaded, builder.getInt64Ty(),
                                   semantic.kind == SemTypeKind::Integer &&
                                       semantic.arg1 != 0);
    };
    const auto make_hash = [&](TypeId type) {
      if (custom_hash && custom_equal)
        return custom_hash;
      const auto function_name = name + "$hash";
      auto *function = state_.module.getFunction(function_name);
      if (function)
        return function;
      function = llvm::Function::Create(hash_type, llvm::GlobalValue::InternalLinkage,
                                        function_name, state_.module);
      auto *entry = llvm::BasicBlock::Create(state_.context, "entry", function);
      llvm::IRBuilder<> builder(entry);
      auto *mixed = builder.CreateXor(bits(type, function->getArg(0), builder),
                                      function->getArg(1));
      mixed = builder.CreateXor(mixed, builder.CreateLShr(mixed, builder.getInt64(30)));
      mixed = builder.CreateMul(mixed, builder.getInt64(0xbf58476d1ce4e5b9ULL));
      mixed = builder.CreateXor(mixed, builder.CreateLShr(mixed, builder.getInt64(27)));
      mixed = builder.CreateMul(mixed, builder.getInt64(0x94d049bb133111ebULL));
      builder.CreateRet(builder.CreateXor(mixed, builder.CreateLShr(mixed,
                                                                     builder.getInt64(31))));
      return function;
    };
    const auto make_equal = [&](TypeId type) {
      if (custom_hash && custom_equal)
        return custom_equal;
      const auto function_name = name + "$equal";
      auto *function = state_.module.getFunction(function_name);
      if (function)
        return function;
      function = llvm::Function::Create(equal_type, llvm::GlobalValue::InternalLinkage,
                                        function_name, state_.module);
      auto *entry = llvm::BasicBlock::Create(state_.context, "entry", function);
      llvm::IRBuilder<> builder(entry);
      builder.CreateRet(builder.CreateZExt(
          builder.CreateICmpEQ(bits(type, function->getArg(0), builder),
                               bits(type, function->getArg(1), builder)),
          builder.getInt32Ty()));
      return function;
    };
    const auto make_move = [&](std::string_view suffix, TypeId type) {
      const auto function_name = name + "$move$" + std::string(suffix);
      auto *function = state_.module.getFunction(function_name);
      if (function)
        return function;
      function = llvm::Function::Create(move_type, llvm::GlobalValue::InternalLinkage,
                                        function_name, state_.module);
      auto *entry = llvm::BasicBlock::Create(state_.context, "entry", function);
      llvm::IRBuilder<> builder(entry);
      // Delegate non-scalar movement to the ordinary object/lifecycle
      // lowering. This preserves computed projections and custom object
      // representations; only scalar values use the raw byte copy path.
      if (scalar(type)) {
        builder.CreateMemCpy(function->getArg(0), llvm::Align(1),
                             function->getArg(1), llvm::Align(1),
                             builder.getInt64(object_size(type)));
      } else {
        state_.move_value(function->getArg(0), function->getArg(1), type,
                          builder);
      }
      builder.CreateRetVoid();
      return function;
    };
    const auto make_drop = [&](std::string_view suffix, TypeId type) {
      const auto function_name = name + "$drop$" + std::string(suffix);
      auto *function = state_.module.getFunction(function_name);
      if (function)
        return function;
      function = llvm::Function::Create(move_type, llvm::GlobalValue::InternalLinkage,
                                        function_name, state_.module);
      auto *entry = llvm::BasicBlock::Create(state_.context, "entry", function);
      llvm::IRBuilder<> builder(entry);
      state_.destroy_address(type, function->getArg(0), builder,
                             *function);
      if (!state_.instruction_error.empty())
        return function;
      builder.CreateRetVoid();
      return function;
    };
    auto *hash = make_hash(key_type);
    auto *equal = make_equal(key_type);
    auto *move_key = make_move("key", key_type);
    auto *move_value = make_move("value", value_type);
    auto *drop_key = make_drop("key", key_type);
    auto *drop_value = make_drop("value", value_type);
    auto *i8 = llvm::Type::getInt8Ty(state_.context);
    auto *digest = llvm::ArrayType::get(i8, 32);
    auto *vtable_type = llvm::StructType::get(
        state_.context,
        {llvm::Type::getInt32Ty(state_.context),
         llvm::Type::getInt16Ty(state_.context),
         llvm::Type::getInt16Ty(state_.context),
         llvm::Type::getInt32Ty(state_.context),
         llvm::Type::getInt32Ty(state_.context), i64, i64, i64, i64, digest,
         digest, digest, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr},
        false);
    const auto canonical_type = [&](auto &&self, TypeId type,
                                    std::string &out) -> void {
      const auto &semantic = state_.sem_ir.type(type);
      out += std::to_string(static_cast<unsigned>(semantic.kind));
      out += ':';
      out += std::to_string(semantic.arg0);
      out += ':';
      out += std::to_string(semantic.arg1);
      out += ';';
      if (semantic.kind == SemTypeKind::Nominal) {
        const auto &nominal = state_.sem_ir.nominalType(
            NominalTypeId(semantic.arg0));
        out += std::string(state_.sem_ir.identifier(
            state_.sem_ir.name(nominal.name).text));
        out += ';';
        for (const auto argument : state_.sem_ir.typeBlock(
                 TypeBlockId(semantic.arg1)))
          self(self, argument, out);
      } else if (semantic.kind == SemTypeKind::Array ||
                 semantic.kind == SemTypeKind::Reference ||
                 semantic.kind == SemTypeKind::RawPointer ||
                 semantic.kind == SemTypeKind::Slice) {
        self(self, TypeId(semantic.arg0), out);
      } else if (semantic.kind == SemTypeKind::Tuple) {
        for (const auto element : state_.sem_ir.typeBlock(
                 TypeBlockId(semantic.arg0)))
          self(self, element, out);
      }
    };
    std::string key_canonical;
    std::string value_canonical;
    canonical_type(canonical_type, key_type, key_canonical);
    canonical_type(canonical_type, value_type, value_canonical);
    const auto key_fingerprint = descriptor->key_type_fingerprint;
    const auto value_fingerprint = descriptor->value_type_fingerprint;
    const auto layout_fingerprint = StableFingerprint::fromCanonicalBytes(
        "chtholly.next.container-layout-target.v1\n" +
        descriptor->layout_fingerprint.hex() + "\n" + key_canonical +
        value_canonical + std::to_string(object_size(key_type)) +
        std::to_string(object_size(value_type)) +
        std::to_string(state_.module.getDataLayout().getPointerSizeInBits()));
    const auto key_digest = key_fingerprint.bytes();
    const auto value_digest = value_fingerprint.bytes();
    const auto layout_digest = layout_fingerprint.bytes();
    const auto digest_constant = [&](const auto &bytes) -> llvm::Constant * {
      return llvm::ConstantDataArray::get(
          state_.context, llvm::ArrayRef<std::uint8_t>(bytes.data(), bytes.size()));
    };
    std::vector<llvm::Constant *> fields{
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(state_.context),
                               0x43485431U),
        llvm::ConstantInt::get(llvm::Type::getInt16Ty(state_.context),
                               1U),
        llvm::ConstantInt::get(llvm::Type::getInt16Ty(state_.context), 0),
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(state_.context),
                               CurrentSemanticArtifactEpoch),
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(state_.context),
                               state_.module.getDataLayout().getPointerSizeInBits()),
        llvm::ConstantInt::get(i64, object_size(key_type)),
        llvm::ConstantInt::get(i64, state_.module.getDataLayout()
                                      .getABITypeAlign(lowerObjectType(key_type))
                                      .value()),
        llvm::ConstantInt::get(i64, object_size(value_type)),
        llvm::ConstantInt::get(i64, state_.module.getDataLayout()
                                      .getABITypeAlign(lowerObjectType(value_type))
                                      .value()),
        digest_constant(key_digest), digest_constant(value_digest),
        digest_constant(layout_digest),
        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr)),
        hash, equal, move_key, move_value, drop_key, drop_value,
        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr)),
        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr)),
        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr))};
    return new llvm::GlobalVariable(
        state_.module, vtable_type, true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(vtable_type, fields), name);
  }

  llvm::Value *lowerInst(LowCompilerIntrinsicCall inst,
                         llvm::IRBuilder<> &builder, llvm::Function &function) {
    const auto role = state_.sem_ir.functionIntrinsicRole(inst.arg0);
    const auto operands = state_.low_ir.valueBlock(inst.arg1);
    if (role == CompilerIntrinsicRole::ChannelMake ||
        role == CompilerIntrinsicRole::ChannelInit) {
      if (operands.size() != 2) {
        state_.instruction_error = "channel.make has invalid operands";
        return nullptr;
      }
      const auto first_type = TypeId(state_.low_ir.inst(operands[0]).type);
      const bool receiver_first =
          state_.sem_ir.type(first_type).kind == SemTypeKind::Reference;
      const auto channel_operand = receiver_first ? operands[0] : operands[1];
      const auto capacity_operand = receiver_first ? operands[1] : operands[0];
      auto channel_type = TypeId(state_.low_ir.inst(channel_operand).type);
      if (state_.sem_ir.type(channel_type).kind == SemTypeKind::Reference)
        channel_type = state_.sem_ir.referencePointee(channel_type);
      if (!channel_type.hasValue() ||
          state_.sem_ir.type(channel_type).kind != SemTypeKind::Nominal) {
        state_.instruction_error = "channel.make has no concrete channel type";
        return nullptr;
      }
      const auto args = state_.sem_ir.typeBlock(
          TypeBlockId(state_.sem_ir.type(channel_type).arg1));
      if (args.size() != 1) {
        state_.instruction_error = "channel.make requires Channel<T>";
        return nullptr;
      }
      const auto payload = args.front();
      const SemTypedChannelDescriptor *descriptor = nullptr;
      for (const auto &candidate : state_.sem_ir.typedChannelDescriptors())
        if (state_.sem_ir.canonicalType(candidate.payload_type) ==
            state_.sem_ir.canonicalType(payload)) {
          descriptor = &candidate;
          break;
        }
      if (!descriptor) {
        state_.instruction_error = "channel.make has no verified descriptor";
        return nullptr;
      }
      const bool has_verified_plan = [&] {
        for (const auto &plan : state_.low_ir.payloadOperationPlans())
          if (plan.payload_type_digest == descriptor->payload_type_fingerprint &&
              plan.layout_digest == descriptor->layout_fingerprint &&
              plan.lifecycle_digest == descriptor->lifecycle_fingerprint &&
              plan.descriptor_digest == descriptor->component_descriptor_digest)
            return true;
        return false;
      }();
      if (!has_verified_plan) {
        state_.instruction_error =
            "channel.make has no verified payload operation plan";
        return nullptr;
      }
      auto *pointer = llvm::PointerType::getUnqual(state_.context);
      const auto base = descriptor->payload_type_fingerprint.hex();
      const auto suffix = base + "_" + descriptor->lifecycle_fingerprint.hex();
      auto *move = state_.module.getFunction("__chtholly_typed_move_" + suffix);
      if (!move) {
        auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(state_.context),
                                             {pointer, pointer}, false);
        move = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                                      "__chtholly_typed_move_" + suffix,
                                      state_.module);
        llvm::IRBuilder<> body(llvm::BasicBlock::Create(state_.context, "entry", move));
        state_.move_object(move->getArg(0), move->getArg(1), payload, body);
        body.CreateRetVoid();
      }
      auto *drop = state_.module.getFunction("__chtholly_typed_drop_" + suffix);
      if (!drop) {
        auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(state_.context),
                                             {pointer}, false);
        drop = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage,
                                      "__chtholly_typed_drop_" + suffix,
                                      state_.module);
        llvm::IRBuilder<> body(llvm::BasicBlock::Create(state_.context, "entry", drop));
        state_.destroy_address(payload, drop->getArg(0), body, *drop);
        body.CreateRetVoid();
      }
      auto *record = llvm::StructType::get(
          state_.context,
          {builder.getInt32Ty(), builder.getInt32Ty(), builder.getInt64Ty(),
           builder.getInt64Ty(), pointer, pointer});
      auto *layout = lowerObjectType(payload);
      const auto size = state_.module.getDataLayout().getTypeAllocSize(layout);
      const auto alignment =
          state_.module.getDataLayout().getABITypeAlign(layout).value();
      const auto global_name = "__chtholly_typed_descriptor_" + suffix;
      auto *global = state_.module.getNamedGlobal(global_name);
      if (!global) {
        auto *initializer = llvm::ConstantStruct::get(
            record,
            {builder.getInt32(1), builder.getInt32(3), builder.getInt64(size),
             builder.getInt64(alignment),
             llvm::ConstantExpr::getBitCast(move, pointer),
             llvm::ConstantExpr::getBitCast(drop, pointer)});
        global = new llvm::GlobalVariable(
            state_.module, record, true, llvm::GlobalValue::InternalLinkage,
            initializer, global_name);
      }
      auto runtime = state_.module.getOrInsertFunction(
          "chtholly_next_host_v1_typed_channel_init",
          llvm::FunctionType::get(builder.getInt32Ty(),
                                  {builder.getInt64Ty(), pointer, pointer}, false));
      auto *descriptor_ptr = builder.CreateBitCast(global, pointer);
      auto *out_channel = value(channel_operand);
      const auto out_type = TypeId(state_.low_ir.inst(channel_operand).type);
      if (state_.sem_ir.type(out_type).kind == SemTypeKind::Reference &&
          state_.sem_ir.type(state_.sem_ir.referencePointee(out_type)).kind ==
              SemTypeKind::Nominal)
        out_channel = builder.CreateStructGEP(
            lowerObjectType(state_.sem_ir.referencePointee(out_type)),
            out_channel, 0, "typed.channel.handle.out");
      out_channel = builder.CreateBitCast(out_channel, pointer);
      auto *status = builder.CreateCall(
          runtime, {value(capacity_operand), descriptor_ptr, out_channel},
          "typed.channel.init.status");
      if (state_.sem_ir.canonicalResultShape(inst.type))
        return makeStatusResult(inst.type, status, builder, function,
                                "typed.channel.init");
      return status;
    }
    if (role == CompilerIntrinsicRole::ChannelSend ||
        role == CompilerIntrinsicRole::ChannelReceive ||
        role == CompilerIntrinsicRole::ChannelClose ||
        role == CompilerIntrinsicRole::ChannelDrop) {
      if (operands.size() != compilerIntrinsicParameterCount(role)) {
        state_.instruction_error = "typed channel operation has invalid operands";
        return nullptr;
      }
      auto channel_type = TypeId(state_.low_ir.inst(operands[0]).type);
      const auto channel_ref =
          state_.sem_ir.type(channel_type).kind == SemTypeKind::Reference;
      if (channel_ref)
        channel_type = state_.sem_ir.referencePointee(channel_type);
      if (!channel_type.hasValue() ||
          state_.sem_ir.type(channel_type).kind != SemTypeKind::Nominal) {
        state_.instruction_error = "typed channel operation has no Channel<T>";
        return nullptr;
      }
      const auto channel_arguments = state_.sem_ir.typeBlock(
          TypeBlockId(state_.sem_ir.type(channel_type).arg1));
      if (channel_arguments.size() != 1) {
        state_.instruction_error = "typed channel operation requires Channel<T>";
        return nullptr;
      }
      const auto payload = channel_arguments.front();
      if (!state_.typed_channel_descriptor &&
          role != CompilerIntrinsicRole::ChannelDrop) {
        state_.instruction_error = "typed channel lowering has no descriptor service";
        return nullptr;
      }
      auto *descriptor = state_.typed_channel_descriptor(payload);
      if (!descriptor && role != CompilerIntrinsicRole::ChannelDrop) {
        state_.instruction_error = "typed channel operation has no verified descriptor";
        return nullptr;
      }
      auto *pointer = llvm::PointerType::getUnqual(state_.context);
      llvm::Value *channel_value = value(operands[0]);
      if (channel_ref) {
        if (state_.sem_ir.type(channel_type).kind == SemTypeKind::Nominal) {
          auto *field = builder.CreateStructGEP(lowerObjectType(channel_type),
                                                channel_value, 0,
                                                "typed.channel.handle");
          channel_value = builder.CreateLoad(pointer, field,
                                             "typed.channel.handle.value");
        } else {
          channel_value = loadValueFromObject(channel_value, channel_type,
                                              builder);
        }
      } else {
        auto *field = builder.CreateStructGEP(lowerObjectType(channel_type),
                                              channel_value, 0,
                                              "typed.channel.handle");
        channel_value = builder.CreateLoad(pointer, field,
                                           "typed.channel.handle.value");
      }
      auto *token_type = llvm::StructType::get(
          state_.context, {pointer, pointer, pointer, pointer,
                           builder.getInt64Ty(), builder.getInt32Ty(),
                           builder.getInt32Ty()});
      auto *token = entryAlloca(function, token_type, "typed.channel.token");
      const auto runtime_status = [&](llvm::StringRef name,
                                      llvm::FunctionType *type,
                                      llvm::ArrayRef<llvm::Value *> args) {
        return builder.CreateCall(state_.module.getOrInsertFunction(name, type),
                                  args, "typed.channel.status");
      };
      llvm::Value *status = nullptr;
      const auto result_shape = state_.sem_ir.canonicalResultShape(inst.type);
      llvm::Value *result_storage = nullptr;
      if (role == CompilerIntrinsicRole::ChannelSend || role == CompilerIntrinsicRole::ChannelReceive) {
        if (!result_shape) {
          state_.instruction_error = "typed channel operation requires a verified Result";
          return nullptr;
        }
        result_storage = entryAlloca(function, lowerObjectType(inst.type), "typed.channel.result.storage");
        builder.CreateLifetimeStart(result_storage);
      }
      if (role == CompilerIntrinsicRole::ChannelClose ||
          role == CompilerIntrinsicRole::ChannelDrop) {
        auto *close_type = llvm::FunctionType::get(builder.getInt32Ty(),
                                                   {pointer}, false);
        status = runtime_status("chtholly_next_host_v1_typed_channel_close",
                                close_type, {channel_value});
      } else {
        auto *ok = llvm::BasicBlock::Create(state_.context, "typed.channel.ok",
                                            &function);
        auto *failed = llvm::BasicBlock::Create(state_.context,
                                                "typed.channel.failed", &function);
        auto *done = llvm::BasicBlock::Create(state_.context, "typed.channel.done",
                                              &function);
        llvm::Value *first_status = nullptr;
        if (role == CompilerIntrinsicRole::ChannelSend) {
          auto *source = value(operands[1]);
          if (!source->getType()->isPointerTy()) {
            auto *storage = entryAlloca(function, lowerObjectType(payload),
                                         "typed.channel.send.value");
            storeValueToObject(storage, source, payload, builder);
            source = storage;
          }
          auto *prepare_type = llvm::FunctionType::get(
              builder.getInt32Ty(), {pointer, pointer, pointer}, false);
          first_status = runtime_status(
              "chtholly_next_host_v1_typed_channel_send_prepare", prepare_type,
              {channel_value, builder.CreateBitCast(source, pointer), token});
        } else {
          auto *acquire_type = llvm::FunctionType::get(
              builder.getInt32Ty(), {pointer, pointer}, false);
          first_status = runtime_status(
              "chtholly_next_host_v1_typed_channel_receive_acquire", acquire_type,
              {channel_value, token});
        }
        builder.CreateCondBr(builder.CreateICmpEQ(first_status, builder.getInt32(0)),
                             ok, failed);
        builder.SetInsertPoint(ok);
        llvm::Value *second_status = nullptr;
        if (role == CompilerIntrinsicRole::ChannelSend) {
          auto *commit_type = llvm::FunctionType::get(builder.getInt32Ty(),
                                                       {pointer}, false);
          second_status = runtime_status(
              "chtholly_next_host_v1_typed_channel_send_commit", commit_type,
              {token});
        } else {
          auto *destination = enumPayloadAddress(result_storage, inst.type,
              result_shape->ok_variant, 0, builder);
          auto *commit_type = llvm::FunctionType::get(
              builder.getInt32Ty(), {pointer, pointer}, false);
          second_status = runtime_status(
              "chtholly_next_host_v1_typed_channel_receive_commit", commit_type,
              {token, builder.CreateBitCast(destination, pointer)});
        }
        builder.CreateBr(done);
        builder.SetInsertPoint(failed);
        builder.CreateBr(done);
        builder.SetInsertPoint(done);
        auto *status_phi = builder.CreatePHI(builder.getInt32Ty(), 2,
                                             "typed.channel.result");
        status_phi->addIncoming(second_status, ok);
        status_phi->addIncoming(first_status, failed);
        status = status_phi;
      }
      if (result_storage) {
        auto *success = llvm::BasicBlock::Create(state_.context, "typed.result.ok", &function);
        auto *failure = llvm::BasicBlock::Create(state_.context, "typed.result.err", &function);
        auto *joined = llvm::BasicBlock::Create(state_.context, "typed.result.done", &function);
        builder.CreateCondBr(builder.CreateICmpEQ(status, builder.getInt32(0)), success, failure);
        builder.SetInsertPoint(success);
        builder.CreateStore(builder.getInt32(result_shape->ok_variant),
            builder.CreateStructGEP(lowerObjectType(inst.type), result_storage, 0));
        builder.CreateBr(joined);
        builder.SetInsertPoint(failure);
        builder.CreateStore(builder.getInt32(result_shape->err_variant),
            builder.CreateStructGEP(lowerObjectType(inst.type), result_storage, 0));
        auto *error_address = enumPayloadAddress(result_storage, inst.type,
            result_shape->err_variant, 0, builder);
        auto code_type = result_shape->error;
        if (role == CompilerIntrinsicRole::ChannelSend) {
          auto *send_layout = lowerObjectType(code_type);
          state_.move_value(builder.CreateStructGEP(send_layout, error_address, 1),
              value(operands[1]), payload, builder);
          error_address = builder.CreateStructGEP(send_layout, error_address, 0);
          code_type = state_.sem_ir.nominalFieldType(code_type, 0);
        }
        auto *code_layout = lowerObjectType(code_type);
        builder.CreateStore(llvm::Constant::getNullValue(code_layout), error_address);
        builder.CreateStore(status, builder.CreateStructGEP(code_layout, error_address, 1));
        builder.CreateBr(joined);
        builder.SetInsertPoint(joined);
        return result_storage;
      }
      if (role != CompilerIntrinsicRole::ChannelDrop &&
          state_.sem_ir.canonicalResultShape(inst.type))
        return makeStatusResult(inst.type, status, builder, function,
                                "typed.channel");
      if (role == CompilerIntrinsicRole::ChannelDrop)
        return nullptr;
      return status;
    }
    if (isVecCompilerIntrinsic(role) || isOptionCompilerIntrinsic(role))
      return lowerVecOrOptionIntrinsic(role, inst, operands, builder, function);
    if (isContainerCompilerIntrinsic(role))
      return lowerContainerIntrinsic(role, inst, operands, builder, function);
    if (role == CompilerIntrinsicRole::TextAsBytes) {
      if (operands.size() != 1 ||
          state_.sem_ir.type(TypeId(state_.low_ir.inst(operands[0]).type)).kind !=
              SemTypeKind::Reference) {
        state_.instruction_error =
            "text.as-bytes intrinsic has an invalid signature";
        return nullptr;
      }
      auto *source_address = value(operands[0]);
      auto *text = loadValueFromObject(
          source_address, state_.sem_ir.referencePointee(
                              TypeId(state_.low_ir.inst(operands[0]).type)),
          builder);
      llvm::Value *result =
          llvm::UndefValue::get(lowerObjectType(inst.type));
      result = builder.CreateInsertValue(result,
                                         builder.CreateExtractValue(text, 0),
                                         0);
      return builder.CreateInsertValue(result,
                                       builder.CreateExtractValue(text, 1), 1);
    }
    if (role == CompilerIntrinsicRole::TextSliceData ||
        role == CompilerIntrinsicRole::TextSliceDataMut) {
      if (operands.size() != 1 ||
          state_.sem_ir.type(TypeId(state_.low_ir.inst(operands[0]).type)).kind !=
              SemTypeKind::Slice) {
        state_.instruction_error =
            "text slice data intrinsic has an invalid signature";
        return nullptr;
      }
      return builder.CreateExtractValue(value(operands[0]), 0,
                                        role == CompilerIntrinsicRole::TextSliceData
                                            ? "slice.data"
                                            : "slice.data.mut");
    }
    if (role == CompilerIntrinsicRole::EnvArgCount) {
      if (!operands.empty()) {
        state_.instruction_error = "environment argument count has invalid operands";
        return nullptr;
      }
      auto runtime = state_.module.getOrInsertFunction(
          "chtholly_next_runtime_v1_arg_count",
          llvm::FunctionType::get(builder.getInt64Ty(), {}, false));
      return builder.CreateCall(runtime, {}, "env.arg.count");
    }
    if (role == CompilerIntrinsicRole::EnvArg) {
      if (operands.size() != 1 || inst.type != state_.sem_ir.stringType()) {
        state_.instruction_error = "environment argument has an invalid signature";
        return nullptr;
      }
      auto count_runtime = state_.module.getOrInsertFunction(
          "chtholly_next_runtime_v1_arg_count",
          llvm::FunctionType::get(builder.getInt64Ty(), {}, false));
      auto data_runtime = state_.module.getOrInsertFunction(
          "chtholly_next_runtime_v1_arg_data",
          llvm::FunctionType::get(builder.getPtrTy(), {builder.getInt64Ty()},
                                  false));
      auto size_runtime = state_.module.getOrInsertFunction(
          "chtholly_next_runtime_v1_arg_size",
          llvm::FunctionType::get(builder.getInt64Ty(), {builder.getInt64Ty()},
                                  false));
      auto trap_runtime = state_.module.getOrInsertFunction(
          "chtholly_next_runtime_v1_trap_failure",
          llvm::FunctionType::get(builder.getVoidTy(), {builder.getInt32Ty()},
                                  false));
      auto *index = value(operands[0]);
      auto *count = builder.CreateCall(count_runtime, {}, "env.arg.count");
      auto *trap = llvm::BasicBlock::Create(state_.context, "env.arg.oob", &function);
      auto *valid =
          llvm::BasicBlock::Create(state_.context, "env.arg.valid", &function);
      builder.CreateCondBr(builder.CreateICmpULT(index, count), valid, trap);
      builder.SetInsertPoint(trap);
      builder.CreateCall(trap_runtime, {builder.getInt32(1)});
      builder.CreateUnreachable();
      builder.SetInsertPoint(valid);
      llvm::Value *result = llvm::UndefValue::get(lowerObjectType(inst.type));
      result = builder.CreateInsertValue(
          result, builder.CreateCall(data_runtime, {index}, "env.arg.data"), 0);
      return builder.CreateInsertValue(
          result, builder.CreateCall(size_runtime, {index}, "env.arg.size"), 1);
    }
    if (role == CompilerIntrinsicRole::IoWriteStdout ||
        role == CompilerIntrinsicRole::IoWriteStderr) {
      if (operands.size() != 1 ||
          TypeId(state_.low_ir.inst(operands[0]).type) != state_.sem_ir.stringType()) {
        state_.instruction_error = "console write has an invalid signature";
        return nullptr;
      }
      auto runtime = state_.module.getOrInsertFunction(
          "chtholly_next_runtime_v1_console_write",
          llvm::FunctionType::get(
              builder.getInt64Ty(),
              {builder.getInt32Ty(), builder.getPtrTy(), builder.getInt64Ty()},
              false));
      auto *text = value(operands[0]);
      return builder.CreateCall(
          runtime,
          {builder.getInt32(role == CompilerIntrinsicRole::IoWriteStdout ? 1
                                                                         : 2),
           builder.CreateExtractValue(text, 0),
           builder.CreateExtractValue(text, 1)},
          "io.write");
    }
    if (role == CompilerIntrinsicRole::FsExists ||
        role == CompilerIntrinsicRole::FsWrite ||
        role == CompilerIntrinsicRole::FsRemove) {
      const auto expected_operands =
          role == CompilerIntrinsicRole::FsWrite ? 2U : 1U;
      if (operands.size() != expected_operands ||
          std::ranges::any_of(operands, [&](const auto operand) {
            return TypeId(state_.low_ir.inst(operand).type) != state_.sem_ir.stringType();
          })) {
        state_.instruction_error = "filesystem intrinsic has an invalid signature";
        return nullptr;
      }
      auto *path = value(operands[0]);
      llvm::SmallVector<llvm::Value *, 4> arguments{
          builder.CreateExtractValue(path, 0),
          builder.CreateExtractValue(path, 1)};
      if (role == CompilerIntrinsicRole::FsWrite) {
        auto *contents = value(operands[1]);
        arguments.push_back(builder.CreateExtractValue(contents, 0));
        arguments.push_back(builder.CreateExtractValue(contents, 1));
      }
      const auto name = role == CompilerIntrinsicRole::FsExists
                            ? "chtholly_next_runtime_v1_fs_exists"
                        : role == CompilerIntrinsicRole::FsWrite
                            ? "chtholly_next_runtime_v1_fs_write"
                            : "chtholly_next_runtime_v1_fs_remove";
      auto *result_type = role == CompilerIntrinsicRole::FsWrite
                              ? builder.getInt64Ty()
                              : builder.getInt32Ty();
      auto runtime = state_.module.getOrInsertFunction(
          name, llvm::FunctionType::get(
                    result_type,
                    role == CompilerIntrinsicRole::FsWrite
                        ? llvm::ArrayRef<llvm::Type *>{builder.getPtrTy(),
                                                       builder.getInt64Ty(),
                                                       builder.getPtrTy(),
                                                       builder.getInt64Ty()}
                        : llvm::ArrayRef<llvm::Type *>{builder.getPtrTy(),
                                                       builder.getInt64Ty()},
                    false));
      auto *result = builder.CreateCall(runtime, arguments, "fs.result");
      return role == CompilerIntrinsicRole::FsExists
                 ? builder.CreateICmpNE(result, builder.getInt32(0),
                                        "fs.exists")
                 : result;
    }
    const auto &function_type =
        state_.sem_ir.type(state_.sem_ir.functionRef(inst.arg0).local_type);
    const auto parameters = state_.sem_ir.typeBlock(TypeBlockId(function_type.arg0));
    const auto alignment_for = [&](TypeId type) {
      return state_.module.getDataLayout().getABITypeAlign(lowerObjectType(type));
    };
    const auto atomic_type = [&] {
      return !parameters.empty() && state_.sem_ir.type(parameters.front()).kind ==
                                        SemTypeKind::Reference
                 ? state_.sem_ir.referencePointee(parameters.front())
                 : TypeId::invalid();
    }();
    const auto scalar_type = [&] {
      if (role == CompilerIntrinsicRole::AtomicInit)
        return operands.empty() ? TypeId::invalid()
                                : TypeId(state_.low_ir.inst(operands[0]).type);
      if (role == CompilerIntrinsicRole::VolatileLoad)
        return inst.type;
      if (role == CompilerIntrinsicRole::VolatileStore)
        return operands.size() < 2 ? TypeId::invalid()
                                   : TypeId(state_.low_ir.inst(operands[1]).type);
      if (role == CompilerIntrinsicRole::WrappingMul)
        return operands.empty() ? TypeId::invalid()
                                : TypeId(state_.low_ir.inst(operands[0]).type);
      if (role == CompilerIntrinsicRole::FloatHash ||
          role == CompilerIntrinsicRole::FloatEqual)
        return parameters.empty() ||
                       state_.sem_ir.type(parameters[0]).kind !=
                           SemTypeKind::Reference
                   ? TypeId::invalid()
                   : state_.sem_ir.referencePointee(parameters[0]);
      if (role == CompilerIntrinsicRole::PointerHash ||
          role == CompilerIntrinsicRole::PointerEqual)
        return parameters.empty() ? TypeId::invalid() : parameters[0];
      return atomic_type.hasValue() ? state_.sem_ir.nominalFieldType(atomic_type, 0)
                                    : TypeId::invalid();
    }();
    const auto atomic_address = [&]() -> llvm::Value * {
      if (!atomic_type.hasValue() || operands.empty())
        return nullptr;
      return builder.CreateStructGEP(lowerObjectType(atomic_type),
                                     value(operands[0]), 0, "atomic.addr");
    };
    const auto order_at =
        [&](std::size_t index) -> std::optional<llvm::AtomicOrdering> {
      if (index >= operands.size())
        return std::nullopt;
      const auto order = compilerIntrinsicOrder(operands[index]);
      return order ? std::optional(llvmAtomicOrdering(*order)) : std::nullopt;
    };

    if (!scalar_type.hasValue()) {
      state_.instruction_error = "compiler intrinsic has no concrete scalar type";
      return nullptr;
    }
    switch (role) {
    case CompilerIntrinsicRole::AtomicInit: {
      if (operands.size() != 1) {
        state_.instruction_error = "atomic init has invalid operands";
        return nullptr;
      }
      auto *storage =
          entryAlloca(function, lowerObjectType(inst.type), "atomic.value");
      builder.CreateLifetimeStart(storage);
      auto *address = builder.CreateStructGEP(lowerObjectType(inst.type),
                                              storage, 0, "atomic.addr");
      builder.CreateStore(value(operands[0]), address);
      return storage;
    }
    case CompilerIntrinsicRole::AtomicLoad: {
      const auto order = order_at(1);
      auto *address = atomic_address();
      if (!order || !address) {
        state_.instruction_error = "atomic load has invalid operands";
        return nullptr;
      }
      auto *load = builder.CreateLoad(lowerObjectType(scalar_type), address,
                                      "atomic.load");
      load->setAtomic(*order);
      load->setAlignment(alignment_for(scalar_type));
      return load;
    }
    case CompilerIntrinsicRole::AtomicStore: {
      const auto order = order_at(2);
      auto *address = atomic_address();
      if (!order || !address || operands.size() != 3) {
        state_.instruction_error = "atomic store has invalid operands";
        return nullptr;
      }
      auto *store = builder.CreateStore(value(operands[1]), address);
      store->setAtomic(*order);
      store->setAlignment(alignment_for(scalar_type));
      return nullptr;
    }
    case CompilerIntrinsicRole::AtomicExchange:
    case CompilerIntrinsicRole::AtomicFetchAdd:
    case CompilerIntrinsicRole::AtomicFetchSub:
    case CompilerIntrinsicRole::AtomicFetchAnd:
    case CompilerIntrinsicRole::AtomicFetchOr:
    case CompilerIntrinsicRole::AtomicFetchXor: {
      const auto order = order_at(2);
      auto *address = atomic_address();
      if (!order || !address || operands.size() != 3) {
        state_.instruction_error = "atomic read-modify-write has invalid operands";
        return nullptr;
      }
      const auto operation = role == CompilerIntrinsicRole::AtomicExchange
                                 ? llvm::AtomicRMWInst::Xchg
                             : role == CompilerIntrinsicRole::AtomicFetchAdd
                                 ? llvm::AtomicRMWInst::Add
                             : role == CompilerIntrinsicRole::AtomicFetchSub
                                 ? llvm::AtomicRMWInst::Sub
                             : role == CompilerIntrinsicRole::AtomicFetchAnd
                                 ? llvm::AtomicRMWInst::And
                             : role == CompilerIntrinsicRole::AtomicFetchOr
                                 ? llvm::AtomicRMWInst::Or
                                 : llvm::AtomicRMWInst::Xor;
      return builder.CreateAtomicRMW(operation, address, value(operands[1]),
                                     alignment_for(scalar_type), *order);
    }
    case CompilerIntrinsicRole::AtomicCompareExchange: {
      const auto success = order_at(3);
      const auto failure = order_at(4);
      auto *address = atomic_address();
      if (!success || !failure || !address || operands.size() != 5) {
        state_.instruction_error = "atomic compare-exchange has invalid operands";
        return nullptr;
      }
      auto *exchange = builder.CreateAtomicCmpXchg(
          address, value(operands[1]), value(operands[2]),
          alignment_for(scalar_type), *success, *failure);
      exchange->setWeak(false);
      auto *result = entryAlloca(function, lowerObjectType(inst.type),
                                 "compare.exchange.result");
      builder.CreateLifetimeStart(result);
      auto *record = llvm::cast<llvm::StructType>(lowerObjectType(inst.type));
      builder.CreateStore(builder.CreateExtractValue(exchange, 0),
                          builder.CreateStructGEP(record, result, 0));
      builder.CreateStore(builder.CreateExtractValue(exchange, 1),
                          builder.CreateStructGEP(record, result, 1));
      return result;
    }
    case CompilerIntrinsicRole::VolatileLoad: {
      if (operands.size() != 1) {
        state_.instruction_error = "volatile load has invalid operands";
        return nullptr;
      }
      auto *load = builder.CreateLoad(lowerObjectType(scalar_type),
                                      value(operands[0]), "volatile.load");
      load->setVolatile(true);
      load->setAlignment(alignment_for(scalar_type));
      return load;
    }
    case CompilerIntrinsicRole::VolatileStore: {
      if (operands.size() != 2) {
        state_.instruction_error = "volatile store has invalid operands";
        return nullptr;
      }
      auto *store = builder.CreateStore(value(operands[1]), value(operands[0]));
      store->setVolatile(true);
      store->setAlignment(alignment_for(scalar_type));
      return nullptr;
    }
    case CompilerIntrinsicRole::WrappingMul: {
      if (operands.size() != 2 || !scalar_type.hasValue()) {
        state_.instruction_error = "wrapping multiplication has invalid operands";
        return nullptr;
      }
      // LLVM integer multiplication is defined modulo 2^N for both signed and
      // unsigned values, which is exactly the Chtholly wrapping contract.
      return builder.CreateMul(value(operands[0]), value(operands[1]),
                               "wrapping.mul");
    }
    case CompilerIntrinsicRole::FloatHash:
    case CompilerIntrinsicRole::FloatEqual: {
      if (operands.size() != 2 || parameters.empty() ||
          state_.sem_ir.type(parameters[0]).kind != SemTypeKind::Reference) {
        state_.instruction_error = "float witness has invalid operands";
        return nullptr;
      }
      const auto float_type =
          state_.sem_ir.referencePointee(parameters[0]);
      const auto &semantic_float = state_.sem_ir.type(float_type);
      if (semantic_float.kind != SemTypeKind::Float ||
          (semantic_float.arg0 != 32 && semantic_float.arg0 != 64)) {
        state_.instruction_error = "float witness has invalid width";
        return nullptr;
      }
      const auto load_float = [&](LowInstId operand) {
        return state_.load_value_from_object(value(operand), float_type,
                                              builder);
      };
      const auto bits_of = [&](llvm::Value *loaded) -> llvm::Value * {
        if (semantic_float.arg0 == 32) {
          auto *bits = builder.CreateBitCast(loaded, builder.getInt32Ty());
          return builder.CreateZExt(bits, builder.getInt64Ty(), "float.bits");
        }
        return builder.CreateBitCast(loaded, builder.getInt64Ty(),
                                     "float.bits");
      };
      const auto left_bits = bits_of(load_float(operands[0]));
      if (role == CompilerIntrinsicRole::FloatEqual) {
        const auto right_bits = bits_of(load_float(operands[1]));
        return builder.CreateICmpEQ(left_bits, right_bits, "float.equal");
      }
      auto *seed = value(operands[1]);
      auto *mixed = builder.CreateXor(left_bits, seed, "float.hash.seed");
      mixed = builder.CreateXor(
          mixed, builder.CreateLShr(mixed, builder.getInt64(30)),
          "float.hash.shift1");
      mixed = builder.CreateMul(mixed, builder.getInt64(0xbf58476d1ce4e5b9ULL),
                                "float.hash.mul1");
      mixed = builder.CreateXor(
          mixed, builder.CreateLShr(mixed, builder.getInt64(27)),
          "float.hash.shift2");
      mixed = builder.CreateMul(mixed, builder.getInt64(0x94d049bb133111ebULL),
                                "float.hash.mul2");
      return builder.CreateXor(mixed, builder.CreateLShr(mixed, builder.getInt64(31)),
                               "float.hash");
    }
    case CompilerIntrinsicRole::PointerHash:
    case CompilerIntrinsicRole::PointerEqual: {
      if (operands.size() != 2 || parameters.empty() ||
          state_.sem_ir.type(parameters[0]).kind != SemTypeKind::RawPointer) {
        state_.instruction_error = "pointer witness has invalid operands";
        return nullptr;
      }
      if (role == CompilerIntrinsicRole::PointerEqual)
        return builder.CreateICmpEQ(value(operands[0]), value(operands[1]),
                                    "pointer.equal");
      const auto pointer_bits =
          state_.module.getDataLayout().getPointerSizeInBits();
      auto *pointer_integer = builder.getIntNTy(pointer_bits);
      auto *bits = builder.CreatePtrToInt(value(operands[0]), pointer_integer,
                                          "pointer.bits.raw");
      if (pointer_bits < 64)
        bits = builder.CreateZExt(bits, builder.getInt64Ty(), "pointer.bits");
      else if (pointer_bits > 64)
        bits = builder.CreateTrunc(bits, builder.getInt64Ty(), "pointer.bits");
      auto *mixed = builder.CreateXor(bits, value(operands[1]),
                                      "pointer.hash.seed");
      mixed = builder.CreateXor(
          mixed, builder.CreateLShr(mixed, builder.getInt64(30)),
          "pointer.hash.shift1");
      mixed = builder.CreateMul(mixed, builder.getInt64(0xbf58476d1ce4e5b9ULL),
                                "pointer.hash.mul1");
      mixed = builder.CreateXor(
          mixed, builder.CreateLShr(mixed, builder.getInt64(27)),
          "pointer.hash.shift2");
      mixed = builder.CreateMul(mixed, builder.getInt64(0x94d049bb133111ebULL),
                                "pointer.hash.mul2");
      return builder.CreateXor(mixed, builder.CreateLShr(mixed, builder.getInt64(31)),
                               "pointer.hash");
    }
    case CompilerIntrinsicRole::EnvArgCount:
    case CompilerIntrinsicRole::EnvArg:
    case CompilerIntrinsicRole::IoWriteStdout:
    case CompilerIntrinsicRole::IoWriteStderr:
    case CompilerIntrinsicRole::FsExists:
    case CompilerIntrinsicRole::FsWrite:
    case CompilerIntrinsicRole::FsRemove:
    case CompilerIntrinsicRole::TextAsBytes:
    case CompilerIntrinsicRole::TextSliceData:
    case CompilerIntrinsicRole::TextSliceDataMut:
    case CompilerIntrinsicRole::ChannelMake:
    case CompilerIntrinsicRole::ChannelSendPrepare:
    case CompilerIntrinsicRole::ChannelSendCommit:
    case CompilerIntrinsicRole::ChannelSendCancel:
    case CompilerIntrinsicRole::ChannelReceiveAcquire:
    case CompilerIntrinsicRole::ChannelReceiveCommit:
    case CompilerIntrinsicRole::ChannelReceiveCancel:
    case CompilerIntrinsicRole::ChannelClose:
    case CompilerIntrinsicRole::ChannelInit:
    case CompilerIntrinsicRole::ChannelSend:
    case CompilerIntrinsicRole::ChannelReceive:
    case CompilerIntrinsicRole::ChannelDrop:
      llvm_unreachable("runtime intrinsic handled before scalar lowering");
    case CompilerIntrinsicRole::VecInit:
    case CompilerIntrinsicRole::VecLen:
    case CompilerIntrinsicRole::VecCapacity:
    case CompilerIntrinsicRole::VecReserve:
    case CompilerIntrinsicRole::VecPush:
    case CompilerIntrinsicRole::VecAt:
    case CompilerIntrinsicRole::VecAtMut:
    case CompilerIntrinsicRole::VecPop:
    case CompilerIntrinsicRole::VecRemove:
    case CompilerIntrinsicRole::VecClear:
    case CompilerIntrinsicRole::VecDrop:
    case CompilerIntrinsicRole::OptionIsSome:
    case CompilerIntrinsicRole::OptionIsNone:
    case CompilerIntrinsicRole::OptionUnwrap:
      llvm_unreachable("container intrinsic handled before scalar lowering");
    case CompilerIntrinsicRole::None:
    case CompilerIntrinsicRole::Count:
      state_.instruction_error = "invalid compiler intrinsic role reached LLVM";
      return nullptr;
    }
    llvm_unreachable("unhandled compiler intrinsic role");
  }

private:
  [[nodiscard]] llvm::Value *value(LowInstId id) const {
    return state_.value(id);
  }
  [[nodiscard]] llvm::Type *lowerValueType(TypeId type) const {
    return state_.lower_value_type(type);
  }
  [[nodiscard]] llvm::Type *lowerObjectType(TypeId type) const {
    return state_.lower_object_type(type);
  }
  [[nodiscard]] llvm::AllocaInst *entryAlloca(
      llvm::Function &function, llvm::Type *type, llvm::StringRef name) const {
    return state_.entry_alloca(function, type, name);
  }
  void copyObject(llvm::Value *destination, llvm::Value *source, TypeId type,
                  llvm::IRBuilder<> &builder) const {
    state_.copy_object(destination, source, type, builder);
  }
  void moveSemanticObject(llvm::Value *destination, llvm::Value *source,
                          TypeId type, llvm::IRBuilder<> &builder) const {
    state_.move_object(destination, source, type, builder);
  }
  void moveSemanticValueToObject(llvm::Value *destination, llvm::Value *source,
                                 TypeId type,
                                 llvm::IRBuilder<> &builder) const {
    state_.move_value(destination, source, type, builder);
  }
  [[nodiscard]] llvm::Value *loadValueFromObject(
      llvm::Value *source, TypeId type, llvm::IRBuilder<> &builder) const {
    return state_.load_value_from_object(source, type, builder);
  }
  void storeValueToObject(llvm::Value *destination, llvm::Value *source,
                          TypeId type, llvm::IRBuilder<> &builder) const {
    state_.store_value_to_object(destination, source, type, builder);
  }
  [[nodiscard]] llvm::Value *enumPayloadAddress(
      llvm::Value *owner, TypeId type, std::uint32_t variant,
      std::uint32_t field, llvm::IRBuilder<> &builder) const {
    return state_.enum_payload_address(owner, type, variant, field, builder);
  }
  void emitCoroutineDestroyAddress(TypeId type, llvm::Value *address,
                                   llvm::IRBuilder<> &builder,
                                   llvm::Function &function) const {
    state_.destroy_address(type, address, builder, function);
  }

  LLVMIntrinsicState &state_;
};

} // namespace

llvm::Value *LLVMIntrinsicLoweringService::lower(
    LowCompilerIntrinsicCall inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMIntrinsicState &state) {
  return IntrinsicEmitter(state).lowerInst(inst, builder, function);
}

} // namespace chtholly::compiler
