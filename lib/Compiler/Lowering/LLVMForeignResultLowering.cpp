#include "LLVMInternal.h"

#include <cassert>
#include <limits>

namespace chtholly::compiler {

llvm::Value *LLVMForeignResultLoweringService::lower(
    LowForeignResultCall inst, llvm::IRBuilder<> &builder,
    llvm::Function &function, LLVMForeignResultState &state) {
    const auto &plan = state.low_ir.foreignCallOutcomePlan(inst.arg0);
    const auto &call_layout = state.low_ir.foreignAbiCallLayout(plan.call_layout);
    const auto &layout = state.low_ir.foreignAbiLayout(call_layout.function_layout);
    llvm::SmallVector<llvm::Value *, 4> arguments;
    for (const auto argument : state.low_ir.valueBlock(inst.arg1))
      arguments.push_back(state.value(argument));
    llvm::SmallVector<llvm::Value *, 6> physical_arguments;
    llvm::AllocaInst *outcome_count_storage = nullptr;
    if (plan.outcome_projection ==
        interop::ForeignOperationArtifact::OutcomeProjection::Win32Read) {
      outcome_count_storage =
          state.entry_alloca(function, state.lower_object_type(plan.outcome_count_type),
                      "cffi.read_count");
      builder.CreateLifetimeStart(outcome_count_storage);
      for (std::uint32_t lane = 0; lane < plan.argument_sources.size();
           ++lane) {
        const auto source = plan.argument_sources[lane];
        if (source.kind ==
            ForeignCallOutcomePlan::ArgumentSourceKind::PublicArgument) {
          physical_arguments.push_back(arguments[source.index]);
        } else if (source.kind ==
                   ForeignCallOutcomePlan::ArgumentSourceKind::OutcomeStorage) {
          physical_arguments.push_back(outcome_count_storage);
        } else {
          physical_arguments.push_back(
              llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(
                  state.lower_value_type(layout.parameters[lane].semantic_type))));
        }
      }
    } else {
      physical_arguments.assign(arguments.begin(), arguments.end());
    }
    const auto trap_if = [&](llvm::Value *condition, std::string_view label) {
      auto *trap = llvm::BasicBlock::Create(
          state.context, llvm::Twine("cffi.") + label + ".trap", &function);
      auto *valid = llvm::BasicBlock::Create(
          state.context, llvm::Twine("cffi.") + label + ".valid", &function);
      builder.CreateCondBr(condition, trap, valid);
      builder.SetInsertPoint(trap);
      auto *trap_type = llvm::FunctionType::get(builder.getVoidTy(),
                                                {builder.getInt32Ty()}, false);
      builder.CreateCall(
          state.module.getOrInsertFunction("chtholly_next_runtime_v1_trap_failure",
                                      trap_type),
          {builder.getInt32(5)});
      builder.CreateUnreachable();
      builder.SetInsertPoint(valid);
    };
    const auto fread =
        plan.outcome_projection ==
        interop::ForeignOperationArtifact::OutcomeProjection::Fread;
    if (plan.outcome_projection ==
        interop::ForeignOperationArtifact::OutcomeProjection::PosixRead) {
      auto *capacity = physical_arguments[plan.outcome_capacity_lane];
      auto *capacity_type = llvm::cast<llvm::IntegerType>(capacity->getType());
      auto *signed_max = llvm::ConstantInt::get(
          capacity_type,
          llvm::APInt::getSignedMaxValue(capacity_type->getBitWidth()));
      trap_if(builder.CreateICmpUGT(capacity, signed_max), "capacity");
    }
    if (fread) {
      auto *element_size = physical_arguments[plan.outcome_size_lane];
      auto *element_count = physical_arguments[plan.outcome_capacity_lane];
      auto *wide_size =
          element_size->getType()->getIntegerBitWidth() == 64
              ? element_size
              : builder.CreateZExt(element_size, builder.getInt64Ty());
      auto *wide_count =
          element_count->getType()->getIntegerBitWidth() == 64
              ? element_count
              : builder.CreateZExt(element_count, builder.getInt64Ty());
      auto *zero = llvm::ConstantInt::get(builder.getInt64Ty(), 0);
      auto *max = llvm::ConstantInt::get(
          builder.getInt64Ty(), std::numeric_limits<std::uint64_t>::max());
      auto *size_nonzero = builder.CreateICmpNE(wide_size, zero);
      auto *max_count = builder.CreateUDiv(max, wide_size);
      trap_if(builder.CreateAnd(size_nonzero,
                                builder.CreateICmpUGT(wide_count, max_count)),
              "fread.capacity");
    }
    auto *raw = state.emit_foreign_call_values(layout, call_layout,
                                      state.functions.at(layout.target.index),
                                      physical_arguments, builder, function);
    const auto result_type = plan.projected_result_type;
    const auto result_shape = state.sem_ir.canonicalResultShape(result_type);
    assert(result_shape && raw);

