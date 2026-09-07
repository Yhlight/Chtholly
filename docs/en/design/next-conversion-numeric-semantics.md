# Next Conversion And Numeric Semantics

Status: implemented normative core.

## Boundary

This milestone owns numeric literal formation, target-sized scalar identity,
the closed lossless implicit matrix, symmetric common-type selection, explicit
numeric `as`, checked numeric `as?`, and checked integer addition. It does not
open interface downcasts, provenance-changing pointer casts, bit
reinterpretation, user-defined conversion protocols, or the remaining builtin
operator family.

The accepted checked form is:

```cns
import std::result;
import std::convert;

let narrowed: std::result::Result<i8, std::convert::CastError> = value as? i8;
```

`CastError` has exactly `Inexact`, `OutOfRange`, and `NonFinite`. `as?` returns
`Ok` only when the mathematical value survives exactly. `as` is deterministic
and may lose precision; float-to-integer conversion truncates toward zero and
traps for non-finite or out-of-range input.

## Compiler Structure

The lexer validates the full token, including base, separators, exponent, and
suffix, and emits one stable malformed-literal diagnostic. Semantic checking
retains unsuffixed integer magnitude/sign and decimal float value as contextual
facts. A non-mutating conversion query returns a kind and rank; application is
a separate operation. Common-type selection evaluates every operand against
every candidate, minimizes total rank, and rejects ambiguity unless one unique
narrowest viable target exists.

SemIR and LowIR represent float constants, numeric conversion, and checked
numeric casts directly. Generic templates and concrete-specialization
components serialize the same operations rather than replaying source-level
conversion decisions. LLVM receives already-selected source and target types;
it executes conversion, exactness checks, Result construction, and checked-add
trap edges without reclassifying language rules.

## Artifact And ABI Impact

Package state is now `CHNXTPK48` version 48. Concrete-specialization components
are now `CHNXSCC32` version 30. Old readers fail closed. The hosted runtime ABI adds no
value lane; integer overflow calls
`chtholly_next_runtime_v1_trap_arithmetic(7)`, while
invalid trapping casts retain reason 5. `Result` and `CastError` use their
ordinary verified nominal layouts.

Hexadecimal floating literals, wrapping and saturating APIs, interface casts,
pointer reinterpretation, and operator overloading are intentionally deferred
to their roadmap owners. Arithmetic edge cases are closed by the subsequent
builtin-operator milestone.
