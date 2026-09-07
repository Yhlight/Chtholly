# Target-aware LowIR Foreign ABI

Status: implemented compiler boundary. Function and call layouts use epoch 11;
callback thunk plans use epoch 10.

## Ownership

The public `ForeignAbiSignature` remains target neutral. During SemIR to LowIR
lowering, `LowIR` queries every local or imported foreign callable exactly once
using its concrete function type, normalized target triple, representation
closure, and ABI epoch. The immutable result contains the target family,
semantic callee, result and parameter pass kinds, physical lanes, offsets,
extensions, alignment, and indirect-copy attributes.

`ForeignCall` names a `ForeignAbiLayoutId`, not a raw function reference.
Declarations and calls therefore cannot select different classifications.
The verifier recomputes each query, requires one layout for every foreign
callable and none for ordinary callables, and rejects stale IDs, duplicate
entries, unsupported aggregate targets, or semantic/representation drift.

LLVM lowering maps the verified physical lane vocabulary to LLVM types and
attributes. It may allocate copies and reconstruct results as directed, but it
does not inspect aggregate fields or the target ABI. The LLVM module triple
must equal the LowIR triple.

## Classifiers

- Windows x64 directly coerces 1-, 2-, 4-, and 8-byte aggregates to integers;
  other parameters are indirect copies and other results use `sret`.
- SysV AMD64 recursively merges at most two eightbytes into INTEGER or SSE;
  invalid lanes and larger objects use MEMORY with `byval` or `sret`.
- AArch64 Linux/ELF recognizes one to four homogeneous `f32` or `f64` leaves
  as one grouped HFA lane; other objects up to 16 bytes use one grouped value
  containing one or two integer eightbytes. Larger parameters use explicit
  indirect copies and larger results use `sret`.
- `repr(C)` unions use maximum-member size and alignment with every member at
  offset zero. Windows x64 applies its ordinary size rule, SysV AMD64 merges
  all overlapping member classifications, and AAPCS64 excludes every
  union-containing object from HFA recognition.
- Windows ARM64, Darwin ARM64, 32-bit targets, and unknown targets reject
  aggregate foreign signatures. Scalar transport remains target independent.

Foreign function and call layout epoch 9 covers direct layouts, variadic suffix
descriptors, and callback types. Callback thunk plan epoch 10 covers explicit
entry/context/release adapter plans and foreign registration handoff plans.
`CHNXLAY11` persists the nominal struct/union kind so layout
verification can distinguish valid overlap from corrupt struct offsets.

## Linux x64 Validation

The Architecture V7 gate checks SysV classification from completed SemIR
through LowIR verification and LLVM declarations. It covers sign/zero
extension, INTEGER/SSE lane combinations, nested aggregates, arrays,
overlapping `repr(C)` unions, and large `byval`/`sret` objects. The telemetry
workspace is also built and run as a native System V ELF64 executable.

CFDL epoch 8 now publishes ABI-only record carriers into this classifier.
Windows x64 and Linux x64 source-driven tests cover fixed-width records and the
LLP64/LP64 split for `c_long`. This does not expose record fields to Chtholly
and now admits ABI-only CFDL unions. Bit-field, flexible-array, and packed
syntax remain closed.

## Reserved Extensions

Variadic call-site suffix layouts, non-capturing callback types, explicit
capturing entry/context/release adapters, and registration handoff are
implemented. The adapter remains invalid as one C parameter. Epoch-10
registration APIs expose exact marked entry, userdata, and optional release
parameters; source-bound extra registration arguments remain closed.