    llvm::Value *failed = nullptr;
    llvm::Value *fread_empty = nullptr;
    if (fread) {
      auto *size = physical_arguments[plan.outcome_size_lane];
      auto *count = physical_arguments[plan.outcome_capacity_lane];
      auto *zero_size = llvm::ConstantInt::get(size->getType(), 0);
      auto *zero_count = llvm::ConstantInt::get(count->getType(), 0);
      fread_empty = builder.CreateOr(builder.CreateICmpEQ(size, zero_size),
                                     builder.CreateICmpEQ(count, zero_count),
                                     "cffi.fread.empty");
      auto *check =
          llvm::BasicBlock::Create(state.context, "cffi.fread.check", &function);
      auto *empty =
          llvm::BasicBlock::Create(state.context, "cffi.fread.empty", &function);
      auto *merge =
          llvm::BasicBlock::Create(state.context, "cffi.fread.merge", &function);
      builder.CreateCondBr(fread_empty, empty, check);
      builder.SetInsertPoint(check);
      auto *ferror_type = llvm::FunctionType::get(
          builder.getInt32Ty(),
          {physical_arguments[plan.outcome_context_lane]->getType()}, false);
      auto *ferror = builder.CreateCall(
          state.module.getOrInsertFunction(plan.outcome_ferror_symbol, ferror_type),
          {physical_arguments[plan.outcome_context_lane]}, "cffi.ferror");
      auto *checked_failed = builder.CreateICmpNE(
          ferror, llvm::ConstantInt::get(ferror->getType(), 0), "cffi.failed");
      builder.CreateBr(merge);
      builder.SetInsertPoint(empty);
      builder.CreateBr(merge);
      builder.SetInsertPoint(merge);
      auto *failed_phi = builder.CreatePHI(builder.getInt1Ty(), 2);
      failed_phi->addIncoming(builder.getFalse(), empty);
      failed_phi->addIncoming(checked_failed, check);
      failed = failed_phi;
    } else if (plan.predicate ==
               interop::ForeignOperationArtifact::ErrorPredicate::Null) {
      failed = builder.CreateICmpEQ(
          raw,
          llvm::ConstantPointerNull::get(
              llvm::cast<llvm::PointerType>(raw->getType())),
          "cffi.failed");
    } else if (raw->getType()->isPointerTy()) {
      assert(plan.predicate == interop::ForeignOperationArtifact::
                                   ErrorPredicate::InvalidSentinel &&
             plan.intervals.size() == 1);
      auto *bits = llvm::ConstantInt::get(
          llvm::IntegerType::get(state.context, plan.predicate_width),
          plan.intervals.front().lower, false);
      auto *sentinel = llvm::ConstantExpr::getIntToPtr(
          bits, llvm::cast<llvm::PointerType>(raw->getType()));
      failed = builder.CreateICmpEQ(raw, sentinel, "cffi.failed");
    } else {
      auto *integer_type = llvm::cast<llvm::IntegerType>(raw->getType());
      for (const auto &interval : plan.intervals) {
        auto *lower =
            llvm::ConstantInt::get(integer_type, interval.lower, false);
        llvm::Value *member = nullptr;
        if (interval.lower == interval.upper) {
          member = builder.CreateICmpEQ(raw, lower, "cffi.member");
        } else {
          auto *upper =
              llvm::ConstantInt::get(integer_type, interval.upper, false);
          auto *above = plan.predicate_signed
                            ? builder.CreateICmpSGE(raw, lower, "cffi.above")
                            : builder.CreateICmpUGE(raw, lower, "cffi.above");
          auto *below = plan.predicate_signed
                            ? builder.CreateICmpSLE(raw, upper, "cffi.below")
                            : builder.CreateICmpULE(raw, upper, "cffi.below");
          member = builder.CreateAnd(above, below, "cffi.member");
        }
        failed =
            failed ? builder.CreateOr(failed, member, "cffi.members") : member;
      }
      assert(failed);
      if (plan.predicate_inverted)
        failed = builder.CreateNot(failed, "cffi.failed");
    }
    auto *storage =
        state.entry_alloca(function, state.lower_object_type(result_type), "cffi.result");
    builder.CreateLifetimeStart(storage);
    auto *success = llvm::BasicBlock::Create(state.context, "cffi.ok", &function);
    auto *failure = llvm::BasicBlock::Create(state.context, "cffi.err", &function);
    auto *done = llvm::BasicBlock::Create(state.context, "cffi.done", &function);
    builder.CreateCondBr(failed, failure, success);

