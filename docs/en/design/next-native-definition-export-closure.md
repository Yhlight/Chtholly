# Next Native Definition Export Closure

## Problem

Public interface witness artifacts preserve the canonical function identity of
their implementation target. A provider can therefore publish a witness whose
implementation is source-private but artifact-addressable. Before this closure,
LLVM chose `internal` linkage for that implementation because it was not marked
`pub`; an artifact-only consumer emitted the matching external declaration, but
the provider object could not satisfy it.

Source visibility, artifact addressability, and native link reachability are
separate properties. Treating them as one property loses cross-module witness
bodies.

## Closure Rules

Each source compilation derives a compilation-local native definition export
closure from the final public interface witness projection. A local function is
added only when it is the final function target of a published witness entry and
it is a real non-template, non-evaluator definition. This includes an explicit
conformance method and a concrete local default target when one is materialized.
Imported, forwarded, bodyless, generic-template, evaluator, and unrelated
private functions are not roots.

The closure is not serialized. A provider owns the local function definitions;
an artifact-only consumer reconstructs only canonical external references. The
existing module dependency graph remains the object-level link closure.

## LLVM And Cache Contract

LLVM uses external linkage for a local function when it is source-public, an
existing compiler-owned ABI target, or a member of this native export closure.
All other private functions remain internal. Generic specifics retain their
existing link-once policy.

Changing provider symbol visibility changes object bytes without changing the
language surface or serialized artifact schema. The compile codegen fingerprint
therefore advances to `chtholly.next.codegen.v3`, and the persistent Next cache
namespace advances to `next-v32` so stale provider objects cannot be reused.