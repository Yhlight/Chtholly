# Compiler Architecture V11: Stable C Callable Facts

Status: implemented by the 1.9.7 CFDL epoch-9 wave.

## Boundary

CFDL now separates four callable facts: source lookup identity, exact external
symbol, target-neutral signature, and normalized calling convention. Public
artifacts and package link closures persist all four; LLVM consumes the single
verified LowIR layout and never guesses them from a source name.

C enums retain nominal identity plus an integer carrier and publish open typed
constants. C unions retain nominal identity plus an overlapping structural
carrier. Neither representation exposes fields or closed variants to Chtholly.

## Closure

Package link requirements contain logical name, external symbol, convention,
and signature fingerprint. Conflicting requirements for one external symbol
are rejected across local modules and direct dependency closure.

Version closure: CFDL epoch 9, `CHNXIOP6` schema 5, Package Artifact v12,
foreign layout epoch 11, nominal layout epoch 9, `CHNXTPK70` state version 67,
and cache namespace `next-v39`. Component ABI and runtime ABI remain epoch 1
and v1.
