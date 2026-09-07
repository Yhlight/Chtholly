# Compiler Architecture V16: Typed C Constants And Errno Projection

Status: implemented by the 1.9.12 Tier-1 CFFI wave.

## Clang-Owned Macro Evaluation

`constant` roots select individual active object-like macros. The first Clang
translation unit validates the configured C environment and final macro
definition. A second bounded translation unit materializes each root as a
typed constant using `__typeof__`; libclang evaluates the resulting declaration.
Chtholly never reparses replacement tokens. Only target C integer and boolean
values enter CFDL; empty, function-like, floating, string, address, type, and
non-constant macros fail closed.

## Error Contract Boundary

CFDL epoch 10 adds `foreign const` and the finite `error errno when result`
contract. The provider artifact keeps the raw signed-integer C signature plus
a typed errno domain, predicate, and literal. Consumer semantic checking
projects a call to canonical `Result<Raw, i32>` only when `std::result` is
available. LowIR retains the raw foreign layout; LLVM checks the returned value
and reads `_errno` on Windows or `__errno_location` on Linux only in the failure
block. No Component ABI or C ABI changes.

Regeneration owns constant type/value changes mechanically and preserves errno
contracts as human semantic overlays. Result carrier changes, removal with a
live contract, unsigned results, and `out`/`inout` combinations fail closed.

## Version Closure

CFFI config v3, `CHCFFI3`, `CHCFFIS3`, CFDL epoch 10, `CHNXIOP7` format 7 and
schema 6, semantic artifact epoch 18, Package Artifact v16, `CHNXTPK73` state
70, and cache namespace `next-v42` replace the prior formats. Old encodings are
not compatibility-parsed. Standard-library epoch 9, Component ABI epoch 1, and
runtime ABI v1 are unchanged.
