# Chtholly Language Charter

Status: normative language direction.

## Product Definition

Chtholly is a component-oriented native systems language for native libraries,
plugins, engines, long-running services, C-facing components, and dynamically
loaded modules. It combines predictable native execution with semantic
interfaces that retain ownership, cleanup, failure, concurrency, and ABI facts
across compilation and deployment boundaries.

Chtholly is not a Rust or C++ dialect. Familiar syntax is retained only when it
fits this model. The language does not prioritize managed-runtime application
coverage or unrestricted C-like memory access in safe code.

## Design Order

Language decisions are evaluated in this order:

1. define one coherent abstract machine;
2. preserve semantic component contracts across compilation boundaries;
3. guarantee deterministic ownership and cleanup in safe code;
4. keep native cost and representation choices inspectable;
5. expose unsafe and foreign assumptions as explicit boundaries;
6. add surface convenience only when the preceding rules remain derivable.

Source syntax expresses decisions a programmer must make: ownership transfer,
copying, mutability, unsafe authority, stable representation, failure, and
external contracts. Compiler-derived proof facts such as borrow duration,
return provenance, access effects, and cleanup paths are not ordinary source
annotations.

The contract abstraction boundary is normative. Binding authors describe
irreducible foreign resource roles; the compiler derives and canonicalizes
proof facts; ordinary users consume safe nominal and structured APIs. Chtholly
does not create a user-programmable contract logic alongside its type system.
See [Contract Abstraction Layers](contract-abstraction-layers.md).

## Compatibility

Persisted artifacts and unpublished compiler-owned ABI revisions are not input
compatibility requirements. Stable C ABI declarations remain governed by the
selected platform ABI. Chtholly component compatibility begins only at an
explicitly published ABI epoch.

## Success Criteria

A feature belongs to Chtholly only when its static semantics, abstract-machine
behavior, component representation, diagnostics, and lowering obligations are
defined coherently. Parser acceptance or a backend experiment is not a
language-support claim.
