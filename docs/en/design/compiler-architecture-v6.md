# Compiler Architecture V6

Status: implemented and enforced for the Chtholly 1.9.2 correctness wave.

Architecture V6 retains V5's sparse ownership solvers and makes aggregate
value representation and concrete specialization identity explicit phase
contracts.

## Aggregate Representation Boundary

SemIR distinguishes an aggregate's value transfer mode from its object layout.
A tuple or array containing an element that requires in-place initialization is
itself Pointer/InPlace; scalar-only aggregates retain Copy/ByCopy. This closure
prevents an inline nominal object from being reinterpreted as an address after
tuple or array projection.

LowIR verifies the resulting representation before LLVM construction. LLVM
constructors return object addresses for Pointer aggregates, tuple and array
projection load through the semantic element representation, and object copy
accepts only address-valued source and destination operands. Invalid operands
produce a compiler error before LLVM intrinsic construction.

## Concrete Specific Identity

Every persisted callee carries its complete concrete callable signature.
Component-local edges additionally carry the target request fingerprint; a
node index is only a canonical ordering check. Public-entity calls preserve
their selected signature even when witness resolution has no explicit type
argument list.

Warm materialization validates component structure, request identity, callable
signatures, call operands and results before creating functions. Cached
specific declarations use the artifact's concrete parameter and return types,
including associated-type substitutions, instead of reconstructing them from
an incomplete template environment. A semantic cache rejection becomes a miss
and local rebuild; I/O errors remain fatal.

## Compatibility Contract

No source grammar, standard-library API, component runtime ABI, or public
runtime symbol changes. The internal generated-module ABI changes for
aggregates containing in-place elements. Concrete components use `CHNXSCC46`
and component fingerprint namespace v18. Semantic artifact epoch 17, compiler
contract 14, and cache namespace `next-v38` reject older facts. Standard-library
epoch 9 and API epoch 16 are unchanged.
