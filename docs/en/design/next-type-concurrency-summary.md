# Type concurrency summary

Status: compiler-internal implementation (2026-09-01).

`TypeConcurrencySummary` is the single structural query for the persisted
`transferable`/`shareable` facts (the source-facing Send/Sync capabilities).
It is keyed by canonical public type bytes rather than session-local `TypeId`s
and is reused while building nominal semantic witnesses.

Scalar integer, floating, Boolean, and character values are transferable and
shareable. Arrays, tuples, and nominal fields are conjunctions of their child
facts. Borrowed references, slices, raw pointers, function values, strings
without an owning witness, dependent types, and unknown foreign identities fail
closed. A foreign resolver may provide facts only from a verified CFDL witness.

The query tracks active canonical keys and bounds work items. Recursive or
malformed graphs therefore produce non-capability facts (or a bounded-worklist
error) instead of recursing indefinitely or inventing a bytewise witness.
Future typed channel checking must call this service and include its result and
dependency fingerprints in the channel descriptor.