    auto *result_record =
        llvm::cast<llvm::StructType>(state.lower_object_type(result_type));
    builder.SetInsertPoint(success);
    builder.CreateStore(builder.getInt32(result_shape->ok_variant),
                        builder.CreateStructGEP(result_record, storage, 0));
    if (plan.outcome_projection ==
            interop::ForeignOperationArtifact::OutcomeProjection::PosixRead ||
        plan.outcome_projection ==
            interop::ForeignOperationArtifact::OutcomeProjection::Win32Read ||
        fread) {
      const auto win32 =
          plan.outcome_projection ==
          interop::ForeignOperationArtifact::OutcomeProjection::Win32Read;
      auto *capacity = physical_arguments[plan.outcome_capacity_lane];
      auto *buffer = physical_arguments[plan.outcome_buffer_lane];
      auto *raw_count =
          win32 ? builder.CreateLoad(state.lower_value_type(plan.outcome_count_type),
                                     outcome_count_storage, "cffi.read_count")
                : raw;
      auto *zero = llvm::ConstantInt::get(raw_count->getType(), 0);
      auto *negative =
          win32 ? builder.getFalse() : builder.CreateICmpSLT(raw_count, zero);
      auto *count =
          raw_count->getType()->getIntegerBitWidth() == 64 ? raw_count
          : win32 ? builder.CreateZExt(raw_count, builder.getInt64Ty())
          : fread ? builder.CreateZExt(raw_count, builder.getInt64Ty())
                  : builder.CreateSExt(raw_count, builder.getInt64Ty());
      if (fread) {
        auto *element_size = physical_arguments[plan.outcome_size_lane];
        auto *wide_size =
            element_size->getType()->getIntegerBitWidth() == 64
                ? element_size
                : builder.CreateZExt(element_size, builder.getInt64Ty());
        count = builder.CreateMul(count, wide_size, "cffi.fread_bytes");
      }
      auto *wide_capacity =
          capacity->getType()->getIntegerBitWidth() == 64
              ? capacity
              : builder.CreateZExt(capacity, builder.getInt64Ty());
      auto *too_large = fread ? builder.CreateICmpUGT(raw_count, capacity)
                              : builder.CreateICmpUGT(count, wide_capacity);
      auto *positive = win32 || fread ? builder.CreateICmpUGT(raw_count, zero)
                                      : builder.CreateICmpSGT(raw_count, zero);
      auto *null_data = builder.CreateAnd(
          positive, builder.CreateIsNull(buffer), "cffi.null_data");
      trap_if(
          builder.CreateOr(negative, builder.CreateOr(too_large, null_data)),
          "outcome");

      auto *outcome_storage = state.enum_payload_address(
          storage, result_type, result_shape->ok_variant, 0, builder);
      auto *outcome_record =
          llvm::cast<llvm::StructType>(state.lower_object_type(plan.outcome_type));
      auto *data_block =
          llvm::BasicBlock::Create(state.context, "cffi.data", &function);
      auto *eof_block =
          llvm::BasicBlock::Create(state.context, "cffi.eof", &function);
      auto *outcome_done =
          llvm::BasicBlock::Create(state.context, "cffi.outcome", &function);
      llvm::Value *is_eof = nullptr;
      if (fread) {
        auto *eof_check =
            llvm::BasicBlock::Create(state.context, "cffi.fread.eof", &function);
        builder.CreateCondBr(fread_empty, data_block, eof_check);
        builder.SetInsertPoint(eof_check);
        auto *eof_type = llvm::FunctionType::get(
            builder.getInt32Ty(),
            {physical_arguments[plan.outcome_context_lane]->getType()}, false);
        auto *eof = builder.CreateCall(
            state.module.getOrInsertFunction(plan.outcome_eof_symbol, eof_type),
            {physical_arguments[plan.outcome_context_lane]}, "cffi.feof");
        auto *short_read = builder.CreateICmpULT(raw_count, capacity);
        auto *eof_set = builder.CreateICmpNE(
            eof, llvm::ConstantInt::get(eof->getType(), 0), "cffi.eof_set");
        trap_if(builder.CreateAnd(short_read, builder.CreateNot(eof_set)),
                "fread.short");
        is_eof = builder.CreateAnd(short_read, eof_set);
        builder.CreateCondBr(is_eof, eof_block, data_block);
      } else {
        is_eof = builder.CreateAnd(
            builder.CreateICmpNE(
                capacity, llvm::ConstantInt::get(capacity->getType(), 0)),
            builder.CreateICmpEQ(raw_count, zero));
        builder.CreateCondBr(is_eof, eof_block, data_block);
      }

      builder.SetInsertPoint(data_block);
      builder.CreateStore(
          builder.getInt32(plan.data_variant),
          builder.CreateStructGEP(outcome_record, outcome_storage, 0));
      llvm::Value *slice =
          llvm::UndefValue::get(state.lower_value_type(plan.slice_type));
      slice = builder.CreateInsertValue(slice, buffer, 0);
      slice = builder.CreateInsertValue(slice, count, 1);
      state.store_value_to_object(state.enum_payload_address(outcome_storage, plan.outcome_type,
                                            plan.data_variant, 0, builder),
                         slice, plan.slice_type, builder);
      builder.CreateBr(outcome_done);

      builder.SetInsertPoint(eof_block);
      builder.CreateStore(
          builder.getInt32(plan.eof_variant),
          builder.CreateStructGEP(outcome_record, outcome_storage, 0));
      builder.CreateBr(outcome_done);
      builder.SetInsertPoint(outcome_done);
    } else if (plan.success_payload ==
               interop::ForeignOperationArtifact::ErrorSuccessPayload::Raw) {
      state.store_value_to_object(state.enum_payload_address(storage, result_type,
                                            result_shape->ok_variant, 0,
                                            builder),
                         raw, result_shape->success, builder);
    }
    builder.CreateBr(done);

