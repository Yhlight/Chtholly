# Compiler Architecture V14: Structured CFFI Regeneration

Status: implemented by the 1.9.10 CFFI regeneration wave.

## Mechanical Model

The CFFI frontend separates session-local canonical Clang cursors from stable
`(kind, CFDL name)` declaration keys. Redeclarations collapse through the
canonical cursor; distinct entities with one key are rejected instead of
depending on traversal order. A bounded dependency worklist completes types
required by configured roots before publishing the deterministic mechanical
model.

## Three-Way Merge

`CHCFFIS1` stores the prior canonical mechanical CFDL and verified target,
module, config, payload size, and digests. Regeneration compares this base with
the current human source and the new Clang model. CFDL declaration source
ranges permit managed declarations to be replaced without rewriting imports,
manual declarations, or surrounding comments.

Flow qualifiers, `where` facts, and invalid sentinels are semantic overlays.
They survive compatible mechanical updates. Parameter renames are admitted
only at the same ordinal with the same physical lane and rewrite place facts.
Ambiguous edits, resource-bearing lane changes, and deletion with live overlays
fail closed.

Dry-run is the default and performs no writes. `--write` rechecks source/state
digests, stages validated outputs, replaces CFDL first, and then replaces the
state. If the second replacement is interrupted, the next invocation may
recover only when the current complete managed key set matches the new model.

## Version Boundary

`CHCFFIS1` is standalone tooling state. `CHCFFI1`, Package Artifact v14,
`CHNXTPK71`, cache namespace `next-v40`, CFDL epoch 9, semantic artifact epoch
17, Component ABI epoch 1, and runtime ABI v1 are unchanged. No Chtholly or
CFDL syntax is added.

Architecture V15 replaces `CHCFFIS1` with toolchain-aware `CHCFFIS2`; the
three-way merge semantics defined here are unchanged.
