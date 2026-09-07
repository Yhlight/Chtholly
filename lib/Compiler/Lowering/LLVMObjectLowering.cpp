#include "LLVMInternal.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"

#include <cassert>
#include <string>

namespace chtholly::compiler {

llvm::Type *LLVMObjectLoweringService::lowerObjectType(
    TypeId id, const LowIR &low_ir, const SemIR &sem_ir, llvm::Module &module,
    llvm::LLVMContext &context,
    std::unordered_map<std::uint32_t, llvm::Type *> &types,
    std::string &lowering_error,
    const std::function<llvm::Type *(TypeId)> &recursive) {
  if (const auto found = types.find(id.index); found != types.end())
    return found->second;
  const auto cache = [&](llvm::Type *value) {
    types.emplace(id.index, value);
    return value;
  };
  const auto object_type = low_ir.typeRepresentation(id).object_type;
  if (object_type != id) {
    auto *carrier = recursive(object_type);
    if (sem_ir.type(id).kind == SemTypeKind::Nominal) {
      const auto &nominal =
          sem_ir.nominalType(NominalTypeId(sem_ir.type(id).arg0));
      const auto dedicated =
          nominal.kind == NominalKind::ForeignHandle ||
          (nominal.kind == NominalKind::ForeignResource &&
           !nominal.foreign_registration_storage_type.hasValue());
      const auto *frozen = low_ir.nominalLayout(id);
      if (!frozen && low_ir.requiresNominalLayouts() && !dedicated)
        lowering_error = "custom nominal LLVM lowering requires a frozen layout";
      else if (frozen && !carrier->isSized())
        lowering_error = "custom nominal LLVM carrier is unsized";
      else if (frozen) {
        const auto &layout = module.getDataLayout();
        if (layout.getTypeAllocSize(carrier).getFixedValue() != frozen->size ||
            layout.getABITypeAlign(carrier).value() != frozen->alignment)
          lowering_error =
              "frozen custom nominal layout disagrees with its LLVM carrier";
      }
    }
    return cache(carrier);
  }
  const auto &type = sem_ir.type(id);
  if (type.kind == SemTypeKind::TypeParameter)
    return llvm::Type::getVoidTy(context);
  if (type.kind == SemTypeKind::Nominal) {
    const auto &nominal = sem_ir.nominalType(NominalTypeId(type.arg0));
    std::vector<llvm::Type *> fields;
    for (const auto field : low_ir.typeRepresentation(id).object_fields)
      fields.push_back(recursive(field));
    auto *record = llvm::StructType::create(
        context, sem_ir.identifier(sem_ir.name(nominal.name).text));
    types.emplace(id.index, record);
    const auto *frozen_nominal = low_ir.nominalLayout(id);
    if (!frozen_nominal && low_ir.requiresNominalLayouts())
      lowering_error =
          "concrete nominal LLVM lowering requires a frozen layout for `" +
          std::string(sem_ir.identifier(sem_ir.name(nominal.name).text)) + "`";
    else if (frozen_nominal && frozen_nominal->kind != nominal.kind)
      lowering_error = "nominal LLVM lowering kind disagrees with frozen layout";
    if (nominal.kind == NominalKind::Enum) {
      const auto *frozen = low_ir.enumLayout(id);
      if (!frozen || frozen->tag_size != 4) {
        lowering_error = "enum LLVM lowering requires a fixed-u32 LowIR layout";
        record->setBody({llvm::Type::getInt32Ty(context)}, false);
        return record;
      }
      const auto &layout = module.getDataLayout();
      llvm::Type *carrier = nullptr;
      std::uint64_t payload_alignment = 1;
      for (std::uint32_t variant = 0; variant < nominal.variants.size(); ++variant) {
        payload_alignment =
            std::max(payload_alignment, frozen->variants[variant].alignment);
        for (std::uint32_t field = 0;
             field < nominal.variants[variant].fields.size(); ++field) {
          auto *field_type = recursive(
              sem_ir.enumPayloadFieldType(id, variant, field));
          if (field_type->isVoidTy())
            continue;
          if (!field_type->isSized()) {
            lowering_error = "enum LLVM payload field is unsized";
            continue;
          }
          if (!carrier || layout.getABITypeAlign(field_type) >
                              layout.getABITypeAlign(carrier))
            carrier = field_type;
        }
      }
      std::vector<llvm::Type *> enum_fields{llvm::Type::getInt32Ty(context)};
      if (frozen->size > frozen->payload_offset) {
        if (!carrier || layout.getABITypeAlign(carrier).value() != payload_alignment)
          lowering_error = "frozen enum alignment disagrees with LLVM target data";
        const auto carrier_size = layout.getTypeAllocSize(carrier).getFixedValue();
        const auto storage_size = frozen->size - frozen->payload_offset;
        if (storage_size < carrier_size)
          lowering_error = "frozen enum storage is smaller than its LLVM carrier";
        std::vector<llvm::Type *> storage{carrier};
        if (storage_size > carrier_size)
          storage.push_back(llvm::ArrayType::get(
              llvm::Type::getInt8Ty(context), storage_size - carrier_size));
        enum_fields.push_back(llvm::StructType::get(context, storage));
      }
      record->setBody(enum_fields, false);
      if (!record->isSized())
        lowering_error = "enum LLVM object is unsized";
      return record;
    }
    if (nominal.kind == NominalKind::Union) {
      if (fields.empty()) {
        lowering_error = "union LLVM lowering requires at least one field";
        record->setBody({llvm::Type::getInt8Ty(context)}, false);
        return record;
      }
      const auto &layout = module.getDataLayout();
      std::size_t carrier = 0;
      std::uint64_t size = 0;
      for (std::size_t index = 0; index < fields.size(); ++index) {
        size = std::max<std::uint64_t>(size,
                                       layout.getTypeAllocSize(fields[index]));
        if (layout.getABITypeAlign(fields[index]) >
            layout.getABITypeAlign(fields[carrier]))
          carrier = index;
      }
      const auto carrier_size = layout.getTypeAllocSize(fields[carrier]);
      std::vector<llvm::Type *> storage{fields[carrier]};
      if (size > carrier_size)
        storage.push_back(llvm::ArrayType::get(
            llvm::Type::getInt8Ty(context), size - carrier_size));
      record->setBody(storage, false);
      if (frozen_nominal) {
        const auto *record_layout = layout.getStructLayout(record);
        if (record_layout->getSizeInBytes() != frozen_nominal->size ||
            record_layout->getAlignment().value() != frozen_nominal->alignment ||
            frozen_nominal->fields.size() != fields.size())
          lowering_error = "frozen union layout disagrees with LLVM target data";
      }
      return record;
    }
    record->setBody(fields, false);
    if (!record->isSized())
      lowering_error = "nominal LLVM object is unsized";
    if (frozen_nominal) {
      const auto &layout = module.getDataLayout();
      const auto *record_layout = layout.getStructLayout(record);
      if (record_layout->getSizeInBytes() != frozen_nominal->size ||
          record_layout->getAlignment().value() != frozen_nominal->alignment ||
          frozen_nominal->fields.size() != fields.size())
        lowering_error = "frozen nominal layout disagrees with LLVM target data";
      else {
        for (std::size_t index = 0; index < fields.size(); ++index)
          if (record_layout->getElementOffset(static_cast<unsigned>(index)) !=
              frozen_nominal->fields[index].offset) {
            lowering_error =
                "frozen nominal field offset disagrees with LLVM target data";
            break;
          }
      }
    }
    return record;
  }
  const auto make_record = [&](auto expected) {
    std::vector<llvm::Type *> fields;
    for (const auto field : low_ir.typeRepresentation(id).object_fields)
      fields.push_back(recursive(field));
    (void)expected;
    assert(fields.size() == expected);
    return cache(llvm::StructType::get(context, fields));
  };
  if (type.kind == SemTypeKind::CallbackAdapter)
    return make_record(3);
  if (type.kind == SemTypeKind::CallbackRegistration) {
    std::vector<llvm::Type *> fields;
    for (const auto field : low_ir.typeRepresentation(id).object_fields)
      fields.push_back(recursive(field));
    assert(fields.size() == 5 || fields.size() == 7 || fields.size() == 8 ||
           fields.size() == 10);
    return cache(llvm::StructType::get(context, fields));
  }
  if (type.kind == SemTypeKind::CallbackCompletion) {
    std::vector<llvm::Type *> fields;
    for (const auto field : low_ir.typeRepresentation(id).object_fields)
      fields.push_back(recursive(field));
    assert(fields.size() == 4 || fields.size() == 5 || fields.size() == 7);
    return cache(llvm::StructType::get(context, fields));
  }
  if (type.kind == SemTypeKind::CallbackWake)
    return make_record(2);
  if (type.kind == SemTypeKind::Reference || type.kind == SemTypeKind::RawPointer ||
      type.kind == SemTypeKind::Function || type.kind == SemTypeKind::CoroutineExecutor ||
      type.kind == SemTypeKind::CoroutineScope || type.kind == SemTypeKind::CoroutineTask ||
      type.kind == SemTypeKind::CoroutineTaskCompletion ||
      type.kind == SemTypeKind::CFunctionPointer ||
      type.kind == SemTypeKind::CVariadicFunctionPointer ||
      type.kind == SemTypeKind::CoroutineTaskCompletionSet)
    return cache(llvm::PointerType::getUnqual(context));
  if (type.kind == SemTypeKind::CoroutineTaskSelection)
    return cache(llvm::StructType::get(
        context, {llvm::Type::getInt32Ty(context),
                  llvm::PointerType::getUnqual(context)}));
  if (type.kind == SemTypeKind::CoroutineTaskOutcome)
    return cache(llvm::Type::getInt32Ty(context));
  if (type.kind == SemTypeKind::CoroutineChecked)
    return cache(llvm::StructType::get(
        context, {llvm::Type::getInt32Ty(context),
                  llvm::PointerType::getUnqual(context)}));
  llvm::Type *result = nullptr;
  switch (type.kind) {
  case SemTypeKind::Void:
  case SemTypeKind::Never:
    result = llvm::Type::getVoidTy(context);
    break;
  case SemTypeKind::Bool:
    result = llvm::Type::getInt1Ty(context);
    break;
  case SemTypeKind::Char:
    result = llvm::Type::getInt32Ty(context);
    break;
  case SemTypeKind::Integer:
    result = llvm::IntegerType::get(context, type.arg0);
    break;
  case SemTypeKind::Float:
    result = type.arg0 == 32 ? llvm::Type::getFloatTy(context)
                             : llvm::Type::getDoubleTy(context);
    break;
  case SemTypeKind::String:
    result = llvm::StructType::get(llvm::PointerType::getUnqual(context),
                                   llvm::Type::getInt64Ty(context));
    break;
  case SemTypeKind::Array:
    result = llvm::ArrayType::get(recursive(TypeId(type.arg0)), type.arg1);
    break;
  case SemTypeKind::Tuple: {
    std::vector<llvm::Type *> fields;
    for (const auto element : sem_ir.typeBlock(TypeBlockId(type.arg0)))
      fields.push_back(recursive(element));
    if (!sem_ir.isCUnionType(id)) {
      result = llvm::StructType::get(context, fields);
      break;
    }
    const auto &layout = module.getDataLayout();
    std::size_t carrier = 0;
    std::uint64_t size = 0;
    for (std::size_t index = 0; index < fields.size(); ++index) {
      size = std::max<std::uint64_t>(
          size, layout.getTypeAllocSize(fields[index]).getFixedValue());
      if (layout.getABITypeAlign(fields[index]) >
          layout.getABITypeAlign(fields[carrier]))
        carrier = index;
    }
    const auto carrier_size =
        layout.getTypeAllocSize(fields[carrier]).getFixedValue();
    std::vector<llvm::Type *> storage{fields[carrier]};
    if (size > carrier_size)
      storage.push_back(llvm::ArrayType::get(
          llvm::Type::getInt8Ty(context), size - carrier_size));
    result = llvm::StructType::get(context, storage);
    break;
  }
  case SemTypeKind::Slice:
    result = llvm::StructType::get(
        context, {llvm::PointerType::getUnqual(context),
                  llvm::Type::getInt64Ty(context)});
    break;
  case SemTypeKind::AsyncFunction:
  case SemTypeKind::Invalid:
  case SemTypeKind::Count:
    result = llvm::Type::getVoidTy(context);
    break;
  default:
    llvm_unreachable("handled before the representation switch");
  }
  return cache(result);
}

llvm::Value *LLVMObjectLoweringService::enumPayloadAddress(
    llvm::Value *owner_address, TypeId owner, std::uint32_t variant,
    std::uint32_t field, llvm::IRBuilder<> &builder, const LowIR &low_ir,
    llvm::LLVMContext &context, const ObjectTypeFn &lower_object_type) {
  const auto *layout = low_ir.enumLayout(owner);
  assert(layout && variant < layout->variants.size() &&
         field < layout->variants[variant].field_offsets.size());
  auto *record = llvm::cast<llvm::StructType>(lower_object_type(owner));
  assert(record->getNumElements() == 2 &&
         "payload projection on a unit-only enum");
  auto *payload = builder.CreateStructGEP(record, owner_address, 1);
  return builder.CreateConstGEP1_64(
      llvm::Type::getInt8Ty(context), payload,
      layout->variants[variant].field_offsets[field], "enum.payload.addr");
}

void LLVMObjectLoweringService::copySemanticObject(
    llvm::Value *destination, llvm::Value *source, TypeId type,
    llvm::IRBuilder<> &builder, llvm::Function &function, const SemIR &sem_ir,
    const LowIR &low_ir, llvm::LLVMContext &context,
    const std::unordered_map<std::uint32_t, llvm::Function *> &functions,
    const ObjectTypeFn &lower_object_type, const CopyObjectFn &copy_object,
    const PayloadAddressFn &payload_address,
    const RecursiveCopyFn &recursive_copy) {
  const auto &representation = low_ir.typeRepresentation(type);
  if (representation.facts.copy == CopyReprKind::Custom) {
    assert(representation.copy_target.hasValue());
    builder.CreateCall(functions.at(representation.copy_target.index),
                       {destination, source});
    return;
  }
  const auto &semantic = sem_ir.type(type);
  if (semantic.kind != SemTypeKind::Nominal ||
      sem_ir.nominalType(NominalTypeId(semantic.arg0)).kind !=
          NominalKind::Enum) {
    copy_object(destination, source, type, builder);
    return;
  }
  const auto &nominal = sem_ir.nominalType(NominalTypeId(semantic.arg0));
  auto *record = llvm::cast<llvm::StructType>(lower_object_type(type));
  auto *source_tag_address = builder.CreateStructGEP(record, source, 0);
  auto *destination_tag_address = builder.CreateStructGEP(record, destination, 0);
  auto *tag = builder.CreateLoad(builder.getInt32Ty(), source_tag_address,
                                 "enum.copy.tag");
  builder.CreateStore(tag, destination_tag_address);
  auto *done = llvm::BasicBlock::Create(context, "enum.copy.done", &function);
  auto *dispatch = builder.CreateSwitch(
      tag, done, static_cast<unsigned>(nominal.variants.size()));
  for (std::uint32_t variant = 0; variant < nominal.variants.size(); ++variant) {
    auto *active = llvm::BasicBlock::Create(
        context, "enum.copy.variant." + std::to_string(variant), &function);
    dispatch->addCase(builder.getInt32(variant), active);
    builder.SetInsertPoint(active);
    for (std::uint32_t field = 0;
         field < nominal.variants[variant].fields.size(); ++field) {
      const auto field_type =
          sem_ir.enumPayloadFieldType(type, variant, field);
      recursive_copy(payload_address(destination, type, variant, field, builder),
                     payload_address(source, type, variant, field, builder),
                     field_type, builder, function);
    }
    builder.CreateBr(done);
  }
  builder.SetInsertPoint(done);
}

llvm::Constant *LLVMObjectLoweringService::constantObject(
    ConstantId id, std::string &error, const SemIR &sem_ir,
    llvm::Module &module, llvm::LLVMContext &context,
    const ObjectTypeFn &lower_object_type,
    const std::function<llvm::Constant *(ConstantId, std::string &)> &recursive) {
  if (!id.hasValue() || id.index >= sem_ir.constantValueCount()) {
    error = "readonly static has an invalid canonical initializer";
    return nullptr;
  }
  const auto &value = sem_ir.constantValue(id);
  const auto payload_integer = [&] {
    return sem_ir.integer(IntegerId(static_cast<std::uint32_t>(value.payload)));
  };
  switch (value.kind) {
  case ConstantValueKind::Integer:
  case ConstantValueKind::ForeignEnum:
    return llvm::ConstantInt::get(lower_object_type(value.type),
                                  payload_integer(), true);
  case ConstantValueKind::Float: {
    const auto width = sem_ir.type(value.type).arg0;
    const auto bits = static_cast<std::uint64_t>(payload_integer());
    const auto number = width == 32
                            ? llvm::APFloat(llvm::APFloat::IEEEsingle(),
                                            llvm::APInt(32, bits))
                            : llvm::APFloat(llvm::APFloat::IEEEdouble(),
                                            llvm::APInt(64, bits));
    return llvm::ConstantFP::get(context, number);
  }
  case ConstantValueKind::Bool:
    return llvm::ConstantInt::get(llvm::Type::getInt1Ty(context),
                                  payload_integer() != 0);
  case ConstantValueKind::String: {
    const auto text = sem_ir.string(
        StringLiteralId(static_cast<std::uint32_t>(value.payload)));
    auto *data = llvm::ConstantDataArray::getString(context, text, false);
    auto *global = new llvm::GlobalVariable(module, data->getType(), true,
                                             llvm::GlobalValue::PrivateLinkage,
                                             data, ".static.str");
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    auto *record = llvm::cast<llvm::StructType>(lower_object_type(value.type));
    return llvm::ConstantStruct::get(
        record, {global, llvm::ConstantInt::get(
                           llvm::Type::getInt64Ty(context), text.size())});
  }
  case ConstantValueKind::Null:
    return llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(lower_object_type(value.type)));
  case ConstantValueKind::Array: {
    std::vector<llvm::Constant *> elements;
    for (const auto element : sem_ir.constantBlock(value.elements)) {
      auto *lowered = recursive(element, error);
      if (!lowered)
        return nullptr;
      elements.push_back(lowered);
    }
    return llvm::ConstantArray::get(
        llvm::cast<llvm::ArrayType>(lower_object_type(value.type)), elements);
  }
  case ConstantValueKind::Aggregate: {
    auto *record = llvm::dyn_cast<llvm::StructType>(lower_object_type(value.type));
    const auto source = sem_ir.constantBlock(value.elements);
    if (!record || record->getNumElements() != source.size()) {
      error = "readonly static aggregate disagrees with its object layout";
      return nullptr;
    }
    std::vector<llvm::Constant *> fields;
    for (const auto field : source) {
      auto *lowered = recursive(field, error);
      if (!lowered)
        return nullptr;
      fields.push_back(lowered);
    }
    return llvm::ConstantStruct::get(record, fields);
  }
  case ConstantValueKind::Union:
  case ConstantValueKind::Enum:
    error = "readonly static initializer requires unsupported object packing";
    return nullptr;
  }
  error = "readonly static has an unknown canonical initializer";
  return nullptr;
}

} // namespace chtholly::compiler
