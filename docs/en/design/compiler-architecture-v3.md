# Compiler Architecture V3

Status: superseded by `compiler-architecture-v4.md`.

This document supersedes the context-organization and semantic-source layout
sections of the v2 architecture and the 2026 Q3 planning baseline. It changes
no Chtholly source syntax or artifact ABI.

## Semantic Pipeline Ownership

`Semantics.cpp` is a facade. It creates `semantics_internal::SemanticContext`,
runs the check, and attaches source metadata. It owns no checker state and no
source-form implementation.

`SemanticContext.h` declares the per-compilation semantic state. Implementations
are divided by responsibility:

- `SemanticContextCore.cpp`: phase orchestration, types, references, callback
  contracts, and common checker operations;
- `SemanticContextDeclarations.cpp`: nominal/interface declarations, callable
  environments, and concrete declaration materialization;
- `SemanticContextSpecialization.cpp`: generic worklists, concrete components,
  constant control expressions, and operator protocol helpers;
- `SemanticContextExpressions.cpp`: non-call expressions, patterns, and
  foreach lowering;
- `SemanticContextCall.cpp`: free, member, associated, callback, foreign, and
  specialized call resolution;
- `SemanticContextStatements.cpp`: statements, structured loop scopes,
  constants, and callable contract syntax;
- `SemanticContextFunctions.cpp`: callable declarations, implementation
  validation, imported entities, and function bodies.

Phase-specific lowering and evaluation contexts remain separate. A semantic
checker, SemIR-to-LowIR lowering context, LLVM module context, and constant
evaluator do not share mutable state or lifecycle and must not be collapsed
into one universal context.

## Shared Semantic Services

The context delegates reusable policy to narrow services:

- `SemanticLiteral` parses numeric spelling without SemIR mutation;
- `SemanticConversionPlanner` owns contextual literal state and conversion
  ranking, while the checker retains instruction emission authority;
- `SemanticCallResolution` deterministically selects one conversion-ranked
  candidate or reports ambiguity;
- `SemanticNameScopes` owns stable lexical scope identity and isolated lookup;
- `SemanticControlFlow` computes fallthrough and structured infinite-loop
  facts from immutable SemIR.
- `SemanticWitnessResolution` gives interface lookup explicit found, missing,
  ambiguous, cycle, and invalid outcomes. Candidate ordering and member lookup
  are centralized rather than depending on SemIR insertion order.
- `SemanticContextIteration` resolves the canonical standard iterator witness,
  specializes `next`, and validates Item/continuation shape before the
  statement checker emits the shared structured loop expansion.
- `SemanticGenericEnvironment` snapshots generic binding names and normalized
  inherited constraints per function. Generic impl member bodies restore that
  snapshot by FunctionId rather than reconstructing only their local parameter
  list.

All loop bodies enter through `checkLoopBlock` or `checkLoopBlockInto`.
`while`, `for`, `do...while`, and `foreach` therefore share one active-loop
stack lifecycle. Cleanup facts remain owned by PlaceState and are consumed by
lowering rather than reconstructed in these services.

## Stable Store Contract

An ID store that returns references while accepting new values must preserve
those references across append. `CanonicalValueStore` and SemIR's graph entity
stores use deque-backed stable storage. Contiguous `ValueStore` remains valid
only for closed stores or code that does not retain references across append.

This rule was introduced after ASan identified canonical-type and callable-
declaration use-after-free defects during generic Vec compilation. Dense IDs,
canonical hashes, serialization, and artifact identities are unchanged.

## Enforced Boundaries

`scripts/semantic-architecture-audit.py` verifies:

- the facade and all semantic modules exist and are registered in CMake;
- no local `CheckContext` or monolithic checker returns;
- module size ceilings remain below the former 16,000-line implementation;
- call ranking uses the shared selector;
- member and specialization paths use centralized witness resolution;
- loop stack pairing occurs only in the shared loop-body helpers;
- user semantic paths do not emit `UnsupportedSemantics`;
- canonical and SemIR stores retain their reference-stability contract.

The dedicated semantic support test directly covers literals, scope isolation,
candidate ranking, stable canonical references, and structured fallthrough.
The 1.4 stability test adds deterministic cold builds, deterministic
diagnostics, parallel compilation, and container ownership stress.
