# Chtholly Language Specification

This directory is the sole normative authority for the Chtholly language and
its versioned release surfaces.

## Status Model

- `normative`: the source form and semantic obligations are fixed.
- `design-pending`: a future-version capability has no admitted source form
  until its design review is complete.
- `post-v1`: the capability is outside the v1 release line.

The 1.0 through 1.10 manifests are frozen. Their generated surface tables
record normative scope and implementation evidence. Parser tokens and internal
IR operations do not independently admit source syntax. No 1.11 source version
is currently admitted; async channel and richer component-resource behavior
remain design documents until their ownership and ABI contracts are closed.

## Reading Order

1. `language-charter.md`
2. `v1-language-roadmap.md`, followed by the 1.1 through 1.10 roadmaps
3. `abstract-machine-and-memory-model.md`
4. `lexical-and-grammar.md`
5. `expressions-and-control-flow.md`
6. `types-and-values.md`
7. `constant-evaluation-and-layout.md`
8. `declarations-callables-and-generics.md`
9. `interfaces-and-specialization.md`
10. `names-and-scopes.md`
11. `failure-lifecycle-and-storage.md`
12. `modules-programs-unsafe-and-ffi.md`
13. `concurrency-and-atomic-memory.md`
14. `async-and-hosted-program-model.md`
15. `environment-bearing-callables.md`
16. `language-versioning.md`
17. `component-and-ffi-abi.md`
18. `contract-abstraction-layers.md`
19. `cfdl.md`
20. `core-language.md`
21. `language-surface.md`
22. the generated versioned surface tables

## Design Review Contract

A future entry becomes normative only when one review defines its canonical
grammar, static semantics, ownership and cleanup, control-flow order,
cross-module identity, representation and ABI, compiler IR obligations,
diagnostics, tooling behavior, and independent-compilation evidence. The owning
specification and surface manifest must advance together.