    builder.SetInsertPoint(failure);
    builder.CreateStore(builder.getInt32(result_shape->err_variant),
                        builder.CreateStructGEP(result_record, storage, 0));
    llvm::Value *error_value = nullptr;
    if (plan.extractor ==
        interop::ForeignOperationArtifact::ErrorExtractor::ReturnedCode) {
      error_value = raw;
    } else if (plan.extractor ==
               interop::ForeignOperationArtifact::ErrorExtractor::Errno) {
      auto *errno_type = llvm::FunctionType::get(
          llvm::PointerType::getUnqual(llvm::Type::getInt32Ty(state.context)), {},
          false);
      const auto accessor =
          state.low_ir.targetTriple().find("windows") != std::string::npos
              ? "_errno"
              : "__errno_location";
      auto *errno_address =
          builder.CreateCall(state.module.getOrInsertFunction(accessor, errno_type));
      error_value = builder.CreateLoad(llvm::Type::getInt32Ty(state.context),
                                       errno_address, "cffi.errno");
    } else {
      auto *last_error_type =
          llvm::FunctionType::get(llvm::Type::getInt32Ty(state.context), {}, false);
      error_value = builder.CreateCall(
          state.module.getOrInsertFunction("GetLastError", last_error_type), {},
          "cffi.win32_error");
    }
    state.store_value_to_object(state.enum_payload_address(storage, result_type,
                                          result_shape->err_variant, 0,
                                          builder),
                       error_value, result_shape->error, builder);
    builder.CreateBr(done);
    builder.SetInsertPoint(done);
    if (outcome_count_storage)
      builder.CreateLifetimeEnd(outcome_count_storage);
    return storage;
  }

} // namespace chtholly::compiler
