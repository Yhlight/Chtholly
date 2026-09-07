# Compiler Architecture V19: CFDL Outcomes And Wide Slices

Status: implemented by the Chtholly 1.10 Tier-1 CFFI wave.

## Source Boundary

CFDL now names ABI categories directly with `foreign struct`, `foreign union`,
and `foreign enum`; incomplete or scalar foreign identities retain `foreign
type`, with `:` introducing a carrier and `invalid` owned by that type.
Function clauses are independent and order-insensitive. Rendering is canonical:
`link`, `call`, `outcome`, `error`, then `where`. Duplicate clauses fail.
Error and outcome semantics are not resource relations and therefore no longer
occupy the `where` relation list.

Regeneration continues to own only mechanical declarations. It preserves
function-clause overlays and updates outcome buffer/capacity references when a
compatible C parameter rename is proven. `CHCFFIS4` records the new canonical
mechanical baseline; obsolete source spellings and state are not accepted.

## POSIX Read Projection

The bounded `posix_read<u8>(buffer, capacity)` contract is admitted only for
Linux, language 1.10, a mutable raw-pointer view, equal-width unsigned capacity
and signed result lanes, and the exact errno sentinel `-1`. The native function
signature is unchanged. Consumers see
`Result<ReadOutcome<slice<u8>>, i32>`.

The raw count is checked before construction. Counts beyond capacity,
unexpected negative counts, positive counts with null storage, and capacities
outside the signed result domain trap. A positive or zero-capacity result forms
a Data slice whose provenance remains tied to the buffer lane; a nonzero
capacity with zero result forms Eof. The Data slice is the sole initialized
prefix authority.

## Verified Lowering

`ForeignCallOutcomePlan` owns raw and projected result types, the normalized
error predicate/extractor, outcome kind, buffer and capacity lanes, element and
slice types, and Data/Eof variant identities. LowIR validates this complete
plan before LLVM. LLVM performs only checked branching and object construction;
it does not reopen CFDL or Interop artifacts.

Language 1.10 makes slice lengths and dynamic bounds `u64`, matching the
physical pointer-plus-i64 representation. Array and slice indexing now trap
before any inbounds GEP. Languages 1.0 through 1.9 retain frozen i32 semantics.
Generic instructions imported from another source version retain their checked
integer types; SemIR, LowIR, and LLVM validate and lower those types directly
instead of reinterpreting them through the consumer package version.

## Version Closure

CFDL epoch 13, `CHNXIOP10` format 10/schema 9, semantic artifact epoch 21,
standard-library epoch 10, Package Artifact v19, `CHNXTPK76` state 73, cache
namespace `next-v45`, foreign operation fingerprint v7, CFFI state `CHCFFIS4`,
and concrete specialization `CHNXSCC49` replace their predecessors. CFFI config
v3, receipt `CHCFFI3`, Component ABI epoch 1, runtime ABI v1, and native C ABI
are unchanged.
