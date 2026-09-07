# Compiler Architecture V18: Typed Foreign Result Predicates

Status: implemented by the 1.9.14 Tier-1 CFFI wave.

## Type-Owned Sentinels

CFDL foreign nominal types may attach a non-zero integer bit pattern to a raw
pointer carrier. A call writes `result == invalid`; it does not repeat the
platform spelling or bit pattern. Elaboration resolves that predicate from the
result nominal, checks it against the target pointer width, and distinguishes a
pointer bit pattern from an integer invalid value. `null` remains the only
spelling for the zero pointer.

This supports Win32 `INVALID_HANDLE_VALUE` and POSIX `MAP_FAILED` without
teaching LLVM either macro name. The physical C signature and native ABI remain
unchanged.

## Canonical Predicate Plan

Single comparisons and `in`/`not in` sets elaborate to sorted, merged closed
integer intervals. Constants are resolved before publication; Interop stores
only target-width bit patterns, signedness, and an optional inversion. Named
constants therefore survive regeneration in source while artifact identity is
independent of spelling and declaration order.

`ForeignCallOutcomePlan` owns the verified call layout, predicate, extractor,
and Result projection. LowIR rechecks the artifact fingerprint, physical type,
nominal sentinel, interval canonicality, target, and Result shape. LLVM emits
typed signed or unsigned comparisons and reads errno or GetLastError only in
the selected failure block.

## POSIX Boundary

The executable corpus now distinguishes real `read` outcomes: `-1` is errno,
zero is EOF, and a positive value may be a short read. V18 deliberately leaves
zero and positive values in `Result<Raw, i32>`. A future result protocol must
model EOF/data/error variants and the initialized prefix of an output buffer;
classifying short reads as failures would be incorrect.

## Version Closure

CFDL epoch 12, `CHNXIOP9` format 9/schema 8, semantic artifact epoch 20,
Package Artifact v18, `CHNXTPK75` state 72, cache namespace `next-v44`, resource
protocol epoch 5, nominal formats `CHNXTYPE29`/`CHNXSPE31`/`CHNXLAY21`/
`CHNXWIT26`, and concrete specialization `CHNXSCC48` replace their previous
formats. CFFI config v3, `CHCFFI3`, `CHCFFIS3`, standard-library epoch 9,
Component ABI epoch 1, runtime ABI v1, and native C ABI are unchanged.
