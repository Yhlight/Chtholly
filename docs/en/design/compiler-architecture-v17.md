# Compiler Architecture V17: Foreign Error Call Plans

Status: implemented by the 1.9.13 Tier-1 CFFI wave.

## Structured Error Contract

CFDL epoch 11 represents a foreign error contract as three independent facts:
the predicate over the raw result, the error extractor, and the successful
payload policy. `code` moves a failed integer return into `Err` and publishes
`Ok(void)`. `errno` reads thread-local errno and preserves a successful integer
or pointer. `win32` reads `GetLastError`, preserves the successful integer, and
is rejected outside Windows.

The raw provider signature remains the sole C ABI authority. The consumer call
is projected to canonical `Result` only after the Interop artifact has been
resolved and verified. Null predicates require a pointer transport; numeric
predicates require an integer transport. `out` and `inout` remain disallowed.

## Plan-Owned Lowering

`LowerToLowIR` converts the artifact contract into one
`ForeignErrorCallPlan`. The plan fixes the foreign call layout, raw and
projected result types, error type, predicate, extractor, payload policy, and
operation fingerprint. LowIR re-resolves the artifact and checks every field,
the target, the Result shape, and the physical result class before admitting
the instruction.

## Version Closure

CFDL epoch 11, `CHNXIOP8` format 8/schema 7, semantic artifact epoch 19,
Package Artifact v17, `CHNXTPK74` state 71, and cache namespace `next-v43`
replace their previous formats. CFFI config v3, `CHCFFI3`, `CHCFFIS3`,
standard-library epoch 9, Component ABI epoch 1, and runtime ABI v1 are
unchanged.
